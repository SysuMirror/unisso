# 验证 SSO 会话并读取当前身份

验证已有 SSO 会话，读取最新用户资料与开发者资格，并延长会话有效期。供 Develop 后端在受保护业务前使用。

[接入 handoff](../handoff.md) · [OpenAPI 定义](qualification.openapi.yaml)

## 地址与鉴权

| 项目 | 值 |
| --- | --- |
| 方法 | `POST` |
| 路径 | `/api/oauth/session/verify` |
| Content-Type | `application/json` |
| 鉴权 | HTTP Basic 或请求体中的应用凭据 |

使用内部机密客户端的 `client_id`、`client_secret`，应用须已启用且获准调用 M2M 接口。新接入应用配置为 `kind=internal`、`is_confidential=true`、`m2m_enabled=true`。

示例使用 HTTP Basic：用户名是 `client_id`，密码是 `client_secret`。也可在 JSON 请求体中同时传这两个字段，一次请求统一使用一种认证方式。客户端密钥由下游后端保存。

既有部署的 `UNISSO_PASSWORD_VERIFY_CLIENTS` 仍可用于历史登录接入；新应用按上述应用属性配置。环境地址和凭据由管理员提供，见 [接入 handoff](../handoff.md#接入前准备)。

## 请求字段

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `client_id` | string | 条件必填 | 使用 Basic 时省略；使用请求体认证时提供应用 ID |
| `client_secret` | string | 条件必填 | 使用 Basic 时省略；使用请求体认证时与 `client_id` 同时提供密钥 |
| `session` | string | 是 | 登录接口返回的 `sso_session`，传入实际会话 ID |

## 调用示例

以下为 Bash 示例。设置 `BASE_URL`、`DEVELOP_CLIENT_ID`、`DEVELOP_CLIENT_SECRET`；将示例会话替换成登录返回值：

```bash
curl --silent --show-error \
  --user "${DEVELOP_CLIENT_ID}:${DEVELOP_CLIENT_SECRET}" \
  --header 'Content-Type: application/json' \
  --data '{"session":"sess_example"}' \
  "${BASE_URL}/api/oauth/session/verify"
```

## 成功响应

HTTP 200，以下示例表示会话有效、用户已有开发者资格：

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
    "developer": true,
    "preferred_username": "example",
    "name": "",
    "email": "user@example.invalid",
    "picture": "",
    "student_id": ""
  }
}
```

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `ok` | boolean | 成功时为 `true` |
| `sso_session` | string | SSO 会话 ID，验证或注销时作为请求参数 `session` 传入 |
| `sso_cookie_name` | string | 建议使用的 Cookie 名，以实际响应为准 |
| `sso_cookie_domain` | string | 建议使用的 Cookie 域，以实际响应为准 |
| `sso_max_age` | integer | 会话有效期，单位为秒 |
| `sso_expires_at` | integer | 会话过期时间，Unix 秒时间戳 |
| `user` | object | 当前用户身份，字段见下方 |
| `user.sub` | string | UniSSO 用户 ID，用作本地身份关联键 |
| `user.sso_sub` | string | 与 `user.sub` 相同 |
| `user.sse_sub` | string / null | 绑定的集市用户 ID，未绑定时为 `null`；与 UniSSO ID 分开保存 |
| `user.internal` | boolean | 院内身份标记 |
| `user.developer` | boolean | 当前开发者资格；严格等于 `true` 时才满足资格条件 |
| `user.preferred_username` | string | 用户名 |
| `user.name` | string | 姓名，未填写时为空字符串 |
| `user.email` | string | UniSSO 邮箱 |
| `user.picture` | string | 头像地址，未填写时为空字符串 |
| `user.student_id` | string | 学号，未填写时为空字符串 |

每次验证会重新读取数据库中的用户，并将会话有效期延长一个配置周期。默认周期为 168 小时，Cookie 名、域和有效期以响应为准；示例时间戳只展示格式。由下游保存新过期时间，或按接入方式更新 Cookie。

## 错误与处理

| HTTP | `error` | 含义与处理 |
| --- | --- | --- |
| 400 | `invalid_request` | 缺少 `session`，补齐请求 |
| 401 | `invalid_client` | 应用凭据或机密客户端配置不符 |
| 401 | `invalid_session` | 会话不存在、过期、内容无效，或用户不存在、已停用；引导重新登录 |
| 403 | `password_verify_forbidden` | 应用没有内部 M2M 接入权限 |
| 503 | `temporarily_unavailable` | Redis 读取、数据库读取或会话续期失败 |

无效会话时，HTTP 401：

```json
{"error":"invalid_session"}
```

依赖不可用时返回 HTTP 503，响应头包含 `Retry-After: 3`：

```json
{"ok":false,"error":"temporarily_unavailable","message":"身份服务暂时不可用，请稍后重试"}
```

按 HTTP 状态和 `error` 分支处理；`error_description`、`message` 用于解释。

## Develop 如何使用结果

```text
200 且 user.developer === true → 继续业务权限判断和操作
200 且 user.developer !== true → Develop 返回业务 403
401 invalid_session           → 引导重新登录
401 invalid_client 或应用 403  → 检查服务端应用凭据与配置
503 或网络超时                → 暂停本次受保护业务，保留本地登录记录
```

用户没有开发者资格时，UniSSO 仍返回 HTTP 200 和 `user.developer=false`。业务 403 由 Develop 返回。

依据 `Retry-After` 等待后可以再次验证。同一个会话可用于后续读取资格变化；如果下游缓存资格，缓存时间会增加生效延迟。Redis 数据真正丢失时，恢复后的旧会话可能返回 401，用户需要重新登录。
