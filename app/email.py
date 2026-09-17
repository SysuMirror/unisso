"""Generic MIME email delivery with an injectable async transport for tests."""
from dataclasses import dataclass
from email.message import EmailMessage
from email.utils import formataddr
from typing import Awaitable, Callable, Optional

import aiosmtplib

from app.config import Settings, get_settings


@dataclass(frozen=True)
class OutgoingEmail:
    subject: str
    recipients: tuple[str, ...]
    text: str
    html: str


class MailDeliveryError(RuntimeError):
    pass


async def send_email(
    message: OutgoingEmail,
    *,
    settings: Optional[Settings] = None,
    transport: Optional[Callable[[EmailMessage, object], Awaitable[None]]] = None,
) -> None:
    """Deliver one MIME message. Callers own rate limiting and secret redaction."""
    config = settings or get_settings()
    if not config.smtp_ready:
        raise MailDeliveryError('SMTP is not configured')
    if not message.recipients:
        raise MailDeliveryError('SMTP recipients are required')

    email = EmailMessage()
    email['From'] = formataddr((config.smtp_from_name, config.smtp_from))
    email['To'] = ', '.join(recipient.lower() for recipient in message.recipients)
    email['Subject'] = message.subject
    email.set_content(message.text)
    email.add_alternative(message.html, subtype='html')

    if transport is not None:
        try:
            await transport(email, config)
        except MailDeliveryError:
            raise
        except Exception as exc:
            raise MailDeliveryError('SMTP delivery failed') from exc
        return

    try:
        await aiosmtplib.send(
            email,
            hostname=config.smtp_host,
            port=config.smtp_port,
            username=config.smtp_username,
            password=config.smtp_password,
            use_tls=config.smtp_use_tls,
            start_tls=config.smtp_starttls,
            timeout=config.smtp_timeout_seconds,
        )
    except Exception as exc:
        raise MailDeliveryError('SMTP delivery failed') from exc
