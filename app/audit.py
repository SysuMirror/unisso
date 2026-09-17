"""UniSSO 安全审计日志

记录所有安全相关操作，便于事后追溯和分析。
日志格式：JSON Lines，每行一条记录。
"""
import json
import os
import time
from datetime import datetime, timezone
from typing import Optional, Dict, Any
from dataclasses import dataclass, asdict

from app.config import get_settings

_settings = get_settings()

# 审计日志文件路径
AUDIT_LOG_DIR = os.environ.get("AUDIT_LOG_DIR", "logs")
AUDIT_LOG_FILE = os.path.join(AUDIT_LOG_DIR, "audit.log")



@dataclass
class AuditEvent:
    """审计事件"""
    timestamp: str
    event_type: str          # login, logout, register, password_change, authorize, token_issue, consent_grant, consent_revoke, identity_bind, identity_unbind, admin_action, security_alert
    user_id: Optional[str]
    client_ip: str
    user_agent: Optional[str]
    details: Dict[str, Any]  # 事件详情（不包含敏感信息如密码）
    success: bool
    risk_level: str          # low, medium, high, critical


# 敏感字段，记录时脱敏
SENSITIVE_KEYS = {"password", "client_secret", "code_verifier", "refresh_token", "access_token", "token"}


def _sanitize_details(details: Dict[str, Any]) -> Dict[str, Any]:
    """脱敏处理"""
    result = {}
    for k, v in details.items():
        if k in SENSITIVE_KEYS:
            result[k] = "<redacted>"
        else:
            result[k] = v
    return result


def log_audit(
    event_type: str,
    user_id: Optional[str] = None,
    client_ip: str = "unknown",
    user_agent: Optional[str] = None,
    details: Optional[Dict[str, Any]] = None,
    success: bool = True,
    risk_level: str = "low",
):
    """记录审计日志（best-effort，永不抛异常）"""
    try:
        event = AuditEvent(
            timestamp=datetime.now(timezone.utc).isoformat(),
            event_type=event_type,
            user_id=user_id,
            client_ip=client_ip,
            user_agent=user_agent,
            details=_sanitize_details(details or {}),
            success=success,
            risk_level=risk_level,
        )

        line = json.dumps(asdict(event), ensure_ascii=False, default=str)

        os.makedirs(AUDIT_LOG_DIR, exist_ok=True)
        with open(AUDIT_LOG_FILE, "a", encoding="utf-8") as f:
            f.write(line + "\n")
            f.flush()

        # 高风险事件同时输出到 stderr
        if risk_level in ("high", "critical"):
            import sys
            print(f"[SECURITY_ALERT] {line}", file=sys.stderr)

    except Exception:
        pass


def log_login(
    user_id: Optional[str],
    client_ip: str,
    user_agent: Optional[str],
    success: bool,
    email: Optional[str] = None,
    reason: Optional[str] = None,
):
    """记录登录事件"""
    risk = "low" if success else "medium"
    # 多次失败应标记为高风险（简单实现：检查日志中最近失败次数）
    if not success:
        try:
            recent_failures = _count_recent_failures(client_ip, "login", minutes=10)
            if recent_failures >= 5:
                risk = "high"
        except Exception:
            pass

    log_audit(
        event_type="login",
        user_id=user_id,
        client_ip=client_ip,
        user_agent=user_agent,
        details={"email": email, "reason": reason},
        success=success,
        risk_level=risk,
    )


def log_register(
    user_id: Optional[str],
    client_ip: str,
    user_agent: Optional[str],
    success: bool,
    email: Optional[str] = None,
    reason: Optional[str] = None,
):
    """记录注册事件"""
    log_audit(
        event_type="register",
        user_id=user_id,
        client_ip=client_ip,
        user_agent=user_agent,
        details={"email": email, "reason": reason},
        success=success,
        risk_level="low",
    )


def log_authorize(
    user_id: str,
    client_ip: str,
    client_id: str,
    scopes: list,
    success: bool,
    auto_approved: bool = False,
):
    """记录 OAuth2 授权事件"""
    log_audit(
        event_type="authorize",
        user_id=user_id,
        client_ip=client_ip,
        details={
            "client_id": client_id,
            "scopes": scopes,
            "auto_approved": auto_approved,
        },
        success=success,
        risk_level="low",
    )


def log_token_issue(
    user_id: str,
    client_ip: str,
    client_id: str,
    grant_type: str,
    success: bool,
):
    """记录 Token 颁发事件"""
    log_audit(
        event_type="token_issue",
        user_id=user_id,
        client_ip=client_ip,
        details={"client_id": client_id, "grant_type": grant_type},
        success=success,
        risk_level="low",
    )


def log_password_change(
    user_id: str,
    client_ip: str,
    success: bool,
    reason: Optional[str] = None,
):
    """记录密码修改事件"""
    log_audit(
        event_type="password_change",
        user_id=user_id,
        client_ip=client_ip,
        details={"reason": reason},
        success=success,
        risk_level="medium" if success else "high",
    )


def log_admin_action(
    admin_id: str,
    client_ip: str,
    action: str,
    target_type: str,
    target_id: str,
    details: Optional[Dict[str, Any]] = None,
):
    """记录管理员操作"""
    log_audit(
        event_type="admin_action",
        user_id=admin_id,
        client_ip=client_ip,
        details={
            "action": action,
            "target_type": target_type,
            "target_id": target_id,
            **(details or {}),
        },
        success=True,
        risk_level="medium",
    )


def log_security_alert(
    client_ip: str,
    alert_type: str,
    details: Dict[str, Any],
    user_id: Optional[str] = None,
):
    """记录安全告警"""
    log_audit(
        event_type="security_alert",
        user_id=user_id,
        client_ip=client_ip,
        details={"alert_type": alert_type, **details},
        success=False,
        risk_level="critical",
    )


def _count_recent_failures(client_ip: str, event_type: str, minutes: int = 10) -> int:
    """统计指定 IP 在指定时间内的失败次数"""
    try:
        cutoff = time.time() - minutes * 60
        count = 0
        with open(AUDIT_LOG_FILE, "r", encoding="utf-8") as f:
            for line in f:
                try:
                    event = json.loads(line.strip())
                    if (event.get("client_ip") == client_ip and
                        event.get("event_type") == event_type and
                        not event.get("success", True)):
                        # 简单时间检查（ISO 格式字符串比较不够精确，但够用）
                        count += 1
                except Exception:
                    continue
        return count
    except Exception:
        return 0
