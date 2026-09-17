"""Explicit maintenance: python -m app.maintenance migrate|bootstrap-admin.

Credentials come from the process environment, never command arguments.
Bootstrap never resets an existing account or changes its privileges.
"""
import argparse
import asyncio
from alembic.config import Config
from alembic import command
from sqlalchemy import select

async def bootstrap_admin():
    from app.config import get_settings
    from app.database import async_session_maker, engine
    from app.models import User, Role
    from app.auth import create_user, init_default_data
    from app.schemas import UserCreate
    s = get_settings()
    if not s.admin_email or not s.admin_password:
        raise RuntimeError('Set administrator email/password in the maintenance environment')
    try:
        async with async_session_maker() as db:
            exists = await db.scalar(select(User.id).where(User.email == s.admin_email))
            if exists:
                raise RuntimeError('Account already exists; no password or privilege changes made')
            # Explicitly initialize metadata, then provision an admin even if normal users exist.
            await init_default_data(db, create_admin=False)
            user = await create_user(db, UserCreate(email=s.admin_email, password=s.admin_password,
                username=s.admin_username or None), is_admin=True)
            # A provisioned address is not automatically proof of mailbox ownership.
            role = await db.scalar(select(Role).where(Role.name == 'admin'))
            user.roles = [role] if role else []
            await db.commit()
    finally:
        await engine.dispose()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['migrate', 'bootstrap-admin'])
    args = parser.parse_args()
    if args.action == 'migrate':
        command.upgrade(Config('alembic.ini'), 'head')
    else:
        asyncio.run(bootstrap_admin())

if __name__ == '__main__':
    main()
