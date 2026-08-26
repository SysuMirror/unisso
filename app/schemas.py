"""UniSSO Pydantic 数据模型"""
import re
from datetime import datetime
from typing import Optional, List, Dict, Any

from pydantic import BaseModel, Field, EmailStr, ConfigDict, field_validator

# Sysu 邮箱正则
SYSU_EMAIL_RE = re.compile(r"^[a-zA-Z0-9._%+-]+@mail\d*\.sysu\.edu\.cn$")


def validate_sysu_email(email: str) -> str:
    if not SYSU_EMAIL_RE.match(email):
        raise ValueError(f"必须使用中山大学邮箱: *.mail*.sysu.edu.cn")
    return email


# ==================== 用户相关 ====================

class UserBase(BaseModel):
    email: str
    username: Optional[str] = None
    full_name: Optional[str] = None


class UserCreate(BaseModel):
    email: str = Field(..., min_length=5, max_length=128)
    password: str = Field(..., min_length=6, max_length=128)
    username: Optional[str] = Field(None, max_length=64)
    full_name: Optional[str] = None

    @field_validator("email")
    @classmethod
    def check_sysu_email(cls, v):
        return validate_sysu_email(v)


class UserUpdate(BaseModel):
    username: Optional[str] = None
    full_name: Optional[str] = None
    student_id: Optional[str] = None
    avatar: Optional[str] = None
    is_active: Optional[bool] = None


class UserResponse(BaseModel):
    model_config = ConfigDict(from_attributes=True)

    id: str
    email: str
    email_verified: bool
    username: Optional[str] = None
    full_name: Optional[str] = None
    student_id: Optional[str] = None
    avatar: Optional[str] = None
    is_active: bool
    is_admin: bool
    created_at: datetime
    last_login_at: Optional[datetime] = None
    roles: List["RoleResponse"] = []
    identities: List["UserIdentityResponse"] = []


class UserLogin(BaseModel):
    email: str
    password: str


class UserInfo(BaseModel):
    """OIDC userinfo 响应：仅输出 effective scopes 允许的 claims，sub 永远返回

    配合端点的 response_model_exclude_none，未授权 claim 不出现在响应中
    """
    sub: str
    preferred_username: Optional[str] = None
    name: Optional[str] = None
    picture: Optional[str] = None
    student_id: Optional[str] = None
    email: Optional[str] = None
    email_verified: Optional[bool] = None
    roles: Optional[List[str]] = None


# ==================== 外部身份绑定 ====================

class UserIdentityBase(BaseModel):
    provider: str = Field(..., min_length=1, max_length=64)
    provider_user_id: str = Field(..., min_length=1, max_length=255)
    provider_username: Optional[str] = None
    provider_email: Optional[str] = None
    provider_avatar: Optional[str] = None
    extra_data: Optional[Dict[str, Any]] = None


class UserIdentityCreate(UserIdentityBase):
    pass


class UserIdentityResponse(UserIdentityBase):
    model_config = ConfigDict(from_attributes=True)

    id: str
    user_id: str
    is_active: bool
    created_at: datetime
    updated_at: datetime


# ==================== 角色权限 ====================

class RoleBase(BaseModel):
    name: str = Field(..., min_length=1, max_length=64)
    description: Optional[str] = None


class RoleCreate(RoleBase):
    pass


class RoleResponse(RoleBase):
    model_config = ConfigDict(from_attributes=True)

    id: str
    is_system: bool
    created_at: datetime
    permissions: List["PermissionResponse"] = []


class PermissionBase(BaseModel):
    name: str = Field(..., min_length=1, max_length=64)
    resource: str = Field(..., min_length=1, max_length=64)
    action: str = Field(..., min_length=1, max_length=64)
    description: Optional[str] = None


class PermissionCreate(PermissionBase):
    pass


class PermissionResponse(PermissionBase):
    model_config = ConfigDict(from_attributes=True)

    id: str
    created_at: datetime


# ==================== 应用相关 ====================

class ApplicationBase(BaseModel):
    name: str = Field(..., min_length=1, max_length=128)
    description: Optional[str] = None
    redirect_uris: List[str] = []
    # 默认仅授予基础身份标识，按需在勾选面板中扩充
    allowed_scopes: List[str] = ["openid"]
    homepage_url: Optional[str] = None
    callback_url: Optional[str] = None
    is_confidential: bool = True


class ApplicationCreate(ApplicationBase):
    pass


class ApplicationUpdate(BaseModel):
    name: Optional[str] = None
    description: Optional[str] = None
    redirect_uris: Optional[List[str]] = None
    allowed_scopes: Optional[List[str]] = None
    homepage_url: Optional[str] = None
    callback_url: Optional[str] = None
    is_active: Optional[bool] = None


class ApplicationResponse(ApplicationBase):
    model_config = ConfigDict(from_attributes=True)

    id: str
    client_id: str
    client_secret: Optional[str] = None
    is_active: bool
    owner_id: Optional[str] = None
    created_at: datetime
    updated_at: datetime


class ApplicationPublic(BaseModel):
    model_config = ConfigDict(from_attributes=True)

    id: str
    client_id: str
    name: str
    description: Optional[str] = None
    icon: Optional[str] = None
    homepage_url: Optional[str] = None
    allowed_scopes: List[str] = []


# ==================== OAuth2 / 授权 ====================

class AuthorizeRequest(BaseModel):
    response_type: str = "code"
    client_id: str
    redirect_uri: str
    scope: str = "openid profile"
    state: Optional[str] = None
    code_challenge: Optional[str] = None
    code_challenge_method: Optional[str] = "S256"


class TokenRequest(BaseModel):
    grant_type: str
    code: Optional[str] = None
    redirect_uri: Optional[str] = None
    client_id: Optional[str] = None
    client_secret: Optional[str] = None
    refresh_token: Optional[str] = None
    code_verifier: Optional[str] = None
    scope: Optional[str] = None


class TokenResponse(BaseModel):
    access_token: str
    token_type: str = "Bearer"
    expires_in: int
    refresh_token: Optional[str] = None
    scope: Optional[str] = None
    id_token: Optional[str] = None


class IntrospectResponse(BaseModel):
    active: bool
    scope: Optional[str] = None
    client_id: Optional[str] = None
    username: Optional[str] = None
    token_type: Optional[str] = None
    exp: Optional[int] = None
    sub: Optional[str] = None


# ==================== 用户授权管理 ====================

class UserConsentResponse(BaseModel):
    model_config = ConfigDict(from_attributes=True)

    id: str
    application: ApplicationPublic
    scopes: List[str]
    granted_at: datetime
    is_active: bool


class ConsentUpdate(BaseModel):
    scopes: List[str]


# ==================== 通用 ====================

class StandardResponse(BaseModel):
    ok: bool = True
    message: Optional[str] = None
    data: Optional[Dict[str, Any]] = None


class ErrorResponse(BaseModel):
    error: str
    error_description: Optional[str] = None
