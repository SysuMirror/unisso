# Join / Develop 接入 handoff

Join 审批通过后，把用户的全局开发者资格写入 UniSSO；Develop 读取当前资格，再执行自己的业务权限判断。从本文选择需要的接口，单份接口文档包含完整参数、响应和调用示例。

## 分工与接口选择

| 接入方 | 负责内容 | 阅读入口 |
| --- | --- | --- |
| UniSSO 管理员 | 提供服务地址与客户端凭据，配置应用和资格写入名单 | 下方“接入前准备” |
| Join 后端 | 保存审批结果，向 UniSSO 同步授予或撤销资格 | [设置开发者资格](api/developer-update.md) |
| Develop 后端：使用内部登录 | 验证密码、保存会话、读取资格、注销会话 | [密码验证](api/password-verify.md)、[会话验证](api/session-verify.md)、[会话注销](api/session-revoke.md) |
| Develop 后端：已有 OAuth 登录 | 继续原有登录流程，用 access token 读取资格 | [userinfo](api/userinfo.md) |

UniSSO 保存账号的 `developer` 布尔值。审批规则由 Join 管理，项目成员、参与人、负责人和资源权限由 Develop 管理。Develop 根据已有登录方式选择一条读取路径即可。

## 接入前准备

管理员向接入方提供实际联调环境的 `BASE_URL`，并确认环境已部署支持本文接口的版本。地址、客户端 ID、密钥通过团队现有凭据渠道交接。

内部 M2M 接入时，为 Join、Develop 分别配置应用及凭据：

| 项目 | Join 写资格 | Develop 使用内部登录与会话接口 |
| --- | --- | --- |
| 应用启用 | 是 | 是 |
| `kind` | `internal` | `internal` |
| `is_confidential` | `true` | `true` |
| `m2m_enabled` | `true` | `true` |
| `status` | `approved` | 接口不额外校验批准状态 |
| `UNISSO_DEVELOPER_WRITE_CLIENTS` | 加入实际 `client_id` | 只读应用无需加入 |

资格写入名单在 UniSSO 运行环境中配置，多个 ID 用逗号分隔：

```sh
UNISSO_DEVELOPER_WRITE_CLIENTS='<Join 的实际 client_id>'
```

空名单拒绝所有应用写资格。历史登录名单 `UNISSO_PASSWORD_VERIFY_CLIENTS` 不授予资格写权限。Develop 若使用 OAuth userinfo，沿用已有 OAuth 应用和 access token，不需要为 userinfo 启用 M2M。

使用仓库的 `deploy.sh` 启动时，也可把逗号分隔的客户端 ID 写入持久目录中的 `backend/developer-write-clients.local`。平台未设置 `UNISSO_DEVELOPER_WRITE_CLIENTS` 时，脚本从该文件加载；平台环境变量优先，包括显式设置的空值。此文件不进 Git。名单复用应用已有的 Client ID，应用继续使用原密钥。

## 身份如何关联

把 UniSSO 的 `sub` 保存为下游身份关联键；`sso_sub` 与它相同。Join 写资格时，把这个值作为 `user_id`。`sse_sub` 是另一套集市账号 ID，单独保存。

内部接口的资格字段在 `user.developer`；userinfo 的资格字段在响应根部 `developer`。有效身份但没有开发者资格时，UniSSO 返回 HTTP 200 和 `developer=false`，由 Develop 决定业务是否返回 403。

## 走通一条链路

1. 选一个已经存在、初始没有资格的测试账号，确认身份读取返回 `developer=false`，Develop 拒绝需要资格的业务。
2. Join 按审批结果调用资格接口，显式传 `is_developer=true`。
3. Develop 用原有会话或仍有效的同一 access token 重新读取，确认 `developer=true`，再验证业务放行。
4. Join 设置 `is_developer=false`；Develop 重读后确认资格撤销、业务停止放行。
5. 用 Develop 的客户端凭据尝试写资格，应得到 `403 developer_update_forbidden`。

会话验证和 userinfo 会重新读取数据库。下游若缓存资格，需要自行约定缓存时间与失效方式；缓存会增加资格变化的生效延迟。已签发的 ID token 内容和下游已有副本不会自动改变。

## 错误处理约定

| 情况 | 接入方处理 |
| --- | --- |
| 400 参数错误 | 按对应接口的字段规则修正请求 |
| 401 应用凭据无效 | 检查客户端凭据与配置 |
| 401 用户凭据、会话或 token 无效 | 根据具体 `error` 引导登录或重新取得身份凭据 |
| 403 应用无接口权限 | 检查应用属性和资格写入名单 |
| 503 或网络超时 | 按接口的重试规则处理；会话验证失败时保留本地登录记录，暂停本次受保护业务 |

资格设置遇到 503 或超时，写入可能已经提交。恢复后查询，或重试仍然有效的同一审批目标；若已经有新的审批或撤销决定，以新决定为准。完整错误码与示例见每份接口文档。

## 版本与验证范围

接口行为对应修复提交 `f2d9e9c`。2026-11-17 的真实 UniSSO 后端已通过 60 项本地隔离检查，Join、Develop 由测试客户端模拟。已合入的代码是否在目标环境生效，由实际部署版本与配置确认。

实际 Join / Develop 业务联调，以及完整 OAuth 授权码、PKCE、刷新流程，尚未由这批检查覆盖。接入方在目标环境完成上面的链路验收；上游负责处理 UniSSO 接口与文档不一致的问题。

- [OpenAPI 3.1 定义](api/qualification.openapi.yaml)：本次接入的五个接口，供工具读取。
- [验证记录](../backend/tests/qualification/RESULTS.md)：隔离检查的环境、范围与复跑方式。
