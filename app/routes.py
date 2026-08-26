"""UniSSO 路由定义（加固版）

集成：
- 审计日志
- 登录失败限制
- CSRF Token
- 文件上传 MIME 白名单
- 管理员操作审计
"""
import json
from typing import Optional

from fastapi import APIRouter, Request, Depends, Form, Query, HTTPException, status
from fastapi.responses import HTMLResponse, RedirectResponse, JSONResponse
from sqlalchemy import select, and_
from sqlalchemy.orm import selectinload
from sqlalchemy.ext.asyncio import AsyncSession

from app.database import get_db
from app.models import User, Application, UserConsent, Role, Permission, UserIdentity
from app.schemas import (
    UserCreate, UserLogin, UserUpdate, UserResponse,
    UserIdentityCreate, UserIdentityResponse,
    ApplicationCreate, ApplicationUpdate, ApplicationResponse,
    AuthorizeRequest, TokenRequest, TokenResponse,
    IntrospectResponse, UserInfo, ConsentUpdate,
    StandardResponse, ErrorResponse,
)
from app.auth import (
    authenticate_user, create_user, get_user_by_id, get_user_by_email,
    get_user_by_username, update_user, create_session, get_session, delete_session,
    require_user, require_admin, get_current_user_from_session,
    get_current_user_from_token, require_token_user, require_auth_user,
    check_permission, init_default_data,
    get_user_identity, get_user_identities_by_user, bind_identity, unbind_identity,
    is_sysu_email,
    check_login_attempts, record_login_failure, clear_login_attempts,
    get_cookie_settings, create_csrf_token, get_csrf_token, validate_csrf_token,
)
from app.oauth2_server import (
    handle_authorize, handle_token, introspect_token, get_userinfo_from_token,
    issue_code_for_consent, compute_effective_scopes,
    create_or_update_consent, revoke_consent, get_user_consent,
    verify_client, get_application_by_client_id,
)
from app.scope_definitions import scope_description, supported_scopes
from app.security import (
    generate_client_id, generate_client_secret, hash_password,
    verify_password, scopes_to_list, validate_password_strength,
)
from app.redis_client import redis_client
from app.config import get_settings
from app.storage import upload_file, get_file_url, delete_file
from app.audit import (
    log_login, log_register, log_authorize, log_token_issue,
    log_password_change, log_admin_action, log_security_alert,
)

_settings = get_settings()

router = APIRouter()


def _url(path: str) -> str:
    """生成带 root_path 前缀的 URL"""
    rp = _settings.root_path.rstrip("/")
    if rp:
        return f"{rp}{path}"
    return path


# ==================== 辅助函数 ====================

def _get_client_ip(request: Request) -> str:
    """获取真实客户端 IP"""
    forwarded = request.headers.get("x-forwarded-for")
    if forwarded:
        return forwarded.split(",")[0].strip()
    real_ip = request.headers.get("x-real-ip")
    if real_ip:
        return real_ip.strip()
    if request.client:
        return request.client.host
    return "unknown"


def _get_user_agent(request: Request) -> Optional[str]:
    return request.headers.get("user-agent")


# 允许的图片 MIME 类型
ALLOWED_IMAGE_TYPES = {
    "image/jpeg", "image/png", "image/gif", "image/webp", "image/svg+xml"
}


def _validate_image_file(content_type: Optional[str], content: bytes) -> bool:
    """验证文件类型（MIME + 魔数）"""
    if not content_type or content_type not in ALLOWED_IMAGE_TYPES:
        return False

    # 魔数检查（文件头）
    magic = {
        b"\xff\xd8\xff": "image/jpeg",
        b"\x89PNG\r\n\x1a\n": "image/png",
        b"GIF87a": "image/gif",
        b"GIF89a": "image/gif",
        b"<?xml": "image/svg+xml",
        b"<svg": "image/svg+xml",
    }
    for header, expected_type in magic.items():
        if content.startswith(header):
            return content_type == expected_type or (expected_type == "image/svg+xml" and content_type in ("image/svg+xml", "image/png"))

    # WebP 魔数
    if content.startswith(b"RIFF") and content[8:12] == b"WEBP":
        return content_type == "image/webp"

    # SVG 可能没有 XML 声明
    if content_type == "image/svg+xml":
        content_str = content[:200].decode("utf-8", errors="ignore").lower()
        if "<svg" in content_str:
            return True

    return False


# ==================== 页面路由（HTML） ====================

@router.get("/", response_class=HTMLResponse)
async def index(request: Request, user: Optional[User] = Depends(get_current_user_from_session)):
    """首页 — 未登录自动跳转登录页"""
    if user is None:
        return RedirectResponse(url="/login", status_code=302)
    from app.main import templates
    return templates.TemplateResponse(request, "index.html", {
        "request": request,
        "user": user,
        "app_name": _settings.app_name,
    })


@router.get("/login", response_class=HTMLResponse)
async def login_page(request: Request, next: Optional[str] = None, error: Optional[str] = None):
    """登录页面"""
    from app.main import templates
    return templates.TemplateResponse(request, "login.html", {
        "request": request,
        "next": next or _url('/'),
        "error": error,
        "app_name": _settings.app_name,
    })


@router.get("/register", response_class=HTMLResponse)
async def register_page(request: Request, error: Optional[str] = None):
    """注册页面"""
    from app.main import templates
    return templates.TemplateResponse(request, "register.html", {
        "request": request,
        "error": error,
        "app_name": _settings.app_name,
    })


@router.get("/profile", response_class=HTMLResponse)
async def profile_page(
    request: Request,
    user: User = Depends(require_user),
    db: AsyncSession = Depends(get_db),
):
    """个人中心"""
    from app.main import templates
    identities = await get_user_identities_by_user(db, user.id)
    return templates.TemplateResponse(request, "profile.html", {
        "request": request,
        "user": user,
        "identities": identities,
        "app_name": _settings.app_name,
    })


@router.get("/authorize", response_class=HTMLResponse)
async def authorize_page(
    request: Request,
    response_type: str = Query("code"),
    client_id: str = Query(...),
    redirect_uri: str = Query(...),
    scope: str = Query("openid profile"),
    state: Optional[str] = Query(None),
    code_challenge: Optional[str] = Query(None),
    code_challenge_method: Optional[str] = Query("S256"),
    user: User = Depends(require_user),
    db: AsyncSession = Depends(get_db),
):
    """OAuth2 授权确认页面"""
    from app.main import templates

    req = AuthorizeRequest(
        response_type=response_type,
        client_id=client_id,
        redirect_uri=redirect_uri,
        scope=scope,
        state=state,
        code_challenge=code_challenge,
        code_challenge_method=code_challenge_method,
    )

    client_ip = _get_client_ip(request)
    result = await handle_authorize(db, req, user, client_ip=client_ip)

    if result.get("auto_approved"):
        code = result["code"]
        redirect = f"{redirect_uri}?code={code}"
        if state:
            redirect += f"&state={state}"
        return RedirectResponse(redirect, status_code=status.HTTP_302_FOUND)

    app = result["application"]
    scopes = result["scopes"]
    scope_items = [{"name": s, "description": scope_description(s)} for s in scopes]
    return templates.TemplateResponse(request, "authorize.html", {
        "request": request,
        "user": user,
        "application": app,
        "scope_items": scope_items,
        "client_id": client_id,
        "redirect_uri": redirect_uri,
        "scope": scope,
        "state": state,
        "code_challenge": code_challenge,
        "code_challenge_method": code_challenge_method,
        "app_name": _settings.app_name,
    })


@router.get("/consents", response_class=HTMLResponse)
async def consents_page(
    request: Request,
    user: User = Depends(require_user),
    db: AsyncSession = Depends(get_db),
):
    """我的授权管理页面"""
    from app.main import templates

    result = await db.execute(
        select(UserConsent).where(
            and_(UserConsent.user_id == user.id, UserConsent.is_active == True)
        )
    )
    consents = result.scalars().all()

    return templates.TemplateResponse(request, "consents.html", {
        "request": request,
        "user": user,
        "consents": consents,
        "app_name": _settings.app_name,
    })


@router.get("/admin/apps", response_class=HTMLResponse)
async def admin_apps_page(
    request: Request,
    user: User = Depends(require_admin),
    db: AsyncSession = Depends(get_db),
):
    """应用管理页面"""
    from app.main import templates

    result = await db.execute(select(Application))
    apps = result.scalars().all()

    from app.scope_definitions import SCOPE_DEFINITIONS
    scope_catalog = [
        {"name": s, "description": SCOPE_DEFINITIONS[s][1]}
        for s in supported_scopes()
    ]

    return templates.TemplateResponse(request, "admin_apps.html", {
        "request": request,
        "user": user,
        "applications": apps,
        "scope_catalog": scope_catalog,
        "app_name": _settings.app_name,
    })


@router.get("/admin/users", response_class=HTMLResponse)
async def admin_users_page(
    request: Request,
    user: User = Depends(require_admin),
    db: AsyncSession = Depends(get_db),
):
    """用户管理页面"""
    from app.main import templates

    result = await db.execute(select(User))
    users = result.scalars().all()

    return templates.TemplateResponse(request, "admin_users.html", {
        "request": request,
        "user": user,
        "users": users,
        "app_name": _settings.app_name,
    })


# ==================== CSRF Token 端点 ====================

@router.get("/api/csrf-token")
async def get_csrf_token_endpoint(request: Request):
    """获取 CSRF token（需要已有 session 或生成临时 token）"""
    session_id = request.cookies.get("unisso_session")
    if session_id:
        token = await create_csrf_token(session_id)
    else:
        # 无 session 时生成一次性 token（用于登录/注册页面）
        token = await create_csrf_token(f"anon_{_get_client_ip(request)}")
    return {"csrf_token": token}


# ==================== 认证 API（加固版） ====================

@router.post("/api/auth/login")
async def api_login(
    request: Request,
    email: str = Form(...),
    password: str = Form(...),
    next: Optional[str] = Form("/"),
    db: AsyncSession = Depends(get_db),
):
    """登录 API（邮箱+密码，带失败限制和审计日志）"""
    client_ip = _get_client_ip(request)
    user_agent = _get_user_agent(request)

    # 检查登录尝试次数
    allowed, remaining = await check_login_attempts(client_ip)
    if not allowed:
        log_login(None, client_ip, user_agent, success=False, email=email, reason=f"登录被锁定，剩余 {remaining} 秒")
        return RedirectResponse(
            f"{_url('/login')}?error=too_many_attempts&next={next}",
            status_code=status.HTTP_302_FOUND,
        )

    user = await authenticate_user(db, email, password)
    if not user:
        await record_login_failure(client_ip)
        log_login(None, client_ip, user_agent, success=False, email=email, reason="凭证错误")
        return RedirectResponse(
            f"{_url('/login')}?error=invalid_credentials&next={next}",
            status_code=status.HTTP_302_FOUND,
        )

    # 登录成功
    await clear_login_attempts(client_ip)

    from datetime import datetime, timezone
    user.last_login_at = datetime.now(timezone.utc)
    await db.flush()

    session_id = await create_session(user.id, user.username or user.email)

    log_login(user.id, client_ip, user_agent, success=True, email=email)

    response = RedirectResponse(next, status_code=status.HTTP_302_FOUND)
    cookie_opts = get_cookie_settings(request)
    response.set_cookie(key="unisso_session", value=session_id, **cookie_opts)
    return response


@router.post("/api/auth/logout")
async def api_logout(request: Request):
    """登出 API"""
    session_id = request.cookies.get("unisso_session")
    if session_id:
        await delete_session(session_id)

    response = RedirectResponse(_url("/"), status_code=status.HTTP_302_FOUND)
    response.delete_cookie("unisso_session", path="/")
    return response


@router.post("/api/auth/register")
async def api_register(
    request: Request,
    email: str = Form(...),
    password: str = Form(...),
    username: Optional[str] = Form(None),
    full_name: Optional[str] = Form(None),
    db: AsyncSession = Depends(get_db),
):
    """注册 API（邮箱+密码，带密码强度验证和审计日志）"""
    client_ip = _get_client_ip(request)
    user_agent = _get_user_agent(request)

    # 验证邮箱格式
    if not is_sysu_email(email):
        log_register(None, client_ip, user_agent, success=False, email=email, reason="邮箱格式错误")
        return RedirectResponse(
            f"{_url('/register')}?error=invalid_email",
            status_code=status.HTTP_302_FOUND,
        )

    # 检查邮箱是否已存在
    existing = await get_user_by_email(db, email)
    if existing:
        log_register(None, client_ip, user_agent, success=False, email=email, reason="邮箱已存在")
        return RedirectResponse(
            f"{_url('/register')}?error=email_exists",
            status_code=status.HTTP_302_FOUND,
        )

    user_data = UserCreate(
        email=email,
        password=password,
        username=username,
        full_name=full_name,
    )
    try:
        user = await create_user(db, user_data)
        await db.commit()
    except ValueError as e:
        log_register(None, client_ip, user_agent, success=False, email=email, reason=str(e))
        return RedirectResponse(
            f"{_url('/register')}?error={str(e)}",
            status_code=status.HTTP_302_FOUND,
        )

    # 自动登录
    session_id = await create_session(user.id, user.username or user.email)
    log_register(user.id, client_ip, user_agent, success=True, email=email)

    response = RedirectResponse(_url("/"), status_code=status.HTTP_302_FOUND)
    cookie_opts = get_cookie_settings(request)
    response.set_cookie(key="unisso_session", value=session_id, **cookie_opts)
    return response


@router.get("/api/auth/me", response_model=UserResponse)
async def api_me(user: User = Depends(require_token_user)):
    """获取当前用户信息（API）"""
    return UserResponse.model_validate(user)


# ==================== 外部身份绑定 API ====================

@router.get("/api/identities", response_model=list[UserIdentityResponse])
async def list_identities(
    user: User = Depends(require_auth_user),
    db: AsyncSession = Depends(get_db),
):
    """列出当前用户绑定的外部身份"""
    identities = await get_user_identities_by_user(db, user.id)
    return [UserIdentityResponse.model_validate(i) for i in identities]


@router.post("/api/identities", response_model=UserIdentityResponse)
async def create_identity(
    data: UserIdentityCreate,
    user: User = Depends(require_auth_user),
    db: AsyncSession = Depends(get_db),
):
    """绑定外部身份"""
    identity = await bind_identity(db, user.id, data)
    await db.commit()
    return UserIdentityResponse.model_validate(identity)


@router.delete("/api/identities/{identity_id}")
async def delete_identity(
    identity_id: str,
    user: User = Depends(require_auth_user),
    db: AsyncSession = Depends(get_db),
):
    """解绑外部身份"""
    ok = await unbind_identity(db, user.id, identity_id)
    if not ok:
        raise HTTPException(status_code=404, detail="绑定记录不存在")
    await db.commit()
    return {"ok": True, "message": "已解绑"}


# ==================== OAuth2 端点（加固版） ====================

@router.post("/api/oauth/authorize")
async def api_authorize(
    request: Request,
    client_id: str = Form(...),
    redirect_uri: str = Form(...),
    scope: str = Form("openid"),
    state: Optional[str] = Form(None),
    code_challenge: Optional[str] = Form(None),
    code_challenge_method: Optional[str] = Form("S256"),
    approved: bool = Form(False),
    scopes: Optional[list] = Form(None),
    user: User = Depends(require_user),
    db: AsyncSession = Depends(get_db),
):
    """用户确认授权（支持按 scope 勾选）"""
    if not approved:
        error_redirect = f"{redirect_uri}?error=access_denied"
        if state:
            error_redirect += f"&state={state}"
        return RedirectResponse(error_redirect, status_code=status.HTTP_302_FOUND)

    # 勾选的 scope 优先；未勾选任何项时回退到请求的 scope
    approved_scopes = scopes if scopes else scopes_to_list(scope)

    req = AuthorizeRequest(
        response_type="code",
        client_id=client_id,
        redirect_uri=redirect_uri,
        scope=scope,
        state=state,
        code_challenge=code_challenge,
        code_challenge_method=code_challenge_method,
    )

    client_ip = _get_client_ip(request)
    code = await issue_code_for_consent(db, req, user, approved_scopes, client_ip=client_ip)
    await db.commit()

    redirect = f"{redirect_uri}?code={code}"
    if state:
        redirect += f"&state={state}"
    return RedirectResponse(redirect, status_code=status.HTTP_302_FOUND)


@router.post("/api/oauth/token", response_model=TokenResponse)
async def api_token(
    request: Request,
    grant_type: str = Form(...),
    code: Optional[str] = Form(None),
    redirect_uri: Optional[str] = Form(None),
    client_id: Optional[str] = Form(None),
    client_secret: Optional[str] = Form(None),
    refresh_token: Optional[str] = Form(None),
    code_verifier: Optional[str] = Form(None),
    scope: Optional[str] = Form(None),
    db: AsyncSession = Depends(get_db),
):
    """OAuth2 Token 端点"""
    client_ip = _get_client_ip(request)
    req = TokenRequest(
        grant_type=grant_type,
        code=code,
        redirect_uri=redirect_uri,
        client_id=client_id,
        client_secret=client_secret,
        refresh_token=refresh_token,
        code_verifier=code_verifier,
        scope=scope,
    )
    return await handle_token(db, req, client_ip=client_ip)


@router.post("/api/oauth/introspect", response_model=IntrospectResponse)
async def api_introspect(
    token: str = Form(...),
    token_type_hint: Optional[str] = Form(None),
    db: AsyncSession = Depends(get_db),
):
    """Token 验证端点"""
    return await introspect_token(token, db)


@router.get("/api/oauth/userinfo", response_model=UserInfo, response_model_exclude_none=True)
async def api_userinfo(
    request: Request,
    user: User = Depends(require_token_user),
    db: AsyncSession = Depends(get_db),
):
    """OIDC UserInfo 端点（按 effective scopes 过滤 claims）"""
    auth = request.headers.get("authorization", "")
    token = auth.removeprefix("Bearer ").strip()
    return await get_userinfo_from_token(db, token)


@router.get("/.well-known/openid-configuration")
async def openid_configuration(request: Request):
    """OIDC 发现端点"""
    base_url = str(request.base_url).rstrip("/")
    return {
        "issuer": base_url,
        "authorization_endpoint": f"{base_url}/authorize",
        "token_endpoint": f"{base_url}/api/oauth/token",
        "userinfo_endpoint": f"{base_url}/api/oauth/userinfo",
        "introspection_endpoint": f"{base_url}/api/oauth/introspect",
        "jwks_uri": f"{base_url}/.well-known/jwks.json",
        "response_types_supported": ["code"],
        "grant_types_supported": ["authorization_code", "refresh_token"],
        "subject_types_supported": ["public"],
        "id_token_signing_alg_values_supported": ["HS256"],
        "scopes_supported": supported_scopes(),
        "token_endpoint_auth_methods_supported": ["client_secret_post", "client_secret_basic"],
        "code_challenge_methods_supported": ["S256", "plain"],
    }


@router.get("/.well-known/jwks.json")
async def jwks():
    """JWKS 端点"""
    import hashlib
    kid = hashlib.sha256(_settings.secret_key.encode()).hexdigest()[:16]
    return {
        "keys": [{
            "kty": "oct",
            "kid": kid,
            "alg": "HS256",
            "k": _settings.secret_key,
            "use": "sig",
        }]
    }


# ==================== 应用管理 API ====================

@router.get("/api/apps", response_model=list[ApplicationResponse])
async def list_apps(
    user: User = Depends(require_auth_user),
    db: AsyncSession = Depends(get_db),
):
    """列出应用"""
    if user.is_admin:
        result = await db.execute(select(Application))
    else:
        result = await db.execute(
            select(Application).where(Application.owner_id == user.id)
        )
    return [ApplicationResponse.model_validate(a) for a in result.scalars().all()]


@router.post("/api/apps", response_model=ApplicationResponse)
async def create_app(
    data: ApplicationCreate,
    user: User = Depends(require_auth_user),
    db: AsyncSession = Depends(get_db),
):
    """注册新应用"""
    raw_secret = generate_client_secret()
    app = Application(
        id=str(__import__('uuid').uuid4()),
        client_id=generate_client_id(),
        client_secret=hash_password(raw_secret),
        name=data.name,
        description=data.description,
        redirect_uris=data.redirect_uris,
        allowed_scopes=data.allowed_scopes,
        homepage_url=data.homepage_url,
        callback_url=data.callback_url,
        is_confidential=data.is_confidential,
        owner_id=user.id,
        is_active=True,
    )
    db.add(app)
    await db.flush()
    await db.refresh(app)
    # 应用创建即返回 client_secret，必须先落库（否则客户端立即发起 /authorize 会查不到应用）
    await db.commit()

    response = ApplicationResponse.model_validate(app)
    response.client_secret = raw_secret
    return response


@router.get("/api/apps/{app_id}", response_model=ApplicationResponse)
async def get_app(
    app_id: str,
    user: User = Depends(require_auth_user),
    db: AsyncSession = Depends(get_db),
):
    """获取应用详情"""
    result = await db.execute(select(Application).where(Application.id == app_id))
    app = result.scalar_one_or_none()
    if not app:
        raise HTTPException(status_code=404, detail="应用不存在")
    if not user.is_admin and app.owner_id != user.id:
        raise HTTPException(status_code=403, detail="无权访问")
    return ApplicationResponse.model_validate(app)


@router.put("/api/apps/{app_id}", response_model=ApplicationResponse)
async def update_app(
    app_id: str,
    data: ApplicationUpdate,
    user: User = Depends(require_auth_user),
    db: AsyncSession = Depends(get_db),
):
    """更新应用"""
    result = await db.execute(select(Application).where(Application.id == app_id))
    app = result.scalar_one_or_none()
    if not app:
        raise HTTPException(status_code=404, detail="应用不存在")
    if not user.is_admin and app.owner_id != user.id:
        raise HTTPException(status_code=403, detail="无权访问")

    if data.name is not None:
        app.name = data.name
    if data.description is not None:
        app.description = data.description
    if data.redirect_uris is not None:
        app.redirect_uris = data.redirect_uris
    if data.allowed_scopes is not None:
        app.allowed_scopes = data.allowed_scopes
    if data.homepage_url is not None:
        app.homepage_url = data.homepage_url
    if data.callback_url is not None:
        app.callback_url = data.callback_url
    if data.is_active is not None:
        app.is_active = data.is_active

    await db.flush()
    await db.refresh(app)
    await db.commit()
    return ApplicationResponse.model_validate(app)


@router.delete("/api/apps/{app_id}")
async def delete_app(
    app_id: str,
    user: User = Depends(require_auth_user),
    db: AsyncSession = Depends(get_db),
):
    """删除应用"""
    result = await db.execute(select(Application).where(Application.id == app_id))
    app = result.scalar_one_or_none()
    if not app:
        raise HTTPException(status_code=404, detail="应用不存在")
    if not user.is_admin and app.owner_id != user.id:
        raise HTTPException(status_code=403, detail="无权访问")

    await db.delete(app)
    await db.flush()
    await db.commit()
    return {"ok": True, "message": "应用已删除"}


@router.post("/api/apps/{app_id}/reset-secret")
async def reset_app_secret(
    app_id: str,
    user: User = Depends(require_auth_user),
    db: AsyncSession = Depends(get_db),
):
    """重置应用密钥"""
    result = await db.execute(select(Application).where(Application.id == app_id))
    app = result.scalar_one_or_none()
    if not app:
        raise HTTPException(status_code=404, detail="应用不存在")
    if not user.is_admin and app.owner_id != user.id:
        raise HTTPException(status_code=403, detail="无权访问")

    new_secret = generate_client_secret()
    app.client_secret = hash_password(new_secret)
    await db.flush()
    await db.commit()
    return {"ok": True, "client_id": app.client_id, "client_secret": new_secret}


# ==================== 用户授权管理 API ====================

@router.get("/api/consents", response_model=list)
async def list_consents(
    user: User = Depends(require_auth_user),
    db: AsyncSession = Depends(get_db),
):
    """列出当前用户的授权记录"""
    result = await db.execute(
        select(UserConsent)
        .options(selectinload(UserConsent.application))
        .where(
            and_(UserConsent.user_id == user.id, UserConsent.is_active == True)  # noqa: E712
        )
    )
    consents = result.scalars().all()
    return [
        {
            "id": c.id,
            "application": {
                "id": c.application.id,
                "client_id": c.application.client_id,
                "name": c.application.name,
                "icon": c.application.icon,
            },
            "scopes": c.scopes,
            "granted_at": c.granted_at.isoformat(),
        }
        for c in consents
    ]


@router.put("/api/consents/{consent_id}")
async def update_consent(
    consent_id: str,
    data: ConsentUpdate,
    user: User = Depends(require_token_user),
    db: AsyncSession = Depends(get_db),
):
    """更新授权范围"""
    result = await db.execute(
        select(UserConsent)
        .options(selectinload(UserConsent.application))
        .where(
            and_(UserConsent.id == consent_id, UserConsent.user_id == user.id)
        )
    )
    consent = result.scalar_one_or_none()
    if not consent:
        raise HTTPException(status_code=404, detail="授权记录不存在")

    app = consent.application
    valid_scopes = compute_effective_scopes(data.scopes, app.allowed_scopes)
    consent.scopes = valid_scopes
    await db.flush()
    await db.commit()
    return {"ok": True, "scopes": valid_scopes}


@router.delete("/api/consents/{consent_id}")
async def delete_consent(
    consent_id: str,
    user: User = Depends(require_auth_user),
    db: AsyncSession = Depends(get_db),
):
    """撤销授权"""
    result = await db.execute(
        select(UserConsent)
        .options(selectinload(UserConsent.application))
        .where(
            and_(UserConsent.id == consent_id, UserConsent.user_id == user.id)
        )
    )
    consent = result.scalar_one_or_none()
    if not consent:
        raise HTTPException(status_code=404, detail="授权记录不存在")

    # 撤销 consent 并联动撤销该应用下该用户的全部 token
    await revoke_consent(db, user.id, consent.application_id, client_id=consent.application.client_id)
    await db.flush()
    await db.commit()
    return {"ok": True, "message": "授权已撤销，相关令牌已失效"}


@router.post("/api/consents/{consent_id}")
async def delete_consent_post(
    consent_id: str,
    request: Request,
    user: User = Depends(require_auth_user),
    db: AsyncSession = Depends(get_db),
):
    """撤销授权（Form 兼容）"""
    form = await request.form()
    if form.get("_method") == "delete":
        return await delete_consent(consent_id, user, db)
    raise HTTPException(status_code=400, detail="无效的请求")


# ==================== 用户管理 API（管理员，带审计日志） ====================

@router.get("/api/admin/users", response_model=list[UserResponse])
async def admin_list_users(
    request: Request,
    user: User = Depends(require_admin),
    db: AsyncSession = Depends(get_db),
):
    """管理员列出所有用户"""
    result = await db.execute(select(User))
    return [UserResponse.model_validate(u) for u in result.scalars().all()]


@router.post("/api/admin/users", response_model=UserResponse)
async def admin_create_user(
    request: Request,
    data: UserCreate,
    is_admin: bool = False,
    current_user: User = Depends(require_admin),
    db: AsyncSession = Depends(get_db),
):
    """管理员创建用户"""
    existing = await get_user_by_email(db, data.email)
    if existing:
        raise HTTPException(status_code=400, detail="邮箱已存在")

    user = await create_user(db, data, is_admin=is_admin)
    await db.commit()

    log_admin_action(
        current_user.id, _get_client_ip(request),
        "create_user", "user", user.id,
        {"email": data.email, "is_admin": is_admin}
    )
    return UserResponse.model_validate(user)


@router.put("/api/admin/users/{user_id}")
async def admin_update_user(
    request: Request,
    user_id: str,
    data: UserUpdate,
    current_user: User = Depends(require_admin),
    db: AsyncSession = Depends(get_db),
):
    """管理员更新用户"""
    user = await get_user_by_id(db, user_id)
    if not user:
        raise HTTPException(status_code=404, detail="用户不存在")

    updated = await update_user(db, user, data)
    await db.commit()

    log_admin_action(
        current_user.id, _get_client_ip(request),
        "update_user", "user", user_id,
        {"fields": [k for k, v in data.model_dump(exclude_unset=True).items() if v is not None]}
    )
    return UserResponse.model_validate(updated)


@router.delete("/api/admin/users/{user_id}")
async def admin_delete_user(
    request: Request,
    user_id: str,
    current_user: User = Depends(require_admin),
    db: AsyncSession = Depends(get_db),
):
    """管理员删除用户"""
    user = await get_user_by_id(db, user_id)
    if not user:
        raise HTTPException(status_code=404, detail="用户不存在")
    if user.id == current_user.id:
        raise HTTPException(status_code=400, detail="不能删除自己")

    await db.delete(user)
    await db.flush()

    log_admin_action(
        current_user.id, _get_client_ip(request),
        "delete_user", "user", user_id,
        {"email": user.email}
    )
    return {"ok": True, "message": "用户已删除"}


# ==================== 文件上传（MinIO，加固版） ====================

@router.post("/api/me/avatar")
async def upload_avatar(
    request: Request,
    user: User = Depends(require_auth_user),
    db: AsyncSession = Depends(get_db),
):
    """上传用户头像到 MinIO（MIME 类型白名单 + 魔数检查）"""
    from fastapi import UploadFile
    form = await request.form()
    file: UploadFile = form.get("file")
    if not file:
        raise HTTPException(status_code=400, detail="未提供文件")

    content = await file.read()
    if len(content) > 5 * 1024 * 1024:
        raise HTTPException(status_code=400, detail="文件超过 5MB")

    # MIME 类型 + 魔数双重验证
    if not _validate_image_file(file.content_type, content):
        raise HTTPException(status_code=400, detail="只允许上传图片文件（JPEG/PNG/GIF/WebP/SVG）")

    object_name = await upload_file(
        content,
        f"avatar_{user.id}_{file.filename}",
        content_type=file.content_type or "image/png",
        prefix="avatars",
    )
    if not object_name:
        raise HTTPException(status_code=500, detail="上传失败，MinIO 未配置或不可用")

    user.avatar = object_name
    await db.flush()
    await db.commit()

    url = await get_file_url(object_name, expires=3600)
    return {"ok": True, "object_name": object_name, "url": url}


@router.get("/api/me/avatar")
async def get_avatar_url(
    user: User = Depends(require_auth_user),
):
    """获取当前用户头像临时 URL"""
    if not user.avatar:
        return {"ok": False, "reason": "未设置头像"}
    url = await get_file_url(user.avatar, expires=3600)
    return {"ok": True, "url": url}


@router.post("/api/apps/{app_id}/icon")
async def upload_app_icon(
    request: Request,
    app_id: str,
    user: User = Depends(require_token_user),
    db: AsyncSession = Depends(get_db),
):
    """上传应用图标到 MinIO（MIME 类型白名单 + 魔数检查）"""
    from fastapi import UploadFile
    result = await db.execute(select(Application).where(Application.id == app_id))
    app = result.scalar_one_or_none()
    if not app:
        raise HTTPException(status_code=404, detail="应用不存在")
    if not user.is_admin and app.owner_id != user.id:
        raise HTTPException(status_code=403, detail="无权访问")

    form = await request.form()
    file: UploadFile = form.get("file")
    if not file:
        raise HTTPException(status_code=400, detail="未提供文件")

    content = await file.read()
    if len(content) > 2 * 1024 * 1024:
        raise HTTPException(status_code=400, detail="文件超过 2MB")

    # MIME 类型 + 魔数双重验证
    if not _validate_image_file(file.content_type, content):
        raise HTTPException(status_code=400, detail="只允许上传图片文件（JPEG/PNG/GIF/WebP/SVG）")

    object_name = await upload_file(
        content,
        f"icon_{app_id}_{file.filename}",
        content_type=file.content_type or "image/png",
        prefix="app_icons",
    )
    if not object_name:
        raise HTTPException(status_code=500, detail="上传失败，MinIO 未配置或不可用")

    app.icon = object_name
    await db.flush()
    await db.commit()

    url = await get_file_url(object_name, expires=3600)
    return {"ok": True, "object_name": object_name, "url": url}


@router.get("/api/files/{object_name:path}")
async def get_file(
    object_name: str,
    expires: int = Query(3600, ge=60, le=86400),
):
    """获取 MinIO 文件临时访问 URL"""
    url = await get_file_url(object_name, expires=expires)
    if not url:
        raise HTTPException(status_code=404, detail="文件不存在或 MinIO 未配置")
    return {"ok": True, "url": url}


# ==================== 健康检查 ====================

@router.get("/health")
async def health_check():
    """健康检查端点"""
    return {"ok": True, "service": "unisso", "version": _settings.app_version}


@router.get("/api/health")
async def api_health_check():
    """API 健康检查"""
    return {"ok": True, "service": "unisso", "version": _settings.app_version}
