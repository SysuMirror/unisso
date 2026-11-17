# Join / Develop 接入指南

Join 审批通过后，把用户的开发者资格写入 UniSSO。Develop 在后端读取 `developer`，决定是否允许访问开发者业务。项目成员、参与人和负责人的权限由 Develop 管理。

本文对应修复提交 `f2d9e9c`，更新于 2026-11-17。真实 UniSSO 后端已通过 60 项本地隔离测试；平台 Dev 和真实 Join、Develop 的联调尚未执行。部署该版本后启用本文契约。

机器可读定义：[OpenAPI 3.1](../api/qualification.openapi.yaml)。验证范围与复跑方法：[链路验证记录](../../backend/tests/qualification/RESULTS.md)。

## 1. 接入前准备

平台管理员为 Join、Develop 分别配置应用。两者均使用后端保存的 `client_id` 和 `client_secret`，应用设置为内部应用（`kind=internal`）、机密客户端（`is_confidential=true`），并启用 `m2m_enabled`。Join 的状态还必须为 `approved`。

管理员在 UniSSO 运行环境设置：

```sh
UNISSO_DEVELOPER_WRITE_CLIENTS=<Join 的实际 client_id>
```

多个写入方用逗号分隔。空值表示没有应用可写资格。登录权限和资格写入权限分别配置；Develop 只需读取时不加入写入名单。历史 `UNISSO_PASSWORD_VERIFY_CLIENTS` 名单继续用于已有登录接入，但不能授予写资格的能力。

接口的基础地址由联调环境提供，以下用 `BASE_URL` 表示，例如本地测试服务地址。已在 README 列出的公共入口为 `https://sso.ssemarket.cn`；是否已部署本文版本，需由发布记录确认。

本文的 POST 示例使用 JSON 和 HTTP Basic 认证：用户名为 `client_id`，密码为 `client_secret`。也支持在请求体传这两个字段。一次请求统一使用一种方式，凭据保存在后端。

## 2. 一条完整链路

1. 用户已拥有 UniSSO 账号。Develop 通过内部登录接口或既有 OAuth 流程取得身份。
2. Develop 保存 `user.sub` 作为本地身份关联键；`developer=false` 时不放行开发者业务。
3. Join 审批通过，用该 `sub` 调用资格更新接口，显式设置 `is_developer=true`。
4. Develop 通过 `session/verify` 或 `userinfo` 重新读取。得到 `developer=true` 后放行业务。
5. 需要撤销资格时，获准的调用方设置 `is_developer=false`。Develop 下次读取后停止放行。

UniSSO 每次验证会话或查询 userinfo 都重新读取数据库中的资格。下游若缓存资格，缓存时长就是额外的生效延迟；需要每次校验的业务，在后端每次请求时读取。已经签发的 ID token 和下游本地副本不会随数据库变更而自动更新。

项目招募通过时，Join / Develop 还需处理对应项目成员关系。这一步由双方的业务接口完成，UniSSO 的资格更新只修改账号上的全局布尔值。

## 3. 身份字段

| 字段 | 含义 | 使用方式 |
| --- | --- | --- |
| `sub` / `sso_sub` | 相同的 UniSSO 用户 ID | 用于下游身份绑定和写资格的 `user_id` |
| `sse_sub` | 绑定的集市用户 ID | 需要关联集市身份时使用，和 UniSSO ID 分开保存 |
| `developer` | 开发者资格，布尔值 | 只有严格等于 `true` 时放行业务 |
| `internal` | 院内身份标记 | 与开发者资格分别判断 |
| `roles` | OAuth scope 允许时返回的角色数组 | 当前可包含 `super_admin`；开发者资格读取 `developer` |

内部 M2M 响应在 `user` 对象中返回身份；`userinfo` 直接在响应根对象返回身份。内部响应的 `sse_sub` 未绑定时为 `null`；OAuth userinfo 仅在授予 `sse_identity` 且已绑定时返回该字段。

## 4. Join：授予或撤销资格

`POST /api/oauth/developer/update`

| 参数 | 必填 | 说明 |
| --- | --- | --- |
| `user_id` | 三选一 | UniSSO `sub`，推荐使用 |
| `email` | 三选一 | UniSSO 邮箱，按去除首尾空白、小写后查找 |
| `sse_user_id` | 三选一 | 绑定的集市用户 ID，对应读取结果的 `sse_sub` |
| `is_developer` | 是，或用别名 | 推荐传 JSON 布尔值 `true` / `false` |
| `developer` | 兼容别名 | `is_developer` 无值时使用 |

若提供多个定位字段，按 `user_id` → `email` → `sse_user_id` 选第一个有值的字段；查找失败直接返回 404。接入时只传一个定位字段。资格值兼容 `1` / `0` 和对应字符串；字符串会去首尾空白并忽略大小写。省略资格值返回 400。

以下为 Bash 调用示例。先设置本次环境的 `BASE_URL`、`JOIN_CLIENT_ID`、`JOIN_CLIENT_SECRET`、`SSO_USER_ID`；值由管理员和用户登录结果提供。

```bash
# 授予资格；撤销时把 true 改为 false。
curl --silent --show-error \
  --user "${JOIN_CLIENT_ID}:${JOIN_CLIENT_SECRET}" \
  --header 'Content-Type: application/json' \
  --data "{\"user_id\":\"${SSO_USER_ID}\",\"is_developer\":true}" \
  "${BASE_URL}/api/oauth/developer/update"
```

成功响应为 HTTP 200：

```json
{
  "ok": true,
  "developer": true,
  "user": {
    "sub": "sso-user-example",
    "sso_sub": "sso-user-example",
    "sse_sub": null,
    "internal": false,
    "developer": true,
    "preferred_username": "example",
    "name": "",
    "email": "user@example.invalid",
    "picture": "",
    "student_id": ""
  }
}
```

重复设置相同值可以重试，不创建新账号。返回的资格是写入后重新读取的当前值；有并发写入时，以重新读取的值为准。未知账号返回 404，Join 可保留审批记录，待账号完成身份关联后再同步。

## 5. Develop：内部登录与会话读取

### 5.1 密码验证并创建 SSO 会话

`POST /api/oauth/password/verify`

请求体必填 `email`、`password`，内容为用户的 UniSSO 邮箱和密码。应用使用 Develop 自己的客户端凭据认证。成功后取得 `sso_session` 和 `user`：

```bash
curl --silent --show-error \
  --user "${DEVELOP_CLIENT_ID}:${DEVELOP_CLIENT_SECRET}" \
  --header 'Content-Type: application/json' \
  --data @login-request.json \
  "${BASE_URL}/api/oauth/password/verify"
```

`login-request.json` 的格式如下，运行时替换为实际用户输入：

```json
{"email":"user@example.invalid","password":"<用户输入的密码>"}
```

成功响应为 HTTP 200，`user` 结构与上节相同：

```json
{
  "ok": true,
  "sso_session": "sess_example",
  "sso_cookie_name": "unisso_sid",
  "sso_cookie_domain": ".ssemarket.cn",
  "sso_max_age": 604800,
  "sso_expires_at": 1795485316,
  "user": {
    "sub": "sso-user-example",
    "sso_sub": "sso-user-example",
    "sse_sub": null,
    "internal": false,
    "developer": false,
    "preferred_username": "example",
    "name": "",
    "email": "user@example.invalid",
    "picture": "",
    "student_id": ""
  }
}
```

`sso_expires_at` 是 Unix 秒时间戳；示例值仅展示格式。有效期默认 168 小时，运行环境可通过 `UNISSO_SSO_SESSION_HOURS` 调整。Cookie 名称、域也可配置，以实际响应为准。接口返回会话和 Cookie 元信息，由下游后端按接入方式保存会话或设置 HttpOnly、Secure Cookie。

密码接口对 IP 与邮箱组合限流：首次尝试开始计时 900 秒，计数超过 5 返回 429；成功验证后清除计数。已有 OAuth 登录的应用可以沿用 OAuth，按第 6 节读取资格。

### 5.2 验证会话并读取最新资格

`POST /api/oauth/session/verify`

```bash
curl --silent --show-error \
  --user "${DEVELOP_CLIENT_ID}:${DEVELOP_CLIENT_SECRET}" \
  --header 'Content-Type: application/json' \
  --data "{\"session\":\"${SSO_SESSION}\"}" \
  "${BASE_URL}/api/oauth/session/verify"
```

成功响应与密码验证相同，并将该会话的有效期向后延长一个配置周期。每次调用都会读取最新 `user.developer`。找不到会话、会话过期、用户不存在或已停用，均返回 401 `invalid_session`。

Develop 受保护业务可按以下顺序处理：

```text
调用 session/verify
  200 且 user.developer === true → 执行业务
  200 且 user.developer !== true → 返回业务 403
  401 invalid_session           → 要求用户重新登录
  503 或网络超时                → 本次业务返回暂时不可用，保留本地登录记录
```

其他 401、403 先按第 7 节检查应用凭据与配置。判断用户资格的业务 403 由 Develop 返回；UniSSO 对有效但无资格的账号仍返回 200 和 `developer=false`。

### 5.3 注销 SSO 会话

`POST /api/oauth/session/revoke`

请求体为 `{"session":"sess_example"}`，使用 Develop 的客户端凭据。成功返回 `{"ok":true}`；重复注销不存在的会话仍返回成功。Redis 删除失败返回 503。

该接口删除传入的 SSO 会话，其他使用同一会话的应用随后也会验证失败；它不撤销其他会话或 OAuth token。下游自己的本地会话也由下游清理。

## 6. Develop：已有 OAuth 登录时读取资格

`GET /api/oauth/userinfo`

使用既有 OAuth 授权流程取得的 access token：

```bash
curl --silent --show-error \
  --header "Authorization: Bearer ${ACCESS_TOKEN}" \
  "${BASE_URL}/api/oauth/userinfo"
```

一个最小成功响应如下：

```json
{
  "sub": "sso-user-example",
  "sso_sub": "sso-user-example",
  "internal": false,
  "developer": true
}
```

其他资料按 token 已授予的 scope 返回，如 `email`、`profile:username`、`sse_identity`。`internal` 和 `developer` 当前会直接返回。同一有效 access token 的后续 userinfo 请求可以读到资格变化，无需仅为刷新资格重新登录。

此请求使用 Bearer token，不需要 M2M 客户端密钥，也不要求应用启用内部登录。无效、过期或已撤销的 token 返回 401 `invalid_token`；读取数据库失败返回 503。OAuth 授权码、PKCE 和 token 换取流程沿用应用已有接入，本轮测试只覆盖 userinfo 读取。

## 7. 错误与重试

| HTTP | `error` | 下游处理 |
| --- | --- | --- |
| 400 | `invalid_request` | 补齐目标用户、资格值或 session 等必填参数 |
| 401 | `invalid_client` | 检查客户端 ID、密钥、启用状态及是否为机密客户端 |
| 401 | `invalid_credentials` | 用户邮箱、密码不匹配，或用户已停用 |
| 401 | `invalid_session` | 当前 SSO 会话不可用，要求重新登录 |
| 401 | `invalid_token` | 当前 OAuth token 不可用，按既有登录流程重新取得 token |
| 403 | `developer_update_forbidden` | 检查 Join 的内部类型、批准状态、M2M 和独立写入名单 |
| 403 | `password_verify_forbidden` | 当前客户端没有内部 M2M 接入权限，适用于密码和会话接口 |
| 404 | `user_not_found` | 核对身份关联；本接口不自动创建账号 |
| 429 | `too_many_attempts` | 等限流窗口结束后，由用户重新尝试 |
| 503 | `temporarily_unavailable` | 服务依赖暂时不可用，依据 `Retry-After: 3` 稍后重试 |

普通接口错误形如：

```json
{"error":"invalid_request","error_description":"请显式提供 is_developer: true / false"}
```

503 形如：

```json
{"ok":false,"error":"temporarily_unavailable","message":"身份服务暂时不可用，请稍后重试"}
```

客户端使用 HTTP 状态和 `error` 分支处理，`error_description` / `message` 用于解释。

资格更新出现 503 或超时，写入可能已经完成。Join 保留待同步任务，在恢复后重试同一个目标值，并核对成功响应中的 `developer`。会话验证出现 503 时，Develop 暂停本次受保护业务，保留本地登录记录；恢复后再验证。若 Redis 会话数据确实丢失，恢复后的验证会返回 401，用户再重新登录。

## 8. 这次升级的接入改动

- Join 加入独立写入名单，并显式传 `is_developer`；旧版本省略该值会授权，新版本返回 400。
- 内部 M2M 调用要求机密客户端和有效密钥；公共客户端继续使用 OAuth 授权路径。
- 下游增加 503 分支，把依赖不可用与账号、会话无效分别处理。
- 先将功能分支合入 Dev 联调，再按项目规范合入 main、手动发布 Prod。推送功能分支后，线上行为仍以实际部署版本为准。
