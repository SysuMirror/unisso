"""UniSSO 认证模块 - 邮箱登录、Session 管理、外部身份绑定（加固版）

加固内容：
- Cookie: secure, SameSite, Path, HttpOnly
- CSRF Token 生成与验证
- 密码复杂度策略
- 登录失败次数限制
- 管理员默认密码检测
- 审计日志集成
"""
import uuid
import re
from datetime import datetime, timezone
from typing import Optional, List

from fastapi import Request, Depends, HTTPException, status
from fastapi.security import HTTPBearer, HTTPAuthorizationCredentials
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.database import get_db
from app.models import User, Role, Permission, UserIdentity
from app.schemas import UserCreate, UserLogin, UserUpdate, UserIdentityCreate
from app.security import (
    verify_password, hash_password, decode_token,
    validate_password_strength, generate_csrf_token, verify_csrf_token,
)
from app.redis_client import redis_client
from app.config import get_settings
from app.audit import log_login, log_register, log_security_alert

_settings = get_settings()
security_bearer = HTTPBearer(auto_error=False)


def _auth_url(path: str) -> str:
    """生成带 root_path 前缀的 URL"""
    rp = _settings.root_path.rstrip("/")
    if rp:
        return f"{rp}{path}"
    return path


# Sysu 邮箱正则
SYSU_EMAIL_RE = re.compile(r"^[a-zA-Z0-9._%+-]+@mail\d*\.sysu\.edu\.cn$")


def is_sysu_email(email: str) -> bool:
    return bool(SYSU_EMAIL_RE.match(email))


# ==================== 用户 CRUD ====================

async def get_user_by_email(db: AsyncSession, email: str) -> Optional[User]:
    result = await db.execute(select(User).where(User.email == email))
    return result.scalar_one_or_none()


async def get_user_by_username(db: AsyncSession, username: str) -> Optional[User]:
    result = await db.execute(select(User).where(User.username == username))
    return result.scalar_one_or_none()


async def get_user_by_id(db: AsyncSession, user_id: str) -> Optional[User]:
    result = await db.execute(select(User).where(User.id == user_id))
    return result.scalar_one_or_none()


async def create_user(db: AsyncSession, user_data: UserCreate, is_admin: bool = False) -> User:
    """创建用户（带密码强度验证）"""
    # 验证密码强度
    validate_password_strength(user_data.password)

    user = User(
        id=str(uuid.uuid4()),
        email=user_data.email,
        password_hash=hash_password(user_data.password),
        username=user_data.username,
        full_name=user_data.full_name,
        email_verified=False,
        is_admin=is_admin,
    )
    db.add(user)
    await db.flush()
    await db.refresh(user)
    return user


async def update_user(db: AsyncSession, user: User, data: UserUpdate) -> User:
    if data.username is not None:
        user.username = data.username
    if data.full_name is not None:
        user.full_name = data.full_name
    if data.avatar is not None:
        user.avatar = data.avatar
    if data.is_active is not None:
        user.is_active = data.is_active
    await db.flush()
    await db.refresh(user)
    return user


async def authenticate_user(db: AsyncSession, email: str, password: str) -> Optional[User]:
    """用邮箱+密码认证用户"""
    user = await get_user_by_email(db, email)
    if not user:
        return None
    if not user.is_active:
        return None
    if not verify_password(password, user.password_hash):
        return None
    return user


# ==================== 登录失败限制 ====================

async def check_login_attempts(client_ip: str) -> tuple[bool, int]:
    """检查登录尝试次数，返回 (是否允许, 剩余秒数)

    5 分钟内失败 5 次则锁定 15 分钟。
    """
    key = f"login_attempts:{client_ip}"
    attempts = await redis_client.get_json(key) or {"count": 0, "locked_until": 0}

    now = datetime.now(timezone.utc).timestamp()

    # 检查是否仍在锁定中
    if attempts.get("locked_until", 0) > now:
        remaining = int(attempts["locked_until"] - now)
        return False, remaining

    # 如果上次失败超过 5 分钟，重置计数
    last_attempt = attempts.get("last_attempt", 0)
    if now - last_attempt > 300:  # 5 分钟
        attempts = {"count": 0, "locked_until": 0}

    return True, 0


async def record_login_failure(client_ip: str):
    """记录登录失败"""
    key = f"login_attempts:{client_ip}"
    attempts = await redis_client.get_json(key) or {"count": 0, "locked_until": 0}

    now = datetime.now(timezone.utc).timestamp()
    attempts["count"] = attempts.get("count", 0) + 1
    attempts["last_attempt"] = now

    # 失败 5 次锁定 15 分钟
    if attempts["count"] >= 5:
        attempts["locked_until"] = now + 900  # 15 分钟

    await redis_client.set_json(key, attempts, expire=900)


async def clear_login_attempts(client_ip: str):
    """清除登录失败记录（登录成功时调用）"""
    key = f"login_attempts:{client_ip}"
    await redis_client.delete(key)


# ==================== 外部身份绑定 ====================

async def get_user_identity(
    db: AsyncSession,
    provider: str,
    provider_user_id: str,
) -> Optional[UserIdentity]:
    result = await db.execute(
        select(UserIdentity).where(
            UserIdentity.provider == provider,
            UserIdentity.provider_user_id == provider_user_id,
            UserIdentity.is_active == True,
        )
    )
    return result.scalar_one_or_none()


async def get_user_identities_by_user(
    db: AsyncSession,
    user_id: str,
) -> List[UserIdentity]:
    result = await db.execute(
        select(UserIdentity).where(
            UserIdentity.user_id == user_id,
            UserIdentity.is_active == True,
        )
    )
    return list(result.scalars().all())


async def bind_identity(
    db: AsyncSession,
    user_id: str,
    data: UserIdentityCreate,
) -> UserIdentity:
    existing = await get_user_identity(db, data.provider, data.provider_user_id)
    if existing:
        if existing.user_id != user_id:
            raise HTTPException(
                status_code=status.HTTP_400_BAD_REQUEST,
                detail=f"该 {data.provider} 账号已被其他用户绑定",
            )
        existing.provider_username = data.provider_username
        existing.provider_email = data.provider_email
        existing.provider_avatar = data.provider_avatar
        existing.extra_data = data.extra_data
        existing.is_active = True
        await db.flush()
        await db.refresh(existing)
        return existing

    identity = UserIdentity(
        id=str(uuid.uuid4()),
        user_id=user_id,
        provider=data.provider,
        provider_user_id=data.provider_user_id,
        provider_username=data.provider_username,
        provider_email=data.provider_email,
        provider_avatar=data.provider_avatar,
        extra_data=data.extra_data,
        is_active=True,
    )
    db.add(identity)
    await db.flush()
    await db.refresh(identity)
    return identity


async def unbind_identity(
    db: AsyncSession,
    user_id: str,
    identity_id: str,
) -> bool:
    result = await db.execute(
        select(UserIdentity).where(
            UserIdentity.id == identity_id,
            UserIdentity.user_id == user_id,
        )
    )
    identity = result.scalar_one_or_none()
    if not identity:
        return False
    identity.is_active = False
    await db.flush()
    return True


# ==================== Session 管理（加固版） ====================

async def create_session(user_id: str, username: str, expires_hours: Optional[int] = None) -> str:
    session_id = f"sess_{uuid.uuid4().hex}"
    expires = expires_hours or _settings.session_expire_hours
    data = {
        "user_id": user_id,
        "username": username,
        "created_at": datetime.now(timezone.utc).isoformat(),
    }
    await redis_client.set_json(f"session:{session_id}", data, expire=expires * 3600)
    return session_id


async def get_session(session_id: str) -> Optional[dict]:
    if not session_id:
        return None
    return await redis_client.get_json(f"session:{session_id}")


async def delete_session(session_id: str):
    if session_id:
        await redis_client.delete(f"session:{session_id}")


def get_cookie_settings(request=None) -> dict:
    """获取加固的 Cookie 设置

    如果传入 request，根据请求动态判断 HTTPS（支持反向代理场景）。
    """
    is_secure = False
    if request is not None:
        from app.middleware import is_request_https
        is_secure = is_request_https(request)
    elif _settings.is_https:
        is_secure = True

    return {
        "httponly": True,
        "secure": is_secure,
        "samesite": "lax",
        "path": "/",
        "max_age": _settings.session_expire_hours * 3600,
    }


# ==================== CSRF Token 管理 ====================

async def create_csrf_token(session_id: str) -> str:
    """为 Session 创建 CSRF token"""
    token = generate_csrf_token()
    await redis_client.set_json(f"csrf:{session_id}", {"token": token}, expire=86400)
    return token


async def get_csrf_token(session_id: str) -> Optional[str]:
    """获取 Session 的 CSRF token"""
    if not session_id:
        return None
    data = await redis_client.get_json(f"csrf:{session_id}")
    return data.get("token") if data else None


async def validate_csrf_token(session_id: str, token: str) -> bool:
    """验证 CSRF token"""
    if not session_id or not token:
        return False
    expected = await get_csrf_token(session_id)
    if not expected:
        return False
    return verify_csrf_token(token, expected)


# ==================== 当前用户依赖 ====================

async def get_current_user_from_session(
    request: Request,
    db: AsyncSession = Depends(get_db),
) -> Optional[User]:
    session_id = request.cookies.get("unisso_session")
    if not session_id:
        return None
    session_data = await get_session(session_id)
    if not session_data:
        return None
    user = await get_user_by_id(db, session_data["user_id"])
    if user and user.is_active:
        return user
    return None


async def require_user(
    request: Request,
    db: AsyncSession = Depends(get_db),
) -> User:
    user = await get_current_user_from_session(request, db)
    if not user:
        raise HTTPException(
            status_code=status.HTTP_307_TEMPORARY_REDIRECT,
            headers={"Location": _auth_url("/login") + "?next=" + str(request.url.path)},
        )
    return user


async def require_admin(
    request: Request,
    db: AsyncSession = Depends(get_db),
) -> User:
    user = await require_user(request, db)
    if not user.is_admin:
        raise HTTPException(
            status_code=status.HTTP_403_FORBIDDEN,
            detail="需要管理员权限",
        )
    return user


async def get_current_user_from_token(
    credentials: HTTPAuthorizationCredentials = Depends(security_bearer),
    db: AsyncSession = Depends(get_db),
) -> Optional[User]:
    if not credentials:
        return None
    payload = decode_token(credentials.credentials)
    if not payload or payload.get("type") != "access":
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="无效的访问令牌",
            headers={"WWW-Authenticate": "Bearer"},
        )
    user_id = payload.get("sub")
    if not user_id:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="令牌中无用户标识",
        )
    user = await get_user_by_id(db, user_id)
    if not user or not user.is_active:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="用户不存在或已禁用",
        )
    return user


async def require_token_user(
    user: Optional[User] = Depends(get_current_user_from_token),
) -> User:
    if not user:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="需要登录",
            headers={"WWW-Authenticate": "Bearer"},
        )
    return user


async def get_current_user_mixed(
    request: Request,
    credentials: HTTPAuthorizationCredentials = Depends(security_bearer),
    db: AsyncSession = Depends(get_db),
) -> Optional[User]:
    """混合认证：先尝试 Session Cookie，再尝试 Bearer Token"""
    user = await get_current_user_from_session(request, db)
    if user:
        return user
    if credentials:
        return await get_current_user_from_token(credentials, db)
    return None


async def require_auth_user(
    request: Request,
    user: Optional[User] = Depends(get_current_user_mixed),
) -> User:
    """要求登录（支持 Session 或 Bearer Token）"""
    if not user:
        accept = request.headers.get("accept", "")
        if "application/json" in accept:
            raise HTTPException(
                status_code=status.HTTP_401_UNAUTHORIZED,
                detail="需要登录",
                headers={"WWW-Authenticate": "Bearer"},
            )
        raise HTTPException(
            status_code=status.HTTP_307_TEMPORARY_REDIRECT,
            headers={"Location": _auth_url("/login") + "?next=" + str(request.url.path)},
        )
    return user


# ==================== 权限检查 ====================

async def check_permission(user: User, resource: str, action: str) -> bool:
    if user.is_admin:
        return True
    for role in user.roles:
        for perm in role.permissions:
            if perm.resource == resource and perm.action == action:
                return True
            if perm.resource == "*" and perm.action == action:
                return True
            if perm.resource == resource and perm.action == "*":
                return True
            if perm.resource == "*" and perm.action == "*":
                return True
    return False


async def require_permission(resource: str, action: str):
    async def _check(user: User = Depends(require_user)) -> User:
        if user.is_admin:
            return user
        has_perm = await check_permission(user, resource, action)
        if not has_perm:
            raise HTTPException(
                status_code=status.HTTP_403_FORBIDDEN,
                detail=f"缺少权限: {resource}:{action}",
            )
        return user
    return _check


# ==================== 初始化数据（加固版） ====================

async def init_default_data(db: AsyncSession):
    """初始化默认权限、角色和管理员（支持增量，避免重复）"""
    from sqlalchemy import select

    # 1. 初始化权限（存在则跳过）
    perm_defs = [
        ("user:read", "user", "read", "查看用户信息"),
        ("user:write", "user", "write", "修改用户信息"),
        ("app:read", "app", "read", "查看应用"),
        ("app:write", "app", "write", "管理应用"),
        ("app:delete", "app", "delete", "删除应用"),
        ("admin:all", "*", "*", "所有管理员权限"),
    ]
    perms_map = {}
    for name, resource, action, desc in perm_defs:
        result = await db.execute(select(Permission).where(Permission.name == name))
        perm = result.scalar_one_or_none()
        if not perm:
            perm = Permission(
                id=str(uuid.uuid4()), name=name, resource=resource,
                action=action, description=desc,
            )
            db.add(perm)
            await db.flush()
            await db.refresh(perm)
        perms_map[name] = perm

    # 2. 初始化角色（存在则跳过）
    role_defs = [
        ("admin", "系统管理员", list(perms_map.values())),
        ("user", "普通用户", [perms_map["user:read"], perms_map["app:read"]]),
    ]
    roles_map = {}
    for name, desc, permissions in role_defs:
        result = await db.execute(select(Role).where(Role.name == name))
        role = result.scalar_one_or_none()
        if not role:
            role = Role(
                id=str(uuid.uuid4()), name=name, description=desc,
                is_system=True, permissions=permissions,
            )
            db.add(role)
            await db.flush()
            await db.refresh(role)
        roles_map[name] = role

    # 3. 检查是否已有用户（有任何用户则跳过管理员自动创建）
    user_result = await db.execute(select(User).limit(1))
    if user_result.scalar_one_or_none():
        return

    # 4. 创建管理员用户（必须通过环境变量设置）
    admin_email = _settings.admin_email
    admin_password = _settings.admin_password

    if not admin_email or not admin_password:
        import sys
        print(
            "\n" + "=" * 60 + "\n"
            "【提示】未设置管理员账号环境变量，跳过创建默认管理员。\n"
            "  如需自动创建管理员，请设置：\n"
            "    UNISSO_ADMIN_EMAIL=你的中大邮箱\n"
            "    UNISSO_ADMIN_PASSWORD=你的强密码\n"
            "=" * 60 + "\n",
            file=sys.stderr,
        )
        await db.commit()
        return

    if not is_sysu_email(admin_email):
        import sys
        print(
            f"【警告】管理员邮箱格式不正确: {admin_email}\n"
            "  管理员邮箱必须是 *.mail*.sysu.edu.cn 格式\n",
            file=sys.stderr,
        )
        await db.commit()
        return

    admin = User(
        id=str(uuid.uuid4()),
        email=admin_email,
        password_hash=hash_password(admin_password),
        username=_settings.admin_username or "admin",
        full_name="系统管理员",
        email_verified=True,
        is_active=True,
        is_admin=True,
        roles=[roles_map["admin"]],
    )
    db.add(admin)
    await db.commit()

    print(f"【UniSSO】管理员账号已创建: {admin_email}")
