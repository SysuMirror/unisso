# 验证密码并创建 SSO 会话

供获准的内部应用在后端验证用户的 UniSSO 邮箱和密码，成功后取得身份与 SSO 会话。有效账号即使没有开发者资格，也能登录；Develop 再按 `user.developer` 判断业务访问。

[接入 handoff](../handoff.md) · [OpenAPI 定义](qualification.openapi.yaml)

## 地址与鉴权

| 项目 | 值 |
| --- | --- |
| 方法 | `POST` |
| 路径 | `/api/oauth/password/verify` |
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
| `email` | string | 是 | 用户的 UniSSO 邮箱，去首尾空白并转小写后查找 |
| `password` | string | 是 | 用户输入的密码 |

## 调用示例

以下为 Bash 示例。设置 `BASE_URL`、`DEVELOP_CLIENT_ID`、`DEVELOP_CLIENT_SECRET`，在运行时按用户输入组装 JSON 请求体。示例文件 `login-request.json`：

```json
{"email":"user@example.invalid","password":"<用户输入的密码>"}
```

```bash
curl --silent --show-error \
  --user "${DEVELOP_CLIENT_ID}:${DEVELOP_CLIENT_SECRET}" \
  --header 'Content-Type: application/json' \
  --data @login-request.json \
  "${BASE_URL}/api/oauth/password/verify"
```

## 成功响应

HTTP 200：

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

`sso_expires_at` 的示例值仅展示格式。默认有效期为 168 小时，即 604800 秒，可用 `UNISSO_SSO_SESSION_HOURS` 调整；Cookie 名与域也可配置，以实际响应为准。

接口返回会话和 Cookie 元信息，由下游后端保存会话，或按接入方式设置 HttpOnly、Secure Cookie。后续请求使用 [会话验证](session-verify.md) 读取当前身份。

## 错误与处理

| HTTP | `error` | 含义与处理 |
| --- | --- | --- |
| 400 | `invalid_request` | 邮箱或密码未提供，补齐请求 |
| 401 | `invalid_client` | 应用凭据或机密客户端配置不符 |
| 401 | `invalid_credentials` | 用户不存在、密码不匹配、账号无密码或已停用，引导用户检查登录信息 |
| 403 | `password_verify_forbidden` | 应用没有内部 M2M 接入权限 |
| 429 | `too_many_attempts` | 同一 IP 与邮箱组合尝试次数过多，等待窗口结束再由用户尝试 |
| 503 | `temporarily_unavailable` | 数据库、限流计数或会话存储操作失败，稍后重试 |

用户凭据错误时，HTTP 401：

```json
{"error":"invalid_credentials","error_description":"邮箱或密码错误"}
```

依赖不可用时返回 HTTP 503，响应头包含 `Retry-After: 3`：

```json
{"ok":false,"error":"temporarily_unavailable","message":"身份服务暂时不可用，请稍后重试"}
```

按 HTTP 状态和 `error` 分支处理；`error_description`、`message` 用于解释。

## 限流、资格与重试

同一 IP 与邮箱组合从首次尝试开始计时 900 秒，计数超过 5 返回 429；成功验证后清除计数。对 429 等待窗口结束，由用户再次提交，避免自动循环登录。

503 时不会返回成功会话。若网络超时，服务端可能已经创建会话但响应未送达，调用方仍没有可用的已确认会话 ID；恢复后重新登录可能创建另一会话。

只有 `user.developer === true` 才满足开发者资格条件；其他项目或资源权限由 Develop 判断。已有 OAuth 登录的应用可沿用登录流程，直接调用 [userinfo](userinfo.md)。
