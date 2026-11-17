# 注销 SSO 会话

删除指定 SSO 会话。其他应用如果使用同一个会话 ID，后续验证也会失败；其他会话和 OAuth token 不受该接口影响。

[接入 handoff](../handoff.md) · [OpenAPI 定义](qualification.openapi.yaml)

## 地址与鉴权

| 项目 | 值 |
| --- | --- |
| 方法 | `POST` |
| 路径 | `/api/oauth/session/revoke` |
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
| `session` | string | 是 | 要删除的 SSO 会话 ID，对应登录响应中的 `sso_session` |

## 调用示例

以下为 Bash 示例。设置 `BASE_URL`、`DEVELOP_CLIENT_ID`、`DEVELOP_CLIENT_SECRET`；将示例会话替换成实际要注销的值：

```bash
curl --silent --show-error \
  --user "${DEVELOP_CLIENT_ID}:${DEVELOP_CLIENT_SECRET}" \
  --header 'Content-Type: application/json' \
  --data '{"session":"sess_example"}' \
  "${BASE_URL}/api/oauth/session/revoke"
```

## 成功响应

HTTP 200：

```json
{"ok":true}
```

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `ok` | boolean | 删除命令执行成功时为 `true`，目标会话本来不存在也返回成功 |

## 错误与处理

| HTTP | `error` | 含义与处理 |
| --- | --- | --- |
| 400 | `invalid_request` | 缺少 `session`，补齐请求 |
| 401 | `invalid_client` | 应用凭据或机密客户端配置不符 |
| 403 | `password_verify_forbidden` | 应用没有内部 M2M 接入权限 |
| 503 | `temporarily_unavailable` | 客户端校验所需的数据库读取失败，或 Redis 删除失败 |

缺少会话参数时，HTTP 400：

```json
{"error":"invalid_request","error_description":"missing session"}
```

依赖不可用时返回 HTTP 503，响应头包含 `Retry-After: 3`：

```json
{"ok":false,"error":"temporarily_unavailable","message":"身份服务暂时不可用，请稍后重试"}
```

按 HTTP 状态和 `error` 分支处理；`error_description`、`message` 用于解释。

## 重试与退出范围

同一个会话可以重复注销，目标状态保持为不存在。503 或网络超时后，可以等待再重试相同会话 ID；未收到成功响应时，不应提示 SSO 会话已确认注销。

下游自己的本地会话与 Cookie 由下游清理。本接口只删除传入的 SSO 会话，其他独立会话、OAuth access token 和 refresh token 仍按各自的失效规则处理。
