"""Email ownership proof before any database account is created."""
import hashlib
import hmac
import secrets
import time

from fastapi import HTTPException
from pydantic import BaseModel, EmailStr, Field
from sqlalchemy.exc import IntegrityError
from app.auth import create_verified_user_with_password_hash, get_user_by_email
from app.config import get_settings
from app.email import MailDeliveryError, OutgoingEmail, send_email
from app.redis_client import StateUnavailable, redis_client
from app.schemas import RegistrationStartRequest
from app.security import hash_password, validate_password_strength


class PendingRegistrationError(HTTPException):
    pass


class RegistrationRateLimited(HTTPException):
    pass


class EmailDeliveryUnavailable(HTTPException):
    pass


class InvalidRegistrationCode(Exception):
    pass


class RegistrationConflict(Exception):
    pass


class RegistrationVerifyRequest(BaseModel):
    email: EmailStr
    code: str = Field(..., min_length=6, max_length=6, pattern=r"^[0-9]{6}$")


def pending_key(email: str) -> str:
    digest = hashlib.sha256(email.strip().lower().encode('utf-8')).hexdigest()
    return f'registration:pending:{digest}'


def _pending_counter_key(email: str, kind: str) -> str:
    return f'{pending_key(email)}:{kind}'


def _count_key(kind: str, value: str) -> str:
    digest = hashlib.sha256(value.encode('utf-8')).hexdigest()
    return f'registration:count:{kind}:{digest}'


def code_digest(code: str, settings=None) -> str:
    config = settings or get_settings()
    return hmac.new(
        config.secret_key.encode('utf-8'), code.encode('ascii'), hashlib.sha256
    ).hexdigest()


def generate_verification_code() -> str:
    return f'{secrets.randbelow(1_000_000):06d}'


def _new_record(email, password_hash, username, full_name, code, sends, settings):
    now = int(time.time())
    return {
        'email': email,
        'password_hash': password_hash,
        'username': username,
        'full_name': full_name,
        'code_hash': code_digest(code, settings),
        'attempts': 0,
        'sends': sends,
        'created_at': now,
        'expires_at': now + settings.registration_pending_ttl_seconds,
    }


async def _consume_send_slots(email, client_ip, settings):
    send_count = await redis_client.incr(
        _pending_counter_key(email, 'sends'),
        settings.registration_pending_ttl_seconds * settings.registration_max_sends,
    )
    if send_count > settings.registration_max_sends:
        raise RegistrationRateLimited(status_code=429, detail='registration_rate_limited')

    email_count = await redis_client.incr(
        _count_key('email', email), 3600
    )
    ip_count = await redis_client.incr(
        _count_key('ip', client_ip), 3600
    )
    if (
        email_count > settings.registration_email_hourly_limit
        or ip_count > settings.registration_ip_hourly_limit
    ):
        raise RegistrationRateLimited(status_code=429, detail='registration_rate_limited')

    return send_count


async def _send_code(email, code, settings):
    try:
        await send_registration_email(email, code, settings)
    except MailDeliveryError as exc:
        raise EmailDeliveryUnavailable(
            status_code=503, detail='registration_email_unavailable'
        ) from exc


async def _consume_invalid_attempt(email, expires_at, settings):
    remaining = max(1, int(expires_at) - int(time.time()))
    attempts = await redis_client.incr(
        _pending_counter_key(email, 'attempts'), remaining
    )
    if attempts >= settings.registration_max_attempts:
        await redis_client.getdel_json(pending_key(email))
        return

    record = await redis_client.getdel_json(pending_key(email))
    if record is not None:
        latest_attempts = await redis_client.get(
            _pending_counter_key(email, 'attempts')
        )
        if latest_attempts is not None and int(latest_attempts) >= (
            settings.registration_max_attempts
        ):
            return
        await redis_client.set_json(pending_key(email), record, expire=remaining)


async def _restore_pending_registration(record):
    """Restore an atomically consumed record after a transient persistence failure."""
    remaining = max(1, int(record['expires_at']) - int(time.time()))
    if await redis_client.get_json(pending_key(record['email'])) is None:
        await redis_client.set_json(pending_key(record['email']), record, expire=remaining)


async def send_registration_email(email, code, settings=None):
    config = settings or get_settings()
    escaped = email.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')
    await send_email(
        OutgoingEmail(
            subject='UniSSO mailbox verification code',
            recipients=(email,),
            text=f'Your UniSSO verification code is {code}. It expires in 15 minutes.',
            html=(
                '<p>Your UniSSO verification code is '
                f'<strong>{code}</strong>. It expires in 15 minutes.</p>'
                f'<p style="color:#64748b">This message was requested for {escaped}.</p>'
            ),
        ),
        settings=config,
    )


async def create_pending_registration(db, request, client_ip, settings=None):
    """Store a bcrypt hash and send a code; an existing account returns the same normalized value."""
    config = settings or get_settings()
    if not config.email_registration_available:
        raise PendingRegistrationError(
            status_code=403, detail='registration_unavailable_pending_email_verification'
        )
    try:
        validate_password_strength(request.password)
    except ValueError as exc:
        raise PendingRegistrationError(status_code=400, detail='registration_input_invalid') from exc

    email = request.email.strip().lower()
    username = request.username.strip() if request.username else None
    full_name = request.full_name.strip() if request.full_name else None
    if await get_user_by_email(db, email):
        return email

    try:
        old = await redis_client.get_json(pending_key(email))
        code = generate_verification_code()
        send_count = await _consume_send_slots(email, client_ip, config)
        record = _new_record(
            email, hash_password(request.password), username, full_name, code, send_count, config
        )
        await redis_client.set_json(
            pending_key(email), record, expire=config.registration_pending_ttl_seconds
        )
        await _send_code(email, code, config)
        return email
    except StateUnavailable:
        raise PendingRegistrationError(
            status_code=503, detail='authentication_state_unavailable'
        )


async def resend_pending_registration(db, email, client_ip, settings=None):
    """Replace a pending code without revealing whether an account already exists."""
    config = settings or get_settings()
    if not config.email_registration_available:
        raise PendingRegistrationError(
            status_code=403, detail='registration_unavailable_pending_email_verification'
        )
    email = email.strip().lower()
    try:
        if await get_user_by_email(db, email):
            return email
        old = await redis_client.get_json(pending_key(email))
        if old is None:
            return email
        code = generate_verification_code()
        send_count = await _consume_send_slots(email, client_ip, config)
        old.update({
            'code_hash': code_digest(code, config),
            'sends': send_count,
            'created_at': int(time.time()),
            'expires_at': int(time.time()) + config.registration_pending_ttl_seconds,
        })
        await redis_client.set_json(
            pending_key(email), old, expire=config.registration_pending_ttl_seconds
        )
        await _send_code(email, code, config)
        return email
    except StateUnavailable:
        raise PendingRegistrationError(
            status_code=503, detail='authentication_state_unavailable'
        )


async def consume_verified_registration(db, email, code, settings=None, password=None):
    """Atomically remove the record first; restore it only when a bad attempt remains allowed."""
    config = settings or get_settings()
    email = email.strip().lower()
    try:
        snapshot = await redis_client.get_json(pending_key(email))
        if snapshot is None:
            raise InvalidRegistrationCode()

        password_hash = snapshot.get('password_hash')
        if password is not None:
            try:
                validate_password_strength(password)
            except ValueError as exc:
                raise PendingRegistrationError(
                    status_code=400, detail='registration_input_invalid'
                ) from exc
            password_hash = hash_password(password)

        if not hmac.compare_digest(
            str(snapshot.get('code_hash', '')), code_digest(code, config)
        ):
            await _consume_invalid_attempt(
                email, snapshot.get('expires_at', 0), config
            )
            raise InvalidRegistrationCode()

        record = await redis_client.getdel_json(pending_key(email))
        if record is None or not hmac.compare_digest(
            str(record.get('code_hash', '')), code_digest(code, config)
        ):
            if record is not None:
                await _consume_invalid_attempt(
                    email, record.get('expires_at', 0), config
                )
            raise InvalidRegistrationCode()

        try:
            if await get_user_by_email(db, record['email']):
                raise RegistrationConflict()
            user = await create_verified_user_with_password_hash(
                db,
                record['email'],
                password_hash,
                record.get('username'),
                record.get('full_name'),
            )
            await db.commit()
            return user
        except RegistrationConflict:
            await db.rollback()
            raise
        except IntegrityError as exc:
            await db.rollback()
            raise RegistrationConflict() from exc
        except Exception as exc:
            await db.rollback()
            try:
                await _restore_pending_registration(record)
            except StateUnavailable as restore_exc:
                raise PendingRegistrationError(
                    status_code=503, detail='authentication_state_unavailable'
                ) from restore_exc
            raise PendingRegistrationError(
                status_code=503, detail='registration_persistence_unavailable'
            ) from exc
    except StateUnavailable:
        raise PendingRegistrationError(
            status_code=503, detail='authentication_state_unavailable'
        )
