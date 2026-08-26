"""UniSSO 本地端到端测试：完整 OAuth2 授权码 + PKCE 流程

前置：服务已在 http://127.0.0.1:8000 运行，管理员已初始化
运行：python examples/e2e_test.py
"""
import base64
import hashlib
import re
import secrets
import sys
import time

import httpx

BASE = "http://127.0.0.1:8000"
ADMIN_EMAIL = "admin@mail2.sysu.edu.cn"
ADMIN_PASSWORD = "Admin123456!"

client = httpx.Client(base_url=BASE, follow_redirects=False, trust_env=False)
PASS, FAIL = 0, 0

# 等待服务就绪（本地启动含 Redis 超时回退 + DB 初始化，最长 30 秒）
for _ in range(30):
    try:
        if client.get("/health", timeout=2).status_code == 200:
            break
    except httpx.TransportError:
        pass
    time.sleep(1)


def check(name: str, cond: bool, detail: str = ""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  [PASS] {name}")
    else:
        FAIL += 1
        print(f"  [FAIL] {name} {detail}")


def make_pkce():
    verifier = base64.urlsafe_b64encode(secrets.token_bytes(32)).rstrip(b"=").decode()
    challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).rstrip(b"=").decode()
    return verifier, challenge


def login(email: str, password: str):
    """登录并保留 session cookie"""
    r = client.post("/api/auth/login", data={"email": email, "password": password, "next": "/"})
    assert r.status_code == 302, f"登录失败: {r.status_code} {r.text}"
    return r


print("=== 1. 服务健康 ===")
r = client.get("/health")
check("GET /health", r.status_code == 200 and r.json()["ok"])

print("=== 2. OIDC 发现端点（细粒度 scope） ===")
r = client.get("/.well-known/openid-configuration")
scopes = r.json()["scopes_supported"]
check("细粒度 scope 已发布", "profile:student_id" in scopes and "profile" not in scopes, str(scopes))

print("=== 3. 管理员登录 + 创建应用 ===")
login(ADMIN_EMAIL, ADMIN_PASSWORD)
r = client.post("/api/apps", json={
    "name": "E2E 测试应用",
    "description": "端到端测试",
    "redirect_uris": ["http://localhost:9000/callback"],
    "allowed_scopes": ["openid", "profile:username", "profile:name", "email"],
    "is_confidential": True,
})
app_data = r.json()
check("创建应用", r.status_code == 200 and "client_id" in app_data, r.text[:200])
CLIENT_ID = app_data["client_id"]
CLIENT_SECRET = app_data["client_secret"]

print("=== 4. 发起授权（PKCE） ===")
verifier, challenge = make_pkce()
r = client.get("/authorize", params={
    "response_type": "code",
    "client_id": CLIENT_ID,
    "redirect_uri": "http://localhost:9000/callback",
    "scope": "openid profile:username profile:student_id email",
    "state": "test-state-123",
    "code_challenge": challenge,
    "code_challenge_method": "S256",
})
check("授权页渲染", r.status_code == 200 and "授权确认" in r.text,
      f"status={r.status_code} loc={r.headers.get('location', '')[:120]} body={r.text[:200]}")
check("scope 中文文案", "用户昵称" in r.text and "邮箱地址" in r.text)
check("请求了超出白名单的 student_id 被应用白名单过滤", 'value="profile:student_id"' not in r.text)

print("=== 5. 提交授权（勾选 scope） ===")
r = client.post("/api/oauth/authorize", data={
    "client_id": CLIENT_ID,
    "redirect_uri": "http://localhost:9000/callback",
    "scope": "openid profile:username profile:student_id email",
    "state": "test-state-123",
    "code_challenge": challenge,
    "code_challenge_method": "S256",
    "approved": "true",
    "scopes": ["openid", "profile:username", "email"],  # 用户只勾选昵称+邮箱
})
check("授权重定向带回 code", r.status_code == 302 and "code=" in r.headers.get("location", ""), str(r.status_code))
check("state 回传", "state=test-state-123" in r.headers.get("location", ""))
code = re.search(r"code=([^&]+)", r.headers["location"]).group(1)

print("=== 6. 授权码换 token ===")
r = client.post("/api/oauth/token", data={
    "grant_type": "authorization_code",
    "code": code,
    "redirect_uri": "http://localhost:9000/callback",
    "client_id": CLIENT_ID,
    "client_secret": CLIENT_SECRET,
    "code_verifier": verifier,
})
tokens = r.json() if r.headers.get("content-type", "").startswith("application/json") else {}
check("换取 token", r.status_code == 200 and "access_token" in tokens,
      f"status={r.status_code} ct={r.headers.get('content-type')} body={r.text[:300]}")
check("生效 scope = 用户勾选（无 student_id）",
      tokens.get("scope") == "email openid profile:username", tokens.get("scope"))
check("返回 id_token", bool(tokens.get("id_token")))

print("=== 7. 授权码一次性使用 ===")
r = client.post("/api/oauth/token", data={
    "grant_type": "authorization_code",
    "code": code,
    "redirect_uri": "http://localhost:9000/callback",
    "client_id": CLIENT_ID,
    "client_secret": CLIENT_SECRET,
    "code_verifier": verifier,
})
check("授权码重放被拒绝", r.status_code == 400, str(r.status_code))

print("=== 8. userinfo 按 scope 过滤 ===")
r = client.get("/api/oauth/userinfo", headers={"Authorization": f"Bearer {tokens['access_token']}"})
info = r.json()
check("userinfo 成功", r.status_code == 200, r.text[:200])
check("sub 永远返回", "sub" in info)
check("昵称按授权返回", "preferred_username" in info)
check("邮箱按授权返回", "email" in info)
check("未授权的学号/姓名不输出", "student_id" not in info and "name" not in info, str(info))

print("=== 9. introspection（token 落库生效） ===")
r = client.post("/api/oauth/introspect", data={"token": tokens["access_token"]})
intro = r.json()
check("token 活跃", intro.get("active") is True, str(intro))

print("=== 10. refresh 轮换 ===")
r = client.post("/api/oauth/token", data={
    "grant_type": "refresh_token",
    "refresh_token": tokens["refresh_token"],
    "client_id": CLIENT_ID,
    "client_secret": CLIENT_SECRET,
})
new_tokens = r.json()
check("refresh 换新 token", r.status_code == 200 and "access_token" in new_tokens, r.text[:200])

# 旧 refresh token 应已撤销
r = client.post("/api/oauth/token", data={
    "grant_type": "refresh_token",
    "refresh_token": tokens["refresh_token"],
    "client_id": CLIENT_ID,
    "client_secret": CLIENT_SECRET,
})
check("旧 refresh token 已撤销（轮换生效）", r.status_code == 400, str(r.status_code))

print("=== 11. 撤销授权 → token 联动失效 ===")
r = client.get("/api/consents")
consents = r.json()
target = next((c for c in consents if c["application"]["client_id"] == CLIENT_ID), None)
check("查询授权记录", target is not None)
if target:
    r = client.request("DELETE", f"/api/consents/{target['id']}")
    check("撤销授权", r.status_code == 200)

    # 撤销后 access_token 应失效
    r = client.get("/api/oauth/userinfo", headers={"Authorization": f"Bearer {new_tokens['access_token']}"})
    check("撤销后 access_token 被拒绝", r.status_code == 401, str(r.status_code))

    r = client.post("/api/oauth/introspect", data={"token": new_tokens["access_token"]})
    check("introspect 确认不活跃", r.json().get("active") is False)

print("=== 12. 安全：confidential 客户端缺 secret 被拒 ===")
v2, c2 = make_pkce()
r = client.get("/authorize", params={
    "response_type": "code", "client_id": CLIENT_ID,
    "redirect_uri": "http://localhost:9000/callback",
    "scope": "openid email", "code_challenge": c2, "code_challenge_method": "S256",
})
check("撤销后再次授权需重新确认（consent 已撤销）", r.status_code == 200 and "授权确认" in r.text,
      f"status={r.status_code} body={r.text[:150]}")
# 重新提交授权以获得 code
r = client.post("/api/oauth/authorize", data={
    "client_id": CLIENT_ID,
    "redirect_uri": "http://localhost:9000/callback",
    "scope": "openid email",
    "code_challenge": c2, "code_challenge_method": "S256",
    "approved": "true",
    "scopes": ["openid", "email"],
})
check("重新授权拿到 code", r.status_code == 302 and "code=" in r.headers.get("location", ""))
code2 = re.search(r"code=([^&]+)", r.headers["location"]).group(1)
r = client.post("/api/oauth/token", data={
    "grant_type": "authorization_code", "code": code2,
    "redirect_uri": "http://localhost:9000/callback",
    "client_id": CLIENT_ID, "code_verifier": v2,
    # 故意不带 client_secret
})
check("缺 secret 拒绝", r.status_code == 400 and "invalid_client" in r.text, r.text[:150])

# 清理：删除测试应用
client.request("DELETE", f"/api/apps/{app_data['id']}")

print(f"\n{'='*50}\n结果: {PASS} 通过, {FAIL} 失败")
sys.exit(1 if FAIL else 0)
