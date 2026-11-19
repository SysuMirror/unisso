# OAuth userinfo：读取当前身份与资格

已有 OAuth 登录的应用，用有效 access token 读取当前 UniSSO 身份和开发者资格。身份字段直接位于响应根部。

[接入 handoff](../handoff.md) · [OpenAPI 定义](qualification.openapi.yaml)

## 地址与鉴权

| 项目 | 值 |
| --- | --- |
| 方法 | `GET` |
| 路径 | `/api/oauth/userinfo` |
| 鉴权 | `Authorization: Bearer <access_token>` |
| 请求体 | 无 |

使用管理员提供的 `BASE_URL`。令牌由应用已有的 OAuth 登录流程取得，传入 access token。本接口不需要 M2M 客户端密钥，也不要求应用启用内部 M2M 接入。

## 请求字段与调用示例

| 位置 | 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- | --- |
| Header | `Authorization` | string | 是 | 格式为 `Bearer ` 加有效 access token |

以下为 Bash 示例，先设置 `BASE_URL`、`ACCESS_TOKEN`：

```bash
curl --silent --show-error \
  --header "Authorization: Bearer ${ACCESS_TOKEN}" \
  "${BASE_URL}/api/oauth/userinfo"
```

## 成功响应

HTTP 200，最小响应：

```json
{
  "sub": "sso-user-example",
  "sso_sub": "sso-user-example",
  "internal": false,
  "developer": true
}
```

| 字段 | 类型 | 返回条件与含义 |
| --- | --- | --- |
| `sub` | string | 总是返回，UniSSO 用户 ID，保存为本地身份关联键 |
| `sso_sub` | string | 总是返回，与 `sub` 相同 |
| `internal` | boolean | 总是返回，院内身份标记 |
| `developer` | boolean | 总是返回，当前开发者资格 |
| `sse_sub` | string | 授予 `sse_identity` 且用户已绑定集市身份时返回 |
| `preferred_username` | string | 授予 `profile:username` 且值非空时返回 |
| `name` | string | 授予 `profile:name` 且值非空时返回 |
| `picture` | string | 授予 `profile:avatar` 且值非空时返回 |
| `student_id` | string | 授予 `profile:student_id` 且值非空时返回 |
| `email` | string | 授予 `email` 时返回 |
| `email_verified` | boolean | 授予 `email` 时返回 |
| `roles` | array[string] | 授予 `roles` 时返回，当前可包含 `super_admin` |

当前实现直接返回 `internal`、`developer`，无需额外的资格 scope；其他资料字段按 token 已授予的 scope 返回。开发者资格读取 `developer`，项目成员、负责人和资源权限由下游判断。

## 错误与处理

| HTTP | `error` | 含义与处理 |
| --- | --- | --- |
| 401 | `invalid_token` | token 缺失、无效、过期或撤销，或用户不存在、已停用；按原有 OAuth 流程重新取得可用凭据 |
| 503 | `temporarily_unavailable` | 读取令牌撤销记录或用户数据失败；保留本地登录记录，暂缓受保护业务 |

token 缺失、校验失败或已撤销时，HTTP 401，响应头包含 `WWW-Authenticate: Bearer error="invalid_token"`。用户不存在或已停用时，也返回相同 JSON 错误，当前实现不附带该响应头：

```json
{"error":"invalid_token"}
```

依赖不可用时返回 HTTP 503，响应头包含 `Retry-After: 3`：

```json
{"ok":false,"error":"temporarily_unavailable","message":"身份服务暂时不可用，请稍后重试"}
```

按 HTTP 状态和 `error` 分支处理；`error_description`、`message` 用于解释。

## 资格生效与重试

每次请求会验证 access token、检查撤销记录，再读取当前用户。同一个仍然有效的 token 重复请求时，可以看到授予或撤销后的资格，无需仅为刷新资格重新登录。

依据 `Retry-After` 等待后可以重新请求。Develop 只在 `developer === true` 时满足资格条件；有效但无资格的用户仍得到 HTTP 200 和 `developer=false`，由 Develop 返回自己的业务 403。

下游若使用本地缓存、以前保存的身份响应或 ID token 内容，这些副本不会随资格变更自动更新。完整授权码、PKCE、token 换取和刷新流程沿用应用已有接入。
