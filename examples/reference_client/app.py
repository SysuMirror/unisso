"""UniSSO 参考客户端：演示 Authorization Code + PKCE 完整接入流程

适用于子应用（如集市主站）对接 UniSSO：
1. 生成 PKCE code_verifier / code_challenge
2. 重定向用户到 /authorize（细粒度 scope）
3. 回调处用 code + verifier 换 token（confidential 客户端带 client_secret）
4. 解析 id_token / 调用 userinfo 获取授权范围内的用户信息
5. 用 refresh_token 轮换续期

运行：python examples/reference_client/app.py
依赖：pip install fastapi uvicorn httpx
"""
import base64
import hashlib
import os
import secrets

import httpx
from fastapi import FastAPI, Request
from fastapi.responses import RedirectResponse

# ===== 配置（从环境变量或管理面板获取） =====
UNISSO_BASE_URL = os.environ.get("UNISSO_BASE_URL", "http://localhost:8000")
CLIENT_ID = os.environ.get("UNISSO_CLIENT_ID", "your_client_id")
CLIENT_SECRET = os.environ.get("UNISSO_CLIENT_SECRET", "your_client_secret")  # Public 客户端留空
CALLBACK_URL = os.environ.get("UNISSO_CALLBACK_URL", "http://localhost:9000/callback")

app = FastAPI(title="UniSSO Reference Client")

# 简单内存存储（生产环境请用 session/DB）
_states: dict[str, dict] = {}


def make_pkce() -> tuple[str, str]:
    """生成 PKCE verifier 与 S256 challenge"""
    verifier = base64.urlsafe_b64encode(secrets.token_bytes(32)).rstrip(b"=").decode()
    challenge = base64.urlsafe_b64encode(
        hashlib.sha256(verifier.encode()).digest()
    ).rstrip(b"=").decode()
    return verifier, challenge


@app.get("/login")
def login():
    """发起登录：重定向到 UniSSO 授权页

    细粒度 scope 示例：只申请昵称与邮箱，不申请学号等敏感信息
    """
    verifier, challenge = make_pkce()
    state = secrets.token_urlsafe(16)
    _states[state] = {"verifier": verifier}  # 生产环境应绑定到用户 session

    params = {
        "response_type": "code",
        "client_id": CLIENT_ID,
        "redirect_uri": CALLBACK_URL,
        "scope": "openid profile:username profile:avatar email",
        "state": state,
        "code_challenge": challenge,
        "code_challenge_method": "S256",
    }
    query = "&".join(f"{k}={v}" for k, v in params.items())
    return RedirectResponse(f"{UNISSO_BASE_URL}/authorize?{query}")


@app.get("/callback")
async def callback(request: Request):
    """处理授权回调：code 换 token"""
    code = request.query_params.get("code")
    state = request.query_params.get("state")
    error = request.query_params.get("error")

    if error:
        return {"error": error, "message": "用户拒绝授权或授权失败"}

    saved = _states.pop(state, None)
    if not saved:
        return {"error": "invalid_state"}

    # 用 code + PKCE verifier 换 token（confidential 客户端附带 secret）
    async with httpx.AsyncClient(trust_env=False) as client:
        resp = await client.post(
            f"{UNISSO_BASE_URL}/api/oauth/token",
            data={
                "grant_type": "authorization_code",
                "code": code,
                "redirect_uri": CALLBACK_URL,
                "client_id": CLIENT_ID,
                "client_secret": CLIENT_SECRET,  # Public 客户端省略
                "code_verifier": saved["verifier"],
            },
        )
        if resp.status_code != 200:
            return {"error": "token_exchange_failed", "detail": resp.text}
        tokens = resp.json()

    # id_token 已按授权 scope 过滤 claims（无 PII 泄漏）
    # userinfo 需带 access_token 获取
    async with httpx.AsyncClient(trust_env=False) as client:
        resp = await client.get(
            f"{UNISSO_BASE_URL}/api/oauth/userinfo",
            headers={"Authorization": f"Bearer {tokens['access_token']}"},
        )
        userinfo = resp.json() if resp.status_code == 200 else {"error": resp.text}

    return {
        "tokens": {
            "access_token": tokens["access_token"][:20] + "...",
            "refresh_token": tokens.get("refresh_token", "")[:20] + "...",
            "expires_in": tokens.get("expires_in"),
            "scope": tokens.get("scope"),
        },
        "userinfo": userinfo,
    }


@app.get("/refresh")
async def refresh(refresh_token: str):
    """refresh_token 轮换：旧 token 失效，返回全新 token 对"""
    async with httpx.AsyncClient(trust_env=False) as client:
        resp = await client.post(
            f"{UNISSO_BASE_URL}/api/oauth/token",
            data={
                "grant_type": "refresh_token",
                "refresh_token": refresh_token,
                "client_id": CLIENT_ID,
                "client_secret": CLIENT_SECRET,
            },
        )
        return resp.json()
