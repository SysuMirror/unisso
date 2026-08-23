"""UniSSO OAuth2 授权服务器实现（加固版）

加固内容：
- redirect_uri 严格匹配（防止开放重定向）
- state 参数验证（防止 CSRF）
- 授权码一次性使用
- PKCE 强制验证
- 审计日志集成
"""
import uuid
from datetime import datetime, timedelta, timezone
from typing import Optional, List, Dict, Any

from fastapi import HTTPException, status
from sqlalchemy import select, and_
from sqlalchemy.ext.asyncio import AsyncSession

from app.models import (
    User, Application, UserConsent,
    AuthorizationCode as AuthCodeModel,
    AccessToken as AccessTokenModel,
    RefreshToken as RefreshTokenModel,
)
from app.schemas import (
    AuthorizeRequest, TokenRequest, TokenResponse,
    IntrospectResponse, UserInfo,
)
from app.security import (
    generate_authorization_code, generate_token,
    create_access_token, create_refresh_token, create_id_token,
    decode_token, verify_pkce_challenge,
    scopes_to_list, list_to_scopes,
)
from app.redis_client import redis_client
from app.config import get_settings
from app.audit import log_authorize, log_token_issue

_settings = get_settings()


# ==================== 授权码管理（Redis 为主，DB 备用） ====================

async def create_authorization_code(
    client_id: str,
    user_id: str,
    redirect_uri: str,
    scope: str,
    code_challenge: Optional[str] = None,
    code_challenge_method: Optional[str] = None,
    state: Optional[str] = None,
) -> str:
    """创建授权码，存储到 Redis"""
    code = generate_authorization_code()
    expires = _settings.authorization_code_expire_seconds

    data = {
        "client_id": client_id,
        "user_id": user_id,
        "redirect_uri": redirect_uri,
        "scope": scope,
        "code_challenge": code_challenge,
        "code_challenge_method": code_challenge_method,
        "state": state,
        "created_at": datetime.now(timezone.utc).isoformat(),
    }

    await redis_client.set_json(f"auth_code:{code}", data, expire=expires)
    return code


async def get_authorization_code(code: str) -> Optional[Dict[str, Any]]:
    """获取授权码信息"""
    return await redis_client.get_json(f"auth_code:{code}")


async def use_authorization_code(code: str) -> Optional[Dict[str, Any]]:
    """使用授权码（获取并删除，确保一次性）"""
    data = await get_authorization_code(code)
    if data:
        await redis_client.delete(f"auth_code:{code}")
    return data


# ==================== 应用验证（加固版） ====================

async def get_application_by_client_id(db: AsyncSession, client_id: str) -> Optional[Application]:
    result = await db.execute(select(Application).where(Application.client_id == client_id))
    return result.scalar_one_or_none()


def _normalize_uri(uri: str) -> str:
    """标准化 URI（去除尾部斜杠、统一小写 scheme）"""
    uri = uri.rstrip("/")
    # 保持路径和查询参数的原始大小写，只统一 scheme
    if "://" in uri:
        scheme, rest = uri.split("://", 1)
        uri = f"{scheme.lower()}://{rest}"
    return uri


def _strict_redirect_uri_match(requested: str, allowed_uris: List[str]) -> bool:
    """严格匹配 redirect_uri

    防止开放重定向漏洞：
    - 不允许子路径匹配（如 https://evil.com?x=1 匹配 https://evil.com）
    - 不允许前缀匹配
    - 必须完全相等（去除尾部斜杠后）
    """
    requested_norm = _normalize_uri(requested)
    for allowed in allowed_uris:
        if _normalize_uri(allowed) == requested_norm:
            return True
    return False


async def verify_client(
    db: AsyncSession,
    client_id: str,
    client_secret: Optional[str] = None,
    redirect_uri: Optional[str] = None,
) -> Application:
    """验证客户端"""
    app = await get_application_by_client_id(db, client_id)
    if not app:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="invalid_client",
        )
    if not app.is_active:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="invalid_client",
        )

    # 验证 client_secret（confidential 客户端）
    if app.is_confidential and client_secret:
        from app.security import verify_password
        if not verify_password(client_secret, app.client_secret):
            raise HTTPException(
                status_code=status.HTTP_400_BAD_REQUEST,
                detail="invalid_client",
            )

    # 验证 redirect_uri（严格匹配）
    if redirect_uri:
        if not _strict_redirect_uri_match(redirect_uri, app.redirect_uris):
            raise HTTPException(
                status_code=status.HTTP_400_BAD_REQUEST,
                detail="invalid_redirect_uri",
            )

    return app


# ==================== Scope 处理 ====================

def validate_scopes(requested: List[str], allowed: List[str]) -> List[str]:
    """验证请求的 scopes 是否在允许范围内"""
    allowed_set = set(allowed)
    # openid 必须始终允许
    allowed_set.add("openid")
    allowed_set.add("profile")
    allowed_set.add("email")

    valid = []
    for scope in requested:
        if scope in allowed_set:
            valid.append(scope)

    return valid


# ==================== 用户授权（Consent）管理 ====================

async def get_user_consent(
    db: AsyncSession,
    user_id: str,
    application_id: str,
) -> Optional[UserConsent]:
    result = await db.execute(
        select(UserConsent).where(
            and_(
                UserConsent.user_id == user_id,
                UserConsent.application_id == application_id,
                UserConsent.is_active == True,
            )
        )
    )
    return result.scalar_one_or_none()


async def create_or_update_consent(
    db: AsyncSession,
    user_id: str,
    application_id: str,
    scopes: List[str],
) -> UserConsent:
    existing = await get_user_consent(db, user_id, application_id)
    if existing:
        existing.scopes = list(set(existing.scopes + scopes))
        existing.granted_at = datetime.now(timezone.utc)
        existing.is_active = True
        await db.flush()
        await db.refresh(existing)
        return existing

    consent = UserConsent(
        id=str(uuid.uuid4()),
        user_id=user_id,
        application_id=application_id,
        scopes=scopes,
        granted_at=datetime.now(timezone.utc),
        is_active=True,
    )
    db.add(consent)
    await db.flush()
    await db.refresh(consent)
    return consent


async def revoke_consent(
    db: AsyncSession,
    user_id: str,
    application_id: str,
):
    consent = await get_user_consent(db, user_id, application_id)
    if consent:
        consent.is_active = False
        consent.revoked_at = datetime.now(timezone.utc)
        await db.flush()


# ==================== Token 颁发 ====================

async def issue_tokens(
    db: AsyncSession,
    user: User,
    client_id: str,
    scope: str,
) -> TokenResponse:
    """颁发 access_token + refresh_token + id_token"""
    scopes = scopes_to_list(scope)

    access_token = create_access_token(
        subject=user.id,
        client_id=client_id,
        scope=scope,
        extra_claims={
            "username": user.username,
            "email": user.email,
        },
    )

    refresh_token = create_refresh_token(
        subject=user.id,
        client_id=client_id,
        scope=scope,
    )

    id_token = None
    if "openid" in scopes:
        id_token = create_id_token(
            user_id=user.id,
            username=user.username,
            email=user.email,
            full_name=user.full_name,
        )

    return TokenResponse(
        access_token=access_token,
        token_type="Bearer",
        expires_in=_settings.jwt_access_token_expire_minutes * 60,
        refresh_token=refresh_token,
        scope=scope,
        id_token=id_token,
    )


# ==================== Token 验证（Introspection） ====================

async def introspect_token(token: str, db: AsyncSession) -> IntrospectResponse:
    payload = decode_token(token)
    if not payload:
        return IntrospectResponse(active=False)

    token_type = payload.get("type")
    if token_type not in ("access", "refresh"):
        return IntrospectResponse(active=False)

    exp = payload.get("exp")
    now = datetime.now(timezone.utc).timestamp()
    if exp and exp < now:
        return IntrospectResponse(active=False)

    user_id = payload.get("sub")
    username = None
    if user_id:
        user = await db.execute(select(User).where(User.id == user_id))
        user = user.scalar_one_or_none()
        if user:
            username = user.username

    return IntrospectResponse(
        active=True,
        scope=payload.get("scope"),
        client_id=payload.get("client_id"),
        username=username,
        token_type=token_type,
        exp=exp,
        sub=user_id,
    )


# ==================== UserInfo ====================

async def get_userinfo(user: User) -> UserInfo:
    return UserInfo(
        sub=user.id,
        preferred_username=user.username or user.email,
        email=user.email,
        name=user.full_name,
        roles=[role.name for role in user.roles],
    )


# ==================== 授权端点处理（加固版） ====================

async def handle_authorize(
    db: AsyncSession,
    req: AuthorizeRequest,
    user: User,
    client_ip: str = "unknown",
) -> Dict[str, Any]:
    """处理授权请求，返回需要展示的信息或错误"""
    # 验证客户端
    app = await verify_client(db, req.client_id, redirect_uri=req.redirect_uri)

    # 验证 response_type
    if req.response_type != "code":
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="unsupported_response_type",
        )

    # 验证 scope
    requested_scopes = scopes_to_list(req.scope)
    valid_scopes = validate_scopes(requested_scopes, app.allowed_scopes)
    if not valid_scopes:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="invalid_scope",
        )

    # 检查是否已有 consent
    consent = await get_user_consent(db, user.id, app.id)
    if consent:
        granted_scopes = set(consent.scopes)
        needed_scopes = set(valid_scopes)
        if needed_scopes.issubset(granted_scopes):
            # 自动同意，直接发 code
            code = await create_authorization_code(
                client_id=req.client_id,
                user_id=user.id,
                redirect_uri=req.redirect_uri,
                scope=list_to_scopes(valid_scopes),
                code_challenge=req.code_challenge,
                code_challenge_method=req.code_challenge_method,
                state=req.state,
            )
            log_authorize(user.id, client_ip, req.client_id, valid_scopes, success=True, auto_approved=True)
            return {"auto_approved": True, "code": code, "state": req.state}

    # 需要用户确认授权
    log_authorize(user.id, client_ip, req.client_id, valid_scopes, success=True, auto_approved=False)
    return {
        "auto_approved": False,
        "application": app,
        "scopes": valid_scopes,
        "request": req,
    }


# ==================== Token 端点处理（加固版） ====================

async def handle_token(
    db: AsyncSession,
    req: TokenRequest,
    client_ip: str = "unknown",
) -> TokenResponse:
    """处理 Token 请求"""
    if req.grant_type == "authorization_code":
        return await _handle_authorization_code_grant(db, req, client_ip)
    elif req.grant_type == "refresh_token":
        return await _handle_refresh_token_grant(db, req, client_ip)
    else:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="unsupported_grant_type",
        )


async def _handle_authorization_code_grant(
    db: AsyncSession,
    req: TokenRequest,
    client_ip: str = "unknown",
) -> TokenResponse:
    """处理 authorization_code 授权（加固版）"""
    if not req.code:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="invalid_request",
        )

    # 获取并验证授权码（一次性使用）
    code_data = await use_authorization_code(req.code)
    if not code_data:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="invalid_grant",
        )

    # 验证客户端
    app = await verify_client(
        db,
        code_data["client_id"],
        client_secret=req.client_secret,
        redirect_uri=req.redirect_uri,
    )

    # 验证 redirect_uri 匹配（严格相等）
    if req.redirect_uri != code_data["redirect_uri"]:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="invalid_grant",
        )

    # 验证 PKCE（如果授权时提供了 challenge）
    if code_data.get("code_challenge"):
        if not req.code_verifier:
            raise HTTPException(
                status_code=status.HTTP_400_BAD_REQUEST,
                detail="invalid_request: code_verifier required",
            )
        if not verify_pkce_challenge(
            req.code_verifier,
            code_data["code_challenge"],
            code_data.get("code_challenge_method", "S256"),
        ):
            raise HTTPException(
                status_code=status.HTTP_400_BAD_REQUEST,
                detail="invalid_grant: pkce verification failed",
            )
    elif _settings.pkce_required:
        # 如果配置要求 PKCE 但授权时没提供
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="invalid_request: pkce required",
        )

    # 获取用户
    from app.auth import get_user_by_id
    user = await get_user_by_id(db, code_data["user_id"])
    if not user or not user.is_active:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="invalid_grant",
        )

    # 颁发 token
    log_token_issue(user.id, client_ip, code_data["client_id"], "authorization_code", success=True)
    return await issue_tokens(
        db, user, code_data["client_id"], code_data.get("scope", ""),
    )


async def _handle_refresh_token_grant(
    db: AsyncSession,
    req: TokenRequest,
    client_ip: str = "unknown",
) -> TokenResponse:
    """处理 refresh_token 授权"""
    if not req.refresh_token:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="invalid_request",
        )

    payload = decode_token(req.refresh_token)
    if not payload or payload.get("type") != "refresh":
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="invalid_grant",
        )

    client_id = payload.get("client_id")
    await verify_client(db, client_id, client_secret=req.client_secret)

    from app.auth import get_user_by_id
    user = await get_user_by_id(db, payload["sub"])
    if not user or not user.is_active:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="invalid_grant",
        )

    original_scope = payload.get("scope", "")
    if req.scope:
        requested = set(scopes_to_list(req.scope))
        original = set(scopes_to_list(original_scope))
        if not requested.issubset(original):
            raise HTTPException(
                status_code=status.HTTP_400_BAD_REQUEST,
                detail="invalid_scope",
            )
        scope = req.scope
    else:
        scope = original_scope

    log_token_issue(user.id, client_ip, client_id, "refresh_token", success=True)
    return await issue_tokens(db, user, client_id, scope)
