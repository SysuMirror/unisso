"""init

Revision ID: 001
Revises:
Create Date: 2024-08-22 00:00:00

"""
from typing import Sequence, Union

from alembic import op
import sqlalchemy as sa


# revision identifiers, used by Alembic.
revision: str = "001"
down_revision: Union[str, None] = None
branch_labels: Union[str, Sequence[str], None] = None
depends_on: Union[str, Sequence[str], None] = None


def upgrade() -> None:
    # 用户表
    op.create_table(
        "users",
        sa.Column("id", sa.String(36), primary_key=True),
        sa.Column("username", sa.String(64), unique=True, index=True, nullable=False),
        sa.Column("password_hash", sa.String(255), nullable=False),
        sa.Column("email", sa.String(128), unique=True, index=True, nullable=True),
        sa.Column("full_name", sa.String(128), nullable=True),
        sa.Column("avatar", sa.String(255), nullable=True),
        sa.Column("is_active", sa.Boolean, default=True, nullable=False),
        sa.Column("is_admin", sa.Boolean, default=False, nullable=False),
        sa.Column("created_at", sa.DateTime, server_default=sa.func.now(), nullable=False),
        sa.Column("updated_at", sa.DateTime, server_default=sa.func.now(), onupdate=sa.func.now(), nullable=False),
        sa.Column("last_login_at", sa.DateTime, nullable=True),
    )

    # 外部身份绑定表
    op.create_table(
        "user_identities",
        sa.Column("id", sa.String(36), primary_key=True),
        sa.Column("user_id", sa.String(36), sa.ForeignKey("users.id", ondelete="CASCADE"), nullable=False),
        sa.Column("provider", sa.String(64), nullable=False, index=True),
        sa.Column("provider_user_id", sa.String(255), nullable=False),
        sa.Column("provider_username", sa.String(128), nullable=True),
        sa.Column("provider_email", sa.String(128), nullable=True),
        sa.Column("provider_avatar", sa.String(255), nullable=True),
        sa.Column("access_token", sa.Text, nullable=True),
        sa.Column("refresh_token", sa.Text, nullable=True),
        sa.Column("extra_data", sa.JSON, nullable=True),
        sa.Column("is_active", sa.Boolean, default=True, nullable=False),
        sa.Column("created_at", sa.DateTime, server_default=sa.func.now(), nullable=False),
        sa.Column("updated_at", sa.DateTime, server_default=sa.func.now(), onupdate=sa.func.now(), nullable=False),
    )

    # 角色表
    op.create_table(
        "roles",
        sa.Column("id", sa.String(36), primary_key=True),
        sa.Column("name", sa.String(64), unique=True, index=True, nullable=False),
        sa.Column("description", sa.Text, nullable=True),
        sa.Column("is_system", sa.Boolean, default=False, nullable=False),
        sa.Column("created_at", sa.DateTime, server_default=sa.func.now(), nullable=False),
    )

    # 权限表
    op.create_table(
        "permissions",
        sa.Column("id", sa.String(36), primary_key=True),
        sa.Column("name", sa.String(64), unique=True, index=True, nullable=False),
        sa.Column("resource", sa.String(64), nullable=False),
        sa.Column("action", sa.String(64), nullable=False),
        sa.Column("description", sa.Text, nullable=True),
        sa.Column("created_at", sa.DateTime, server_default=sa.func.now(), nullable=False),
    )

    # 用户角色关联
    op.create_table(
        "user_roles",
        sa.Column("user_id", sa.String(36), sa.ForeignKey("users.id", ondelete="CASCADE"), primary_key=True),
        sa.Column("role_id", sa.String(36), sa.ForeignKey("roles.id", ondelete="CASCADE"), primary_key=True),
    )

    # 角色权限关联
    op.create_table(
        "role_permissions",
        sa.Column("role_id", sa.String(36), sa.ForeignKey("roles.id", ondelete="CASCADE"), primary_key=True),
        sa.Column("permission_id", sa.String(36), sa.ForeignKey("permissions.id", ondelete="CASCADE"), primary_key=True),
    )

    # 应用表
    op.create_table(
        "applications",
        sa.Column("id", sa.String(36), primary_key=True),
        sa.Column("client_id", sa.String(64), unique=True, index=True, nullable=False),
        sa.Column("client_secret", sa.String(255), nullable=False),
        sa.Column("name", sa.String(128), nullable=False),
        sa.Column("description", sa.Text, nullable=True),
        sa.Column("icon", sa.String(255), nullable=True),
        sa.Column("redirect_uris", sa.JSON, default=list, nullable=False),
        sa.Column("allowed_scopes", sa.JSON, default=list, nullable=False),
        sa.Column("homepage_url", sa.String(255), nullable=True),
        sa.Column("callback_url", sa.String(255), nullable=True),
        sa.Column("is_active", sa.Boolean, default=True, nullable=False),
        sa.Column("is_confidential", sa.Boolean, default=True, nullable=False),
        sa.Column("owner_id", sa.String(36), sa.ForeignKey("users.id"), nullable=True),
        sa.Column("created_at", sa.DateTime, server_default=sa.func.now(), nullable=False),
        sa.Column("updated_at", sa.DateTime, server_default=sa.func.now(), onupdate=sa.func.now(), nullable=False),
    )

    # 用户授权记录
    op.create_table(
        "user_consents",
        sa.Column("id", sa.String(36), primary_key=True),
        sa.Column("user_id", sa.String(36), sa.ForeignKey("users.id", ondelete="CASCADE"), nullable=False),
        sa.Column("application_id", sa.String(36), sa.ForeignKey("applications.id", ondelete="CASCADE"), nullable=False),
        sa.Column("scopes", sa.JSON, default=list, nullable=False),
        sa.Column("granted_at", sa.DateTime, server_default=sa.func.now(), nullable=False),
        sa.Column("revoked_at", sa.DateTime, nullable=True),
        sa.Column("is_active", sa.Boolean, default=True, nullable=False),
    )

    # 授权码表（备用）
    op.create_table(
        "authorization_codes",
        sa.Column("id", sa.String(36), primary_key=True),
        sa.Column("code", sa.String(128), unique=True, index=True, nullable=False),
        sa.Column("client_id", sa.String(64), nullable=False),
        sa.Column("user_id", sa.String(36), nullable=False),
        sa.Column("redirect_uri", sa.String(255), nullable=False),
        sa.Column("scope", sa.Text, default="", nullable=False),
        sa.Column("code_challenge", sa.String(255), nullable=True),
        sa.Column("code_challenge_method", sa.String(10), nullable=True),
        sa.Column("expires_at", sa.DateTime, nullable=False),
        sa.Column("used", sa.Boolean, default=False, nullable=False),
        sa.Column("created_at", sa.DateTime, server_default=sa.func.now(), nullable=False),
    )

    # 访问令牌表（备用）
    op.create_table(
        "access_tokens",
        sa.Column("id", sa.String(36), primary_key=True),
        sa.Column("token", sa.String(512), unique=True, index=True, nullable=False),
        sa.Column("client_id", sa.String(64), nullable=False),
        sa.Column("user_id", sa.String(36), nullable=True),
        sa.Column("scope", sa.Text, default="", nullable=False),
        sa.Column("expires_at", sa.DateTime, nullable=False),
        sa.Column("revoked", sa.Boolean, default=False, nullable=False),
        sa.Column("created_at", sa.DateTime, server_default=sa.func.now(), nullable=False),
    )

    # 刷新令牌表
    op.create_table(
        "refresh_tokens",
        sa.Column("id", sa.String(36), primary_key=True),
        sa.Column("token", sa.String(512), unique=True, index=True, nullable=False),
        sa.Column("access_token_id", sa.String(36), nullable=True),
        sa.Column("client_id", sa.String(64), nullable=False),
        sa.Column("user_id", sa.String(36), nullable=True),
        sa.Column("scope", sa.Text, default="", nullable=False),
        sa.Column("expires_at", sa.DateTime, nullable=False),
        sa.Column("revoked", sa.Boolean, default=False, nullable=False),
        sa.Column("created_at", sa.DateTime, server_default=sa.func.now(), nullable=False),
    )


def downgrade() -> None:
    op.drop_table("refresh_tokens")
    op.drop_table("access_tokens")
    op.drop_table("authorization_codes")
    op.drop_table("user_consents")
    op.drop_table("user_identities")
    op.drop_table("applications")
    op.drop_table("role_permissions")
    op.drop_table("user_roles")
    op.drop_table("permissions")
    op.drop_table("roles")
    op.drop_table("users")
