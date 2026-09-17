"""UniSSO 安全工具 - 加固版

包含：
- 密码哈希（bcrypt，限制 72 字节）
- JWT 令牌（固定算法、aud/iss 声明、密钥长度验证）
- PKCE 挑战生成与验证
- 密码复杂度策略
- CSRF Token 生成
"""
import re
import secrets
import hashlib
import base64
from datetime import datetime, timedelta, timezone
from typing import Optional, Dict, Any, List

import jwt
from passlib.context import CryptContext

from app.config import get_settings
from app.token_keys import canonical_issuer, signing_material

_settings = get_settings()
pwd_context = CryptContext(schemes=["bcrypt"], deprecated="auto")

# 密码复杂度正则
PASSWORD_MIN_LENGTH = 8
PASSWORD_MAX_LENGTH = 128
COMMON_WEAK_PASSWORDS = {
    "password", "123456", "12345678", "qwerty", "abc123",
    "monkey", "letmein", "dragon", "111111", "baseball",
    "iloveyou", "trustno1", "sunshine", "princess", "admin",
    "welcome", "shadow", "ashley", "football", "jesus",
    "michael", "ninja", "mustang", "password1", "123456789",
    "adobe123", "admin123", "letmein1", "photoshop", "1234567",
}


def verify_password(plain_password: str, hashed_password: str) -> bool:
    """验证密码（bcrypt 限制 72 字节，统一截断）"""
    return pwd_context.verify(plain_password.encode('utf-8')[:72], hashed_password)


def hash_password(password: str) -> str:
    """哈希密码（bcrypt 限制 72 字节，统一截断）"""
    return pwd_context.hash(password.encode('utf-8')[:72])


# ==================== 密码复杂度策略 ====================

class PasswordValidationError(ValueError):
    """密码验证错误"""
    pass


def validate_password_strength(password: str) -> None:
    """验证密码强度，不满足则抛出 PasswordValidationError

    要求：
    - 最少 8 位，最多 128 位
    - 大写字母、小写字母、数字、符号中至少包含两类
    """
    if len(password) < PASSWORD_MIN_LENGTH:
        raise PasswordValidationError(f"密码长度不能少于 {PASSWORD_MIN_LENGTH} 位")

    if len(password) > PASSWORD_MAX_LENGTH:
        raise PasswordValidationError(f"密码长度不能超过 {PASSWORD_MAX_LENGTH} 位")

    category_count = sum((
        bool(re.search(r'[A-Z]', password)),
        bool(re.search(r'[a-z]', password)),
        bool(re.search(r'\d', password)),
        bool(re.search(r'[^A-Za-z0-9\s]', password)),
    ))
    if category_count < 2:
        raise PasswordValidationError("密码至少需要包含两类字符")

    # 检查常见弱密码
    if password.lower() in COMMON_WEAK_PASSWORDS:
        raise PasswordValidationError("密码过于常见，请更换")

    # 检查连续字符
    if re.search(r'(.)\1{3,}', password):  # 同一字符连续 4 次以上
        raise PasswordValidationError("密码不能包含连续 4 个相同字符")

    # 检查连续数字序列
    if re.search(r'0123|1234|2345|3456|4567|5678|6789|9876|8765|7654|6543|5432|4321|3210', password):
        raise PasswordValidationError("密码不能包含连续数字序列")


def check_password_not_common(password: str) -> bool:
    """检查密码是否在常见弱密码列表中（额外检查）"""
    return password.lower() not in COMMON_WEAK_PASSWORDS


# ==================== 密钥安全 ====================

def validate_secret_key() -> None:
    """验证 secret_key 强度，生产环境必须修改"""
    default_keys = [
        "change-me-in-production-unisso-secret-key-2024",
        "secret",
        "your-secret-key",
        "change-me",
    ]
    key = _settings.secret_key

    if not key or len(key) < 32:
        raise RuntimeError(
            "SECRET_KEY 长度必须至少 32 位。"
            "请设置环境变量 UNISSO_SECRET_KEY 为一个强随机字符串。"
        )

    if key in default_keys:
        raise RuntimeError(
            "正在使用默认的 SECRET_KEY，这在生产环境极其危险！"
            "请设置环境变量 UNISSO_SECRET_KEY 为一个强随机字符串（如: openssl rand -base64 48）"
        )


# ==================== 客户端凭证生成 ====================

def generate_client_id() -> str:
    """生成客户端 ID"""
    return f"client_{secrets.token_urlsafe(16)}"


def generate_client_secret() -> str:
    """生成客户端密钥"""
    return secrets.token_urlsafe(32)


def generate_authorization_code() -> str:
    """生成授权码"""
    return secrets.token_urlsafe(32)


def generate_token() -> str:
    """生成随机令牌"""
    return secrets.token_urlsafe(32)


def generate_jti() -> str:
    """生成 JWT 唯一标识（用于落库撤销）"""
    return secrets.token_urlsafe(16)


# ==================== PKCE ====================

def generate_pkce_challenge() -> tuple[str, str]:
    """生成 PKCE code_verifier 和 code_challenge"""
    verifier = base64.urlsafe_b64encode(secrets.token_bytes(32)).decode("utf-8").rstrip("=")
    challenge = base64.urlsafe_b64encode(
        hashlib.sha256(verifier.encode()).digest()
    ).decode("utf-8").rstrip("=")
    return verifier, challenge


def verify_pkce_challenge(verifier: str, challenge: str, method: str = "S256") -> bool:
    """验证 PKCE challenge"""
    if method != "S256":
        return False
    expected = base64.urlsafe_b64encode(
        hashlib.sha256(verifier.encode()).digest()
    ).decode("utf-8").rstrip("=")
    return secrets.compare_digest(expected, challenge)
    return False


# ==================== CSRF Token ====================

def generate_csrf_token() -> str:
    """生成 CSRF token"""
    return secrets.token_urlsafe(32)


def verify_csrf_token(token: str, expected: str) -> bool:
    """验证 CSRF token"""
    if not token or not expected:
        return False
    return secrets.compare_digest(token, expected)


# ==================== JWT 令牌（加固版） ====================

# 固定允许的算法，防止 algorithm confusion attack
ALLOWED_ALGORITHMS = ["HS256"]


def _get_issuer() -> str:
    """获取 JWT issuer"""
    return canonical_issuer(_settings)


def access_token_expiry(now: Optional[datetime] = None) -> datetime:
    """access token 过期时间（单一事实来源，签发与落库共用）"""
    now = now or datetime.now(timezone.utc)
    return now + timedelta(minutes=_settings.jwt_access_token_expire_minutes)


def refresh_token_expiry(now: Optional[datetime] = None) -> datetime:
    """refresh token 过期时间（单一事实来源，签发与落库共用）"""
    now = now or datetime.now(timezone.utc)
    return now + timedelta(days=_settings.jwt_refresh_token_expire_days)


def create_access_token(
    subject: str,
    client_id: str,
    scope: str = "",
    jti: Optional[str] = None,
) -> str:
    """创建 JWT access token（加固版）

    禁止携带任何 PII（姓名/邮箱/学号等），仅包含身份指向与授权信息：
    - iss / aud / sub / client_id / scope / type / iat / exp / jti
    - jti 落库用于主动撤销
    """
    now = datetime.now(timezone.utc)

    payload = {
        "sub": subject,
        "client_id": client_id,
        "scope": scope,
        "type": "access",
        "iat": now,
        "exp": access_token_expiry(now),
        "jti": jti or generate_jti(),
        "iss": _get_issuer(),
        "aud": _get_issuer() + "/api/oauth/userinfo",
    }

    private, _ = signing_material(_settings)
    return jwt.encode(payload, private, algorithm=_settings.jwt_algorithm, headers={"kid": _settings.signing_key_id})


def create_refresh_token(
    subject: str,
    client_id: str,
    scope: str = "",
    jti: Optional[str] = None,
) -> str:
    """创建 JWT refresh token（加固版）"""
    now = datetime.now(timezone.utc)

    payload = {
        "sub": subject,
        "client_id": client_id,
        "scope": scope,
        "type": "refresh",
        "iat": now,
        "exp": refresh_token_expiry(now),
        "jti": jti or generate_jti(),
        "iss": _get_issuer(),
        "aud": _get_issuer() + "/api/oauth/token",
    }

    private, _ = signing_material(_settings)
    return jwt.encode(payload, private, algorithm=_settings.jwt_algorithm, headers={"kid": _settings.signing_key_id})


def create_id_token(
    user_id: str,
    client_id: str,
    claims: Optional[Dict[str, Any]] = None,
) -> str:
    """创建 OIDC ID Token

    claims 由调用方按 effective scopes 过滤后传入（隐私信息仅放在 id_token
    与 userinfo，不进入 access_token）。
    """
    now = datetime.now(timezone.utc)

    payload = {
        "sub": user_id,
        "type": "id",
        "iat": now,
        "exp": now + timedelta(minutes=_settings.jwt_id_token_expire_minutes),
        "jti": generate_jti(),
        "iss": _get_issuer(),
        "aud": client_id,
    }
    if claims:
        # 防止覆盖标准声明
        payload.update({k: v for k, v in claims.items() if k not in payload})

    private, _ = signing_material(_settings)
    return jwt.encode(payload, private, algorithm=_settings.jwt_algorithm, headers={"kid": _settings.signing_key_id})


def decode_token(token: str, audience: Optional[str] = None, token_type: str = "access") -> Optional[Dict[str, Any]]:
    """Validate signature, issuer, intended resource and token purpose together."""
    try:
        if token_type not in ("access", "refresh", "id"):
            return None
        if token_type == "id" and not audience:
            return None
        expected = audience or _get_issuer() + ("/api/oauth/token" if token_type == "refresh" else "/api/oauth/userinfo")
        _, public = signing_material(_settings)
        header = jwt.get_unverified_header(token)
        if header.get("kid") != _settings.signing_key_id:
            return None
        payload = jwt.decode(token, public, algorithms=[_settings.jwt_algorithm],
            issuer=_get_issuer(), audience=expected,
            options={"require": ["exp", "iat", "iss", "aud", "sub", "jti", "type"]})
        if payload.get("type") != token_type or not payload.get("jti") or not payload.get("sub"):
            return None
        if token_type != "id" and not payload.get("client_id"):
            return None
        return payload
    except (jwt.InvalidTokenError, ValueError, TypeError):
        return None


def get_token_expiry(token: str) -> Optional[datetime]:
    payload = decode_token(token) or decode_token(token, token_type="refresh")
    return datetime.fromtimestamp(payload["exp"], timezone.utc) if payload else None


# ==================== Scope 工具 ====================

def scopes_to_list(scope: str) -> List[str]:
    """将空格分隔的 scope 转为列表"""
    return [s.strip() for s in scope.split() if s.strip()]


def list_to_scopes(scopes: List[str]) -> str:
    """将 scope 列表转为空格分隔字符串"""
    return " ".join(scopes)
