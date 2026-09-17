"""UniSSO OAuth2 授权服务器实现（加固版）

加固内容：
- redirect_uri 严格匹配（防止开放重定向）
- state 参数验证（防止 CSRF）
- 授权码一次性使用
- PKCE 强制验证（公开客户端强制，confidential 客户端必须携带 secret）
- 细粒度 scope 三层交集校验（请求 ∩ 应用白名单 ∩ 用户同意）
- access_token 禁止携带 PII；id_token / userinfo 按 effective scopes 过滤
- token 落库支持主动撤销（撤销 consent 联动批量撤销）
- 审计日志集成
"""
import uuid
import re
from datetime import datetime, timedelta, timezone
from typing import Optional, List, Dict, Any

from fastapi import HTTPException, status
from sqlalchemy import select, and_, update
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
from app.scope_definitions import expand_legacy_scopes, get_claims_for_scopes
from app.security import (
    generate_authorization_code, generate_token,
    create_access_token, create_refresh_token, create_id_token,
    decode_token, verify_pkce_challenge,
    scopes_to_list, list_to_scopes,
    generate_jti, access_token_expiry, refresh_token_expiry,
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
    nonce: Optional[str] = None,
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
        "nonce": nonce,
        "created_at": datetime.now(timezone.utc).isoformat(),
    }

    await redis_client.set_json(f"auth_code:{code}", data, expire=expires)
    return code


async def get_authorization_code(code: str) -> Optional[Dict[str, Any]]:
    """获取授权码信息"""
    return await redis_client.get_json(f"auth_code:{code}")


async def use_authorization_code(code: str) -> Optional[Dict[str, Any]]:
    """使用授权码（获取并删除，确保一次性）"""
    return await redis_client.getdel_json(f"auth_code:{code}")


# ==================== 应用验证（加固版） ====================

async def get_application_by_client_id(db: AsyncSession, client_id: str) -> Optional[Application]:
    result = await db.execute(select(Application).where(Application.client_id == client_id))
    return result.scalar_one_or_none()


async def get_active_application(db: AsyncSession, client_id: Optional[str]) -> Optional[Application]:
    """Resolve a token's client only while its registration remains enabled."""
    if not client_id:
        return None
    result = await db.execute(
        select(Application).where(Application.client_id == client_id, Application.is_active.is_(True))
    )
    return result.scalar_one_or_none()


def _strict_redirect_uri_match(requested: str, allowed_uris: List[str]) -> bool:
    """Registered redirect URIs are matched exactly."""
    return requested in allowed_uris


async def verify_client(
    db: AsyncSession,
    client_id: str,
    client_secret: Optional[str] = None,
    redirect_uri: Optional[str] = None,
    authenticate_secret: bool = False,
) -> Application:
    """验证客户端

    - 基础校验（authorize 端点）：client 存在、启用、redirect_uri 严格匹配。
      浏览器前信道不携带 client_secret，不做密钥校验。
    - 密钥校验（token 端点，authenticate_secret=True）：
      - confidential 客户端：必须携带 client_secret（缺失或错误均拒绝）
      - 公开客户端：禁止携带 client_secret（强制走 PKCE）
    """
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

    if authenticate_secret:
        if app.is_confidential:
            if not client_secret:
                raise HTTPException(
                    status_code=status.HTTP_400_BAD_REQUEST,
                    detail="invalid_client: confidential 客户端必须携带 client_secret",
                )
            from app.security import verify_password
            if not verify_password(client_secret, app.client_secret):
                raise HTTPException(
                    status_code=status.HTTP_400_BAD_REQUEST,
                    detail="invalid_client",
                )
        elif client_secret:
            raise HTTPException(
                status_code=status.HTTP_400_BAD_REQUEST,
                detail="invalid_client: 公开客户端不允许携带 client_secret，请使用 PKCE",
            )

    # 验证 redirect_uri（严格匹配）
    if redirect_uri:
        if not _strict_redirect_uri_match(redirect_uri, app.redirect_uris):
            raise HTTPException(
                status_code=status.HTTP_400_BAD_REQUEST,
                detail="invalid_redirect_uri",
            )

    return app


def _require_pkce(code_challenge: Optional[str], code_challenge_method: Optional[str]) -> None:
    """Every authorization-code flow uses the stronger S256 contract."""
    if not code_challenge or code_challenge_method != "S256" or not re.fullmatch(r"[A-Za-z0-9_-]{43}", code_challenge):
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="invalid_request: S256 PKCE code_challenge required",
        )


# ==================== Scope 处理（三层交集） ====================

def compute_effective_scopes(
    requested: List[str],
    app_allowed: List[str],
    consented: Optional[List[str]] = None,
) -> List[str]:
    """三层取交集得到最终生效 scope 集合

    effective = requested ∩ 应用白名单 ∩ 用户已同意（consented 为 None 时跳过第三层）

    - 老粗粒度 scope（profile）先展开为等价细粒度集合（过渡期兼容）
    - openid 永远放行（sub 用于子应用账号绑定）
    - 超出应用白名单的 scope 静默丢弃，不报错
    """
    allowed = set(expand_legacy_scopes(app_allowed))
    allowed.add("openid")
    effective = set(expand_legacy_scopes(requested)) & allowed
    if consented is not None:
        effective &= set(expand_legacy_scopes(consented))
    return sorted(effective)


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
    client_id: Optional[str] = None,
):
    """撤销用户授权，并联动撤销该应用下该用户的全部 token"""
    consent = await get_user_consent(db, user_id, application_id)
    if consent:
        consent.is_active = False
        consent.revoked_at = datetime.now(timezone.utc)
        await db.flush()
        if client_id:
            await revoke_tokens_by_consent(db, user_id, client_id)


# ==================== Token 颁发 ====================

def build_user_claims(user: User, scopes: List[str]) -> Dict[str, Any]:
    """按 effective scopes 构建用户 claims（供 id_token / userinfo 使用，禁止进入 access_token）"""
    claims: Dict[str, Any] = {"sub": user.id}
    source = {
        "preferred_username": user.username,
        "name": user.full_name,
        "picture": user.avatar,
        "student_id": user.student_id,
        "email": user.email,
        "email_verified": user.email_verified,
        "roles": [r.name for r in user.roles],
    }
    for claim in get_claims_for_scopes(scopes):
        if claim in source and source[claim] is not None:
            claims[claim] = source[claim]
    return claims


# ==================== Token 存储与撤销 ====================

async def is_token_revoked(db: AsyncSession, jti: str, token_type: str) -> bool:
    """检查 token 是否已撤销（不存在记录或 revoked=True 均视为撤销）"""
    model = AccessTokenModel if token_type == "access" else RefreshTokenModel
    result = await db.execute(
        select(model.revoked).where(model.token == jti)
    )
    row = result.scalar_one_or_none()
    return row is None or row


async def revoke_tokens_by_consent(db: AsyncSession, user_id: str, client_id: str) -> int:
    """撤销某用户在某应用下的全部 token（consent 撤销时联动调用）"""
    now = datetime.now(timezone.utc)
    revoked = 0
    for model in (AccessTokenModel, RefreshTokenModel):
        result = await db.execute(
            update(model)
            .where(
                and_(
                    model.user_id == user_id,
                    model.client_id == client_id,
                    model.revoked == False,  # noqa: E712
                    model.expires_at > now,
                )
            )
            .values(revoked=True)
        )
        revoked += result.rowcount or 0
    return revoked


# ==================== Token 签发 ====================

async def issue_tokens(
    db: AsyncSession,
    user: User,
    application: Application,
    granted_scopes: List[str],
    include_id_token: bool = False,
    nonce: Optional[str] = None,
) -> TokenResponse:
    """为用户签发 token

    - access_token / refresh_token 落库（jti 指向），支持主动撤销
    - access_token 不携带 PII；id_token 按 effective scopes 输出 claims
    - refresh 轮换由调用方实现：先撤销旧 jti 再调用本函数签发全新 token
    """
    scope_str = list_to_scopes(granted_scopes)
    now = datetime.now(timezone.utc)

    access_jti = generate_jti()
    refresh_jti = generate_jti()

    access_token = create_access_token(
        subject=user.id,
        client_id=application.client_id,
        scope=scope_str,
        jti=access_jti,
    )
    refresh_token = create_refresh_token(
        subject=user.id,
        client_id=application.client_id,
        scope=scope_str,
        jti=refresh_jti,
    )

    db.add(AccessTokenModel(
        token=access_jti,  # token 列存储 jti 标识（不落明文 JWT）
        user_id=user.id,
        client_id=application.client_id,
        scope=scope_str,
        expires_at=access_token_expiry(now),
        revoked=False,
    ))
    db.add(RefreshTokenModel(
        token=refresh_jti,
        user_id=user.id,
        client_id=application.client_id,
        scope=scope_str,
        expires_at=refresh_token_expiry(now),
        revoked=False,
    ))

    # 凭证必须先落库再返回：客户端拿到响应后会立刻用 token 请求资源，
    # 依赖 get_db 兜底提交在远程库高延迟下会输给下一个请求（读到未提交数据）
    await db.commit()

    id_token = None
    if include_id_token and "openid" in granted_scopes:
        claims = build_user_claims(user, granted_scopes)
        if nonce:
            claims["nonce"] = nonce
        id_token = create_id_token(
            user_id=user.id,
            client_id=application.client_id,
            claims=claims,
        )

    settings = get_settings()
    return TokenResponse(
        access_token=access_token,
        token_type="Bearer",
        expires_in=settings.jwt_access_token_expire_minutes * 60,
        refresh_token=refresh_token,
        scope=scope_str,
        id_token=id_token,
    )


# ==================== Token 验证（Introspection） ====================

async def introspect_token(token: str, db: AsyncSession) -> IntrospectResponse:
    payload = decode_token(token) or decode_token(token, token_type="refresh")
    if not payload:
        return IntrospectResponse(active=False)

    token_type = payload.get("type")
    if token_type not in ("access", "refresh"):
        return IntrospectResponse(active=False)

    exp = payload.get("exp")
    now = datetime.now(timezone.utc).timestamp()
    if exp and exp < now:
        return IntrospectResponse(active=False)

    # 落库撤销检查（被撤销或未落库的 token 视为不活跃）
    if await is_token_revoked(db, payload.get("jti", ""), token_type):
        return IntrospectResponse(active=False)

    if not await get_active_application(db, payload.get("client_id")):
        return IntrospectResponse(active=False)

    user_id = payload.get("sub")
    username = None
    if user_id:
        user = await db.execute(select(User).where(User.id == user_id))
        user = user.scalar_one_or_none()
        if not user or not user.is_active:
            return IntrospectResponse(active=False)
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

async def get_userinfo_from_token(db: AsyncSession, token: str) -> UserInfo:
    """从 access_token 解析用户信息（按 effective scopes 过滤 claims）

    - 校验 token 类型、有效期、撤销状态、用户活跃状态
    - sub 永远返回；其余 claims 仅在 scope 授权范围内输出
    """
    payload = decode_token(token)
    if not payload or payload.get("type") != "access":
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="invalid_token",
        )

    exp = payload.get("exp")
    if exp and exp < datetime.now(timezone.utc).timestamp():
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="token_expired",
        )

    if await is_token_revoked(db, payload.get("jti", ""), "access"):
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="token_revoked",
        )

    if not await get_active_application(db, payload.get("client_id")):
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="invalid_client",
        )

    from app.auth import get_user_by_id
    user = await get_user_by_id(db, payload["sub"])
    if not user or not user.is_active:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="invalid_token",
        )

    scopes = scopes_to_list(payload.get("scope", ""))
    return UserInfo(**build_user_claims(user, scopes))


# ==================== 授权端点处理（加固版） ====================

async def handle_authorize(
    db: AsyncSession,
    req: AuthorizeRequest,
    user: User,
    client_ip: str = "unknown",
) -> Dict[str, Any]:
    """处理授权请求，返回需要展示的信息或错误

    - 公开客户端强制 PKCE
    - 展示给用户确认的 scope = requested ∩ 应用白名单（细粒度展开后）
    - 已有 consent 且覆盖请求 scope 时静默通过，否则要求用户确认
    """
    # 验证客户端
    app = await verify_client(db, req.client_id, redirect_uri=req.redirect_uri)

    # 验证 response_type
    if req.response_type != "code":
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="unsupported_response_type",
        )

    _require_pkce(req.code_challenge, req.code_challenge_method)

    # 计算 requested ∩ 应用白名单
    requested_scopes = scopes_to_list(req.scope)
    candidate_scopes = compute_effective_scopes(requested_scopes, app.allowed_scopes)
    if not candidate_scopes:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="invalid_scope",
        )

    # 检查是否已有 consent 覆盖请求的 scope
    consent = await get_user_consent(db, user.id, app.id)
    if consent:
        if set(candidate_scopes) <= set(expand_legacy_scopes(consent.scopes)):
            # 自动同意，直接发 code（实际生效 scope 不超过 consent）
            code = await create_authorization_code(
                client_id=req.client_id,
                user_id=user.id,
                redirect_uri=req.redirect_uri,
                scope=list_to_scopes(candidate_scopes),
                code_challenge=req.code_challenge,
                code_challenge_method=req.code_challenge_method,
                state=req.state,
                nonce=req.nonce,
            )
            log_authorize(user.id, client_ip, req.client_id, candidate_scopes, success=True, auto_approved=True)
            return {"auto_approved": True, "code": code, "state": req.state}

    # 需要用户确认授权
    log_authorize(user.id, client_ip, req.client_id, candidate_scopes, success=True, auto_approved=False)
    return {
        "auto_approved": False,
        "application": app,
        "scopes": candidate_scopes,
        "request": req,
    }


async def issue_code_for_consent(
    db: AsyncSession,
    req: AuthorizeRequest,
    user: User,
    approved_scopes: List[str],
    client_ip: str = "unknown",
) -> str:
    """用户在授权页确认后：保存 consent 并签发授权码

    - approved_scopes 已被 routes 层限制在应用白名单内
    - 保存后复用 Redis 的 state 防重放（one-time）
    """
    app = await verify_client(db, req.client_id, redirect_uri=req.redirect_uri)
    _require_pkce(req.code_challenge, req.code_challenge_method)
    if not set(approved_scopes) <= set(expand_legacy_scopes(scopes_to_list(req.scope))):
        raise HTTPException(status_code=400, detail="invalid_scope")

    # 三层交集最终确定生效 scope
    effective_scopes = compute_effective_scopes(approved_scopes, app.allowed_scopes)
    if not effective_scopes:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="invalid_scope",
        )

    # 持久化用户授权记录
    await create_or_update_consent(db, user.id, app.id, effective_scopes)

    code = await create_authorization_code(
        client_id=req.client_id,
        user_id=user.id,
        redirect_uri=req.redirect_uri,
        scope=list_to_scopes(effective_scopes),
        code_challenge=req.code_challenge,
        code_challenge_method=req.code_challenge_method,
        state=req.state,
        nonce=req.nonce,
    )
    log_authorize(user.id, client_ip, req.client_id, effective_scopes, success=True, auto_approved=False)
    return code


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

    if req.client_id != code_data["client_id"]:
        raise HTTPException(status_code=400, detail="invalid_grant")

    # 验证客户端（token 端点，强制密钥校验）
    app = await verify_client(
        db,
        code_data["client_id"],
        client_secret=req.client_secret,
        redirect_uri=req.redirect_uri,
        authenticate_secret=True,
    )

    # 验证 redirect_uri 匹配（严格相等）
    if req.redirect_uri != code_data["redirect_uri"]:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="invalid_grant",
        )

    if not code_data.get("code_challenge") or code_data.get("code_challenge_method") != "S256":
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="invalid_request: pkce required",
        )
    if not req.code_verifier:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="invalid_request: code_verifier required",
        )
    if not verify_pkce_challenge(req.code_verifier, code_data["code_challenge"], "S256"):
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="invalid_grant: pkce verification failed",
        )

    # 获取用户
    from app.auth import get_user_by_id
    user = await get_user_by_id(db, code_data["user_id"])
    if not user or not user.is_active:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="invalid_grant",
        )

    # 颁发 token（含 id_token，scope 不变）
    granted_scopes = scopes_to_list(code_data.get("scope", ""))
    log_token_issue(user.id, client_ip, code_data["client_id"], "authorization_code", success=True)
    return await issue_tokens(
        db, user, app, granted_scopes, include_id_token=True, nonce=code_data.get("nonce"),
    )


async def _handle_refresh_token_grant(
    db: AsyncSession,
    req: TokenRequest,
    client_ip: str = "unknown",
) -> TokenResponse:
    """处理 refresh_token 授权（轮换：旧 refresh token 撤销后签发新的）"""
    if not req.refresh_token:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="invalid_request",
        )

    payload = decode_token(req.refresh_token, token_type="refresh")
    if not payload or payload.get("type") != "refresh":
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="invalid_grant",
        )

    # 撤销检查（未落库或已撤销均拒绝）
    if await is_token_revoked(db, payload.get("jti", ""), "refresh"):
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="invalid_grant",
        )

    client_id = payload.get("client_id")
    if req.client_id != client_id:
        raise HTTPException(status_code=400, detail="invalid_grant")
    app = await verify_client(db, client_id, client_secret=req.client_secret, authenticate_secret=True)

    from app.auth import get_user_by_id
    user = await get_user_by_id(db, payload["sub"])
    if not user or not user.is_active:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="invalid_grant",
        )

    consent = await get_user_consent(db, user.id, app.id)
    if not consent:
        raise HTTPException(status_code=400, detail="invalid_grant")
    original_scope = list_to_scopes(compute_effective_scopes(
        scopes_to_list(payload.get("scope", "")), app.allowed_scopes, consent.scopes))
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

    # 轮换：撤销旧 refresh token 后签发全新 token 对
    consumed = await db.execute(
        update(RefreshTokenModel)
        .where(RefreshTokenModel.token == payload.get("jti"),
               RefreshTokenModel.revoked == False,
               RefreshTokenModel.user_id == user.id,
               RefreshTokenModel.client_id == client_id)
        .values(revoked=True)
    )

    if consumed.rowcount != 1:
        raise HTTPException(status_code=400, detail="invalid_grant")

    log_token_issue(user.id, client_ip, client_id, "refresh_token", success=True)
    return await issue_tokens(db, user, app, scopes_to_list(scope))
