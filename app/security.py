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

_settings = get_settings()
pwd_context = CryptContext(schemes=["bcrypt"], deprecated="auto")

# 密码复杂度正则
PASSWORD_MIN_LENGTH = 8
PASSWORD_MAX_LENGTH = 128


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
    - 包含至少 1 个大写字母
    - 包含至少 1 个小写字母
    - 包含至少 1 个数字
    - 包含至少 1 个特殊字符 (!@#$%^&*()_+-=[]{}|;:,.<>?)
    """
    if len(password) < PASSWORD_MIN_LENGTH:
        raise PasswordValidationError(f"密码长度不能少于 {PASSWORD_MIN_LENGTH} 位")

    if len(password) > PASSWORD_MAX_LENGTH:
        raise PasswordValidationError(f"密码长度不能超过 {PASSWORD_MAX_LENGTH} 位")

    if not re.search(r'[A-Z]', password):
        raise PasswordValidationError("密码必须包含至少 1 个大写字母")

    if not re.search(r'[a-z]', password):
        raise PasswordValidationError("密码必须包含至少 1 个小写字母")

    if not re.search(r'\d', password):
        raise PasswordValidationError("密码必须包含至少 1 个数字")

    if not re.search(r'[!@#$%^&*()_+\-=\[\]{}|;:,.<>?]', password):
        raise PasswordValidationError("密码必须包含至少 1 个特殊字符")

    # 检查常见弱密码
    common_weak_passwords = {
        "password", "12345678", "qwerty", "admin123",
        "password123", "123456789", "iloveyou", "sunshine",
    }
    if password.lower() in common_weak_passwords:
        raise PasswordValidationError("密码过于常见，请更换")

    # 检查连续字符
    if re.search(r'(.)\1{3,}', password):  # 同一字符连续 4 次以上
        raise PasswordValidationError("密码不能包含连续 4 个相同字符")

    # 检查连续数字序列
    if re.search(r'0123|1234|2345|3456|4567|5678|6789|9876|8765|7654|6543|5432|4321|3210', password):
        raise PasswordValidationError("密码不能包含连续数字序列")


def check_password_not_common(password: str) -> bool:
    """检查密码是否在常见弱密码列表中（额外检查）"""
    # 这里可以扩展为加载更大的弱密码字典
    common = {
        "password", "123456", "12345678", "qwerty", "abc123",
        "monkey", "letmein", "dragon", "111111", "baseball",
        "iloveyou", "trustno1", "sunshine", "princess", "admin",
        "welcome", "shadow", "ashley", "football", "jesus",
        "michael", "ninja", "mustang", "password1", "123456789",
        "adobe123", "admin123", "letmein1", "photoshop", "1234567",
    }
    return password.lower() not in common


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
    if method == "S256":
        expected = base64.urlsafe_b64encode(
            hashlib.sha256(verifier.encode()).digest()
        ).decode("utf-8").rstrip("=")
        return secrets.compare_digest(expected, challenge)
    elif method == "plain":
        return secrets.compare_digest(verifier, challenge)
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
    return _settings.public_url or "unisso"


def create_access_token(
    subject: str,
    client_id: str,
    scope: str = "",
    extra_claims: Optional[Dict[str, Any]] = None,
) -> str:
    """创建 JWT access token（加固版）

    包含：
    - iss (issuer)
    - aud (audience)
    - jti (唯一标识，用于撤销)
    - iat (签发时间)
    - exp (过期时间)
    - sub (主题/用户ID)
    - type (token 类型)
    """
    now = datetime.now(timezone.utc)
    expire = now + timedelta(minutes=_settings.jwt_access_token_expire_minutes)

    payload = {
        "sub": subject,
        "client_id": client_id,
        "scope": scope,
        "type": "access",
        "iat": now,
        "exp": expire,
        "jti": secrets.token_urlsafe(16),
        "iss": _get_issuer(),
        "aud": client_id,
    }
    if extra_claims:
        # 防止覆盖标准声明
        safe_claims = {k: v for k, v in extra_claims.items() if k not in payload}
        payload.update(safe_claims)

    return jwt.encode(payload, _settings.secret_key, algorithm="HS256")


def create_refresh_token(
    subject: str,
    client_id: str,
    scope: str = "",
) -> str:
    """创建 JWT refresh token（加固版）"""
    now = datetime.now(timezone.utc)
    expire = now + timedelta(days=_settings.jwt_refresh_token_expire_days)

    payload = {
        "sub": subject,
        "client_id": client_id,
        "scope": scope,
        "type": "refresh",
        "iat": now,
        "exp": expire,
        "jti": secrets.token_urlsafe(16),
        "iss": _get_issuer(),
        "aud": client_id,
    }

    return jwt.encode(payload, _settings.secret_key, algorithm="HS256")


def create_id_token(
    user_id: str,
    username: str,
    email: Optional[str] = None,
    full_name: Optional[str] = None,
    extra_claims: Optional[Dict[str, Any]] = None,
) -> str:
    """创建 OIDC ID Token（加固版）"""
    now = datetime.now(timezone.utc)
    expire = now + timedelta(minutes=_settings.jwt_id_token_expire_minutes)

    payload = {
        "sub": user_id,
        "preferred_username": username,
        "type": "id",
        "iat": now,
        "exp": expire,
        "jti": secrets.token_urlsafe(16),
        "iss": _get_issuer(),
        "aud": "unisso_clients",  # ID token 的 audience 是客户端群体
    }
    if email:
        payload["email"] = email
    if full_name:
        payload["name"] = full_name
    if extra_claims:
        safe_claims = {k: v for k, v in extra_claims.items() if k not in payload}
        payload.update(safe_claims)

    return jwt.encode(payload, _settings.secret_key, algorithm="HS256")


def decode_token(token: str, audience: Optional[str] = None) -> Optional[Dict[str, Any]]:
    """解码并验证 JWT token（加固版）

    固定使用 HS256 算法，防止 algorithm confusion attack。
    可选验证 audience。
    """
    try:
        options = {"verify_exp": True, "verify_alg": True}
        kwargs = {"algorithms": ["HS256"], "options": options}
        if audience:
            kwargs["audience"] = audience
        return jwt.decode(token, _settings.secret_key, **kwargs)
    except jwt.ExpiredSignatureError:
        return None
    except jwt.InvalidTokenError:
        return None
    except Exception:
        return None


def get_token_expiry(token: str) -> Optional[datetime]:
    """获取 token 过期时间"""
    try:
        payload = jwt.decode(
            token,
            _settings.secret_key,
            algorithms=["HS256"],
            options={"verify_exp": False},
        )
        exp = payload.get("exp")
        if exp:
            return datetime.fromtimestamp(exp, tz=timezone.utc)
    except Exception:
        pass
    return None


# ==================== Scope 工具 ====================

def scopes_to_list(scope: str) -> List[str]:
    """将空格分隔的 scope 转为列表"""
    return [s.strip() for s in scope.split() if s.strip()]


def list_to_scopes(scopes: List[str]) -> str:
    """将 scope 列表转为空格分隔字符串"""
    return " ".join(scopes)
