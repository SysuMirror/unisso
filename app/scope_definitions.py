"""UniSSO Scope 全局映射配置

定义 scope → claim 输出映射、中文描述、老粗粒度 scope 兼容展开。

核心约束：
- `sub` 不受 scope 控制，永远输出（子应用用它做账号绑定）；
- access_token 禁止携带 PII，隐私 claims 只出现在 id_token / userinfo；
- `app:*` 为业务功能授权 scope，不映射任何 PII claim。
"""
from typing import Dict, List, Tuple

# scope → (claim 输出列表, 中文描述)
SCOPE_DEFINITIONS: Dict[str, Tuple[List[str], str]] = {
    "openid": ([], "基础身份标识（用户唯一 ID）"),
    "profile:username": (["preferred_username"], "用户昵称"),
    "profile:name": (["name"], "真实姓名"),
    "profile:avatar": (["picture"], "头像"),
    "profile:student_id": (["student_id"], "学号"),
    "email": (["email", "email_verified"], "邮箱地址"),
    "roles": (["roles"], "用户角色列表"),
}

# 过渡期兼容：老粗粒度 scope → 等价细粒度集合
LEGACY_SCOPES: Dict[str, List[str]] = {
    "profile": ["profile:username", "profile:name", "profile:avatar", "profile:student_id"],
}


def expand_legacy_scopes(scopes: List[str]) -> List[str]:
    """展开老粗粒度 scope 为细粒度集合，保持顺序去重"""
    result: List[str] = []
    for scope in scopes:
        for expanded in LEGACY_SCOPES.get(scope, [scope]):
            if expanded not in result:
                result.append(expanded)
    return result


def get_claims_for_scopes(scopes: List[str]) -> List[str]:
    """返回 scopes 对应的 claim 集合（不含 sub，sub 永远输出）"""
    claims: List[str] = []
    for scope in scopes:
        for claim in SCOPE_DEFINITIONS.get(scope, ([], ""))[0]:
            if claim not in claims:
                claims.append(claim)
    return claims


def scope_description(scope: str) -> str:
    """scope 的中文描述；未知 scope（如 app:xxx）原样返回"""
    return SCOPE_DEFINITIONS[scope][1] if scope in SCOPE_DEFINITIONS else scope


def supported_scopes() -> List[str]:
    """全量已知 scope，供发现端点与管理面板使用"""
    return list(SCOPE_DEFINITIONS.keys())
