"""UniSSO 数据库模型

主身份：sysu 邮箱 (*.mail*.sysu.edu.cn)
可绑定：集市账号、其他应用账号（通过 UserIdentity）
"""
import uuid
import re
from datetime import datetime
from typing import Optional, List

from sqlalchemy import (
    String, Boolean, DateTime, Text, Integer, ForeignKey, Table, Column, JSON
)
from sqlalchemy.orm import DeclarativeBase, Mapped, mapped_column, relationship, validates
from sqlalchemy.sql import func


class Base(DeclarativeBase):
    pass


# 多对多关联表
user_roles = Table(
    "user_roles",
    Base.metadata,
    Column("user_id", String(36), ForeignKey("users.id", ondelete="CASCADE"), primary_key=True),
    Column("role_id", String(36), ForeignKey("roles.id", ondelete="CASCADE"), primary_key=True),
)

role_permissions = Table(
    "role_permissions",
    Base.metadata,
    Column("role_id", String(36), ForeignKey("roles.id", ondelete="CASCADE"), primary_key=True),
    Column("permission_id", String(36), ForeignKey("permissions.id", ondelete="CASCADE"), primary_key=True),
)


# Sysu 邮箱正则
SYSU_EMAIL_RE = re.compile(r"^[a-zA-Z0-9._%+-]+@mail\d*\.sysu\.edu\.cn$")


class User(Base):
    """用户表 - 主身份为 sysu 邮箱

    注册时必须使用 *.mail*.sysu.edu.cn 邮箱。
    username 为可选的显示昵称，不用于登录。
    """
    __tablename__ = "users"

    id: Mapped[str] = mapped_column(String(36), primary_key=True, default=lambda: str(uuid.uuid4()))
    # 主身份：sysu 邮箱（唯一、必填）
    email: Mapped[str] = mapped_column(String(128), unique=True, index=True, nullable=False)
    email_verified: Mapped[bool] = mapped_column(Boolean, default=False, nullable=False)
    password_hash: Mapped[str] = mapped_column(String(255), nullable=False)
    # 显示昵称（可选，不用于登录）
    username: Mapped[Optional[str]] = mapped_column(String(64), unique=True, index=True, nullable=True)
    full_name: Mapped[Optional[str]] = mapped_column(String(128), nullable=True)
    avatar: Mapped[Optional[str]] = mapped_column(String(255), nullable=True)
    is_active: Mapped[bool] = mapped_column(Boolean, default=True, nullable=False)
    is_admin: Mapped[bool] = mapped_column(Boolean, default=False, nullable=False)
    created_at: Mapped[datetime] = mapped_column(DateTime, server_default=func.now(), nullable=False)
    updated_at: Mapped[datetime] = mapped_column(
        DateTime, server_default=func.now(), onupdate=func.now(), nullable=False
    )
    last_login_at: Mapped[Optional[datetime]] = mapped_column(DateTime, nullable=True)

    # 关系
    roles: Mapped[List["Role"]] = relationship("Role", secondary=user_roles, back_populates="users")
    consents: Mapped[List["UserConsent"]] = relationship("UserConsent", back_populates="user", cascade="all, delete-orphan")
    owned_apps: Mapped[List["Application"]] = relationship("Application", back_populates="owner")
    identities: Mapped[List["UserIdentity"]] = relationship("UserIdentity", back_populates="user", cascade="all, delete-orphan")

    @validates("email")
    def validate_email(self, key, email):
        if email and not SYSU_EMAIL_RE.match(email):
            raise ValueError(f"邮箱必须是中山大学邮箱格式: *.mail*.sysu.edu.cn, 收到: {email}")
        return email

    def __repr__(self) -> str:
        return f"<User {self.email}>"


class UserIdentity(Base):
    """外部身份绑定表

    用户可绑定多个外部身份：
    - provider="ssemarket"  → 集市账号
    - provider="github"     → GitHub
    - provider="wechat"     → 微信
    - provider="custom:xxx" → 其他自定义应用
    """
    __tablename__ = "user_identities"

    id: Mapped[str] = mapped_column(String(36), primary_key=True, default=lambda: str(uuid.uuid4()))
    user_id: Mapped[str] = mapped_column(String(36), ForeignKey("users.id", ondelete="CASCADE"), nullable=False)
    provider: Mapped[str] = mapped_column(String(64), nullable=False, index=True)  # ssemarket, github, wechat...
    provider_user_id: Mapped[str] = mapped_column(String(255), nullable=False)      # 外部系统用户ID
    provider_username: Mapped[Optional[str]] = mapped_column(String(128), nullable=True)  # 外部系统用户名
    provider_email: Mapped[Optional[str]] = mapped_column(String(128), nullable=True)
    provider_avatar: Mapped[Optional[str]] = mapped_column(String(255), nullable=True)
    access_token: Mapped[Optional[str]] = mapped_column(Text, nullable=True)        # 外部系统 access_token（加密存储）
    refresh_token: Mapped[Optional[str]] = mapped_column(Text, nullable=True)       # 外部系统 refresh_token
    extra_data: Mapped[Optional[dict]] = mapped_column(JSON, nullable=True)         # 额外元数据
    is_active: Mapped[bool] = mapped_column(Boolean, default=True, nullable=False)
    created_at: Mapped[datetime] = mapped_column(DateTime, server_default=func.now(), nullable=False)
    updated_at: Mapped[datetime] = mapped_column(
        DateTime, server_default=func.now(), onupdate=func.now(), nullable=False
    )

    # 关系
    user: Mapped["User"] = relationship("User", back_populates="identities")

    def __repr__(self) -> str:
        return f"<UserIdentity {self.provider}:{self.provider_user_id}>"


class Role(Base):
    """角色表"""
    __tablename__ = "roles"

    id: Mapped[str] = mapped_column(String(36), primary_key=True, default=lambda: str(uuid.uuid4()))
    name: Mapped[str] = mapped_column(String(64), unique=True, index=True, nullable=False)
    description: Mapped[Optional[str]] = mapped_column(Text, nullable=True)
    is_system: Mapped[bool] = mapped_column(Boolean, default=False, nullable=False)
    created_at: Mapped[datetime] = mapped_column(DateTime, server_default=func.now(), nullable=False)

    users: Mapped[List["User"]] = relationship("User", secondary=user_roles, back_populates="roles")
    permissions: Mapped[List["Permission"]] = relationship("Permission", secondary=role_permissions, back_populates="roles")

    def __repr__(self) -> str:
        return f"<Role {self.name}>"


class Permission(Base):
    """权限表"""
    __tablename__ = "permissions"

    id: Mapped[str] = mapped_column(String(36), primary_key=True, default=lambda: str(uuid.uuid4()))
    name: Mapped[str] = mapped_column(String(64), unique=True, index=True, nullable=False)
    resource: Mapped[str] = mapped_column(String(64), nullable=False)
    action: Mapped[str] = mapped_column(String(64), nullable=False)
    description: Mapped[Optional[str]] = mapped_column(Text, nullable=True)
    created_at: Mapped[datetime] = mapped_column(DateTime, server_default=func.now(), nullable=False)

    roles: Mapped[List["Role"]] = relationship("Role", secondary=role_permissions, back_populates="permissions")

    def __repr__(self) -> str:
        return f"<Permission {self.resource}:{self.action}>"


class Application(Base):
    """应用注册表 - OAuth2 客户端"""
    __tablename__ = "applications"

    id: Mapped[str] = mapped_column(String(36), primary_key=True, default=lambda: str(uuid.uuid4()))
    client_id: Mapped[str] = mapped_column(String(64), unique=True, index=True, nullable=False)
    client_secret: Mapped[str] = mapped_column(String(255), nullable=False)
    name: Mapped[str] = mapped_column(String(128), nullable=False)
    description: Mapped[Optional[str]] = mapped_column(Text, nullable=True)
    icon: Mapped[Optional[str]] = mapped_column(String(255), nullable=True)
    redirect_uris: Mapped[List[str]] = mapped_column(JSON, default=list, nullable=False)
    allowed_scopes: Mapped[List[str]] = mapped_column(JSON, default=list, nullable=False)
    homepage_url: Mapped[Optional[str]] = mapped_column(String(255), nullable=True)
    callback_url: Mapped[Optional[str]] = mapped_column(String(255), nullable=True)
    is_active: Mapped[bool] = mapped_column(Boolean, default=True, nullable=False)
    is_confidential: Mapped[bool] = mapped_column(Boolean, default=True, nullable=False)
    owner_id: Mapped[Optional[str]] = mapped_column(String(36), ForeignKey("users.id"), nullable=True)
    created_at: Mapped[datetime] = mapped_column(DateTime, server_default=func.now(), nullable=False)
    updated_at: Mapped[datetime] = mapped_column(
        DateTime, server_default=func.now(), onupdate=func.now(), nullable=False
    )

    owner: Mapped[Optional["User"]] = relationship("User", back_populates="owned_apps")
    consents: Mapped[List["UserConsent"]] = relationship("UserConsent", back_populates="application", cascade="all, delete-orphan")

    def __repr__(self) -> str:
        return f"<Application {self.name}>"


class UserConsent(Base):
    """用户授权记录"""
    __tablename__ = "user_consents"

    id: Mapped[str] = mapped_column(String(36), primary_key=True, default=lambda: str(uuid.uuid4()))
    user_id: Mapped[str] = mapped_column(String(36), ForeignKey("users.id", ondelete="CASCADE"), nullable=False)
    application_id: Mapped[str] = mapped_column(String(36), ForeignKey("applications.id", ondelete="CASCADE"), nullable=False)
    scopes: Mapped[List[str]] = mapped_column(JSON, default=list, nullable=False)
    granted_at: Mapped[datetime] = mapped_column(DateTime, server_default=func.now(), nullable=False)
    revoked_at: Mapped[Optional[datetime]] = mapped_column(DateTime, nullable=True)
    is_active: Mapped[bool] = mapped_column(Boolean, default=True, nullable=False)

    user: Mapped["User"] = relationship("User", back_populates="consents")
    application: Mapped["Application"] = relationship("Application", back_populates="consents")

    def __repr__(self) -> str:
        return f"<UserConsent {self.user_id} -> {self.application_id}>"


class AuthorizationCode(Base):
    """OAuth2 授权码"""
    __tablename__ = "authorization_codes"

    id: Mapped[str] = mapped_column(String(36), primary_key=True, default=lambda: str(uuid.uuid4()))
    code: Mapped[str] = mapped_column(String(128), unique=True, index=True, nullable=False)
    client_id: Mapped[str] = mapped_column(String(64), nullable=False)
    user_id: Mapped[str] = mapped_column(String(36), nullable=False)
    redirect_uri: Mapped[str] = mapped_column(String(255), nullable=False)
    scope: Mapped[str] = mapped_column(Text, default="", nullable=False)
    code_challenge: Mapped[Optional[str]] = mapped_column(String(255), nullable=True)
    code_challenge_method: Mapped[Optional[str]] = mapped_column(String(10), nullable=True)
    expires_at: Mapped[datetime] = mapped_column(DateTime, nullable=False)
    used: Mapped[bool] = mapped_column(Boolean, default=False, nullable=False)
    created_at: Mapped[datetime] = mapped_column(DateTime, server_default=func.now(), nullable=False)


class AccessToken(Base):
    """OAuth2 访问令牌"""
    __tablename__ = "access_tokens"

    id: Mapped[str] = mapped_column(String(36), primary_key=True, default=lambda: str(uuid.uuid4()))
    token: Mapped[str] = mapped_column(String(512), unique=True, index=True, nullable=False)
    client_id: Mapped[str] = mapped_column(String(64), nullable=False)
    user_id: Mapped[Optional[str]] = mapped_column(String(36), nullable=True)
    scope: Mapped[str] = mapped_column(Text, default="", nullable=False)
    expires_at: Mapped[datetime] = mapped_column(DateTime, nullable=False)
    revoked: Mapped[bool] = mapped_column(Boolean, default=False, nullable=False)
    created_at: Mapped[datetime] = mapped_column(DateTime, server_default=func.now(), nullable=False)


class RefreshToken(Base):
    """OAuth2 刷新令牌"""
    __tablename__ = "refresh_tokens"

    id: Mapped[str] = mapped_column(String(36), primary_key=True, default=lambda: str(uuid.uuid4()))
    token: Mapped[str] = mapped_column(String(512), unique=True, index=True, nullable=False)
    access_token_id: Mapped[Optional[str]] = mapped_column(String(36), nullable=True)
    client_id: Mapped[str] = mapped_column(String(64), nullable=False)
    user_id: Mapped[Optional[str]] = mapped_column(String(36), nullable=True)
    scope: Mapped[str] = mapped_column(Text, default="", nullable=False)
    expires_at: Mapped[datetime] = mapped_column(DateTime, nullable=False)
    revoked: Mapped[bool] = mapped_column(Boolean, default=False, nullable=False)
    created_at: Mapped[datetime] = mapped_column(DateTime, server_default=func.now(), nullable=False)
