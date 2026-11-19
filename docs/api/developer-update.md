# 设置开发者资格

Join 审批后调用本接口，授予或撤销已有 UniSSO 账号的全局开发者资格。接口设置目标值，不创建账号；项目成员关系由 Join / Develop 的业务处理。

[接入 handoff](../handoff.md) · [OpenAPI 定义](qualification.openapi.yaml)

## 地址与鉴权

| 项目 | 值 |
| --- | --- |
| 方法 | `POST` |
| 路径 | `/api/oauth/developer/update` |
| Content-Type | `application/json` |
| 鉴权 | HTTP Basic 或请求体中的应用凭据 |

`BASE_URL` 使用管理员提供的实际环境地址。调用应用必须已启用，为内部机密客户端，启用 M2M，状态为 `approved`，并加入 `UNISSO_DEVELOPER_WRITE_CLIENTS`。历史登录白名单不能代替资格写权限。

Basic 用户名为 `client_id`，密码为 `client_secret`；请求体认证则同时传入这两个字段。密钥保存在 Join 后端，一次请求统一使用一种认证方式。

## 请求字段

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `client_id` | string | 条件必填 | 使用 Basic 时省略；使用请求体认证时提供应用 ID |
| `client_secret` | string | 条件必填 | 使用 Basic 时省略；使用请求体认证时与 `client_id` 同时提供密钥 |
| `user_id` | string | 三选一 | 推荐使用，值为 UniSSO 的 `sub` |
| `email` | string | 三选一 | UniSSO 邮箱，去首尾空白并转小写后查找 |
| `sse_user_id` | string | 三选一 | 绑定的集市用户 ID，对应身份中的 `sse_sub` |
| `is_developer` | boolean，兼容值见下 | 与别名至少一个 | `true` 授予，`false` 撤销；必须显式提供 |
| `developer` | 同上 | 兼容别名 | `is_developer` 无值时使用，建议新接入使用正式字段 |

目标选择优先级是 `user_id` → `email` → `sse_user_id`，选择第一个有值的字段，查找失败直接返回 404。接入时传一个定位字段即可。

资格值推荐使用 JSON 布尔值，也兼容数字 `1` / `0` 和字符串 `true` / `false` / `1` / `0`。字符串去首尾空白并忽略大小写。没有默认资格值，省略会返回 400。

## 调用示例

下面是 Bash 示例。先设置 `BASE_URL`、`JOIN_CLIENT_ID`、`JOIN_CLIENT_SECRET`，并将示例 `user_id` 替换成登录取得的真实 `sub`：

```bash
curl --silent --show-error \
  --user "${JOIN_CLIENT_ID}:${JOIN_CLIENT_SECRET}" \
  --header 'Content-Type: application/json' \
  --data '{"user_id":"sso-user-example","is_developer":true}' \
  "${BASE_URL}/api/oauth/developer/update"
```

撤销时，请求体改为：

```json
{"user_id":"sso-user-example","is_developer":false}
```

## 成功响应

HTTP 200：

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

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `ok` | boolean | 成功时为 `true` |
| `developer` | boolean | 写入后重新读取的当前资格，与 `user.developer` 相同 |
| `user` | object | 当前用户身份 |
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

## 错误与处理

| HTTP | `error` | 含义与处理 |
| --- | --- | --- |
| 400 | `invalid_request` | 未提供定位字段、未提供资格值或资格值非法，修正请求 |
| 401 | `invalid_client` | 应用不存在、停用、不是机密客户端或密钥无效，检查凭据和应用配置 |
| 403 | `developer_update_forbidden` | 缺少写资格权限，检查内部类型、M2M、批准状态和独立名单 |
| 404 | `user_not_found` | 目标用户不存在，或写入后的确认读取发现用户已被删除；核对身份关联 |
| 503 | `temporarily_unavailable` | 数据库操作失败，按下节确认或重试 |

例如省略资格值时，HTTP 400：

```json
{"error":"invalid_request","error_description":"请显式提供 is_developer: true / false"}
```

依赖不可用时返回 HTTP 503，响应头包含 `Retry-After: 3`：

```json
{"ok":false,"error":"temporarily_unavailable","message":"身份服务暂时不可用，请稍后重试"}
```

按 HTTP 状态和 `error` 分支处理；`error_description`、`message` 用于解释。

## 重复调用、生效与重试

重复设置相同资格值保持相同目标状态，不新增用户；`updated_at` 可能更新。成功响应返回确认读取时的资格，并发写入时可能受到另一请求影响。

503 或超时可能发生在写入提交之后，不能据此断定资格未变。保留待同步审批结果，恢复后读取，或依据 `Retry-After` 重试同一个仍有效的目标值；有新审批决定时先更新待同步目标，避免旧授予覆盖新撤销。

Develop 下次调用 [会话验证](session-verify.md) 或 [userinfo](userinfo.md) 时读取当前资格。下游缓存的资格和已签发的 ID token 不会自动更新。
