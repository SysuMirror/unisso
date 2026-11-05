---
name: unisso-integration-external
description: 把外部平台（第三方应用）接入 UniSSO 跳转授权（OAuth2 授权码 + PKCE S256 + 同意页）。覆盖申请审核、授权、换 token、userinfo、错误契约、部署守则与验收清单。
---

# UniSSO 外部平台跳转授权接入

外部平台（third-party）接入 = 标准 OAuth2 授权码流程：用户**跳转**到 unisso 登录 → **每次授权都出同意页** → 带 `code` 跳回本平台 → 后端换 token → 取 userinfo。
UniSSO 只负责身份（sub/sse_sub/internal/name/email/picture/roles）；权限与角色由各平台自行判定。

> 与**内部平台无感登录**的区别：外部应用**不能**使用 M2M `password/verify`（调用只会返回 `403 password_verify_forbidden`），也**不会**静默通过——必须走下面的跳转授权，且**每次**授权都展示同意页（`auto_approved` 恒为 false）。

## 前置

1. 外部平台走**申请**流程（https://sso.ssemarket.cn/admin/apps 提交后 `status=pending`，超管审核 approve 后才可用）。内接应用的创建入口只用于内部平台。
2. 拿到 `client_id` 与 `client_secret`（secret 只在生成时显示一次）。
3. 登记 **`redirect_uris`**：逐字符精确匹配，含 scheme/host/port/path；未登记的回调地址会被直接拒绝（`invalid_redirect_uri`）。
4. 登记 **`allowed_scopes`**（白名单：`openid, profile, profile:username, profile:name, profile:avatar, profile:student_id, email, roles`）。
5. **凭据唯一可信来源是运行实例的 env**；仓库里的 env/compose 值视为可能过期——重建容器/进程前先从运行实例取（docker inspect / /proc/PID/environ）。
6. `client_secret` **只在后端持有**，绝不能下发到浏览器/前端。

## 授权码流程

### 1) 生成 PKCE（S256，必填）

- `code_verifier`：43–128 位 `[A-Za-z0-9-._~]` 随机串。
- `code_challenge = base64url(sha256(code_verifier))`，**去掉 padding**，长度恰为 **43** 字符。
- `code_challenge_method=S256`（其他方式一律拒绝）。

### 2) 跳转授权（浏览器 302）

```
GET https://sso.ssemarket.cn/authorize
  ?client_id=...
  &redirect_uri=...            # 必须与登记值完全一致
  &response_type=code
  &scope=openid profile:name email
  &state=...                   # 自己生成，回调时校验
  &code_challenge=...
  &code_challenge_method=S256
  &nonce=...                   # 可选
```

- `/authorize` 是**前端路由**（不是 API）；未登录会先跳登录页，随后进**同意页**。
- 同意页数据来自 `GET /api/oauth/authorize/context`（返回应用名、scope 列表、可选账号），提交同意/拒绝走 `POST /api/oauth/authorize`。
- 同意 → `302 {redirect_uri}?code=...&state=...`；拒绝 → `302 {redirect_uri}?error=access_denied&state=...`。
- **`redirect_uri` 与 `state` 必须原样带回并校验**（防 CSRF/开放重定向）。

### 3) 换 token（后端，POST）

```
POST https://sso.ssemarket.cn/api/oauth/token
Content-Type: application/x-www-form-urlencoded

grant_type=authorization_code
&code=...
&redirect_uri=...          # 与授权时一致
&client_id=...
&client_secret=...
&code_verifier=...
```

响应：

```json
{"access_token":"...","token_type":"Bearer","expires_in":3600,
 "refresh_token":"...","scope":"openid profile:name email",
 "sso_sub":"...","sse_sub":"...","id_token":"..."}
```

- **`code` 是一次性的**：重复使用 → `invalid_grant`。
- 支持 `refresh_token`；`client_credentials` 仅内接应用可用，外部应用只用授权码 + refresh。

### 4) 取用户信息

```
GET https://sso.ssemarket.cn/api/oauth/userinfo
Authorization: Bearer <access_token>
```

claims（按已授权的 scope 裁剪）：`sub`/`sso_sub`、`sse_sub`（已绑定校本身份时）、`preferred_username`、`name`、`picture`、`student_id`、`email`+`email_verified`、`roles`、`internal`。

- `profile` 是 legacy 别名，会展开为 `profile:username/profile:name/profile:avatar/profile:student_id`；建议**显式申请**子 scope，同意页更精确。
- `groups` / `organizations` 已移除，不要依赖。
- 实际授予 = 你申请 ∩ 应用 `allowed_scopes`，并强制包含 `openid`。

## 错误契约

| 端点 | 情况 | 返回 |
|---|---|---|
| `/authorize`（跳转前校验） | 参数/客户端/回调/scope 不合法 | **JSON 400**，`invalid_request` / `invalid_client` / `invalid_redirect_uri` / `unsupported_response_type` / `invalid_scope`（**不重定向**） |
| `POST /api/oauth/authorize`（用户拒绝） | 用户点"拒绝" | `200 {redirect_url}`，其中 `error=access_denied&state=...` |
| `POST /api/oauth/token` | code 缺失/过期/已用 | `invalid_grant` |
| `POST /api/oauth/token` | `client_id`/`secret` 不对 | 401 `invalid_client` |
| `GET /api/oauth/userinfo` | token 无效/过期 | 401 `invalid_token` |
| `POST /api/oauth/introspect` | 客户端鉴权通过、token 无效/为空 | `200 {"active":false}` |

- 应用 `status` 不是 `approved` ⇒ 授权直接失败（申请中/被拒的应用不可用）。

## 部署守则

1. `client_secret` 必须 **bcrypt（`$2b$12$...`）** 存储；明文 secret 在任何 v2 校验路径下都匹配不上（历史事故主因）。
2. 重建/重启后必须跑部署自检：用本平台 client 真实调一次 `POST /api/oauth/introspect`（带 client_id/secret、token 留空）必须 `200 {"active":false}`，否则回滚镜像。**不要**用假 code 打 token 端点来自检——那只会得到 `invalid_grant`，探不到客户端鉴权。
3. 排查授权失败先看 unisso 日志里的 `client=` / `kind=` 行，再对着上面的错误表定位。
4. 回调地址多入口（多域名/多端口）时，**每个入口发出的 `redirect_uri` 都要在登记表内**。

## 验收清单

- 未登录：跳转 `/authorize` → 登录页 → 登录后进同意页
- 同意：回调带回 `code`+`state`；换 token 成功；userinfo 字段正确
- 拒绝：回调带回 `error=access_denied`，平台优雅处理（不 500）
- code 复用：第二次换 token 得 `invalid_grant`
- 错 secret：`introspect` / `token` 得 401 `invalid_client`
- 未登记的回调地址：授权被拒 `invalid_redirect_uri`
- 多入口逐个走通（每个 origin 的 redirect_uri 均被接受）
