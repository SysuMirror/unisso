# UniSSO v2 安全修复记录

日期：2026-10-25。范围：`backend/` C++ 服务、Vue SPA、部署脚本。

> 说明：这是修复记录，不是“零漏洞”证明。已修复项均有对应接口/回归测试；
> 残余风险在最后一节列出，需要按部署规模继续处理。

## 已修复

| 编号 | 问题 | 修复 |
|---|---|---|
| C1 | 静态文件路径穿越（`//absolute`、`..`、编码变体可读私钥） | `pathIsSafe` 拒绝空/非根/`..`/反斜杠/NUL/`//`；`safeJoin` 用 `weakly_canonical` 做根目录包含校验 |
| H1 | 递归 JSON 解析可打爆栈 | 解析深度上限 64；畸形/超深 JSON 直接 400 `invalid_body`；请求体上限 8 MiB |
| H2 | 信任首个 `X-Forwarded-For`，可伪造 IP 绕过限流 | 仅当 TCP peer 是 relay（`10.42.0.1`）或 loopback 时才读转发头，取**最右**一跳；其它 peer 直接用 TCP 地址 |
| H3 | 账号关联密码/验证码可无限爆破 | 每对关联账号失败 5 次后锁定 15 分钟（Redis 计数） |
| H4 | 集市邮箱与本地账号冲突时静默接管；`561` 硬编码超管 | 本地密码账号不再按邮箱自动合并；同一邮箱已绑其他 SSE id 时拒绝；超管 id 改为 `UNISSO_SSE_SUPER_ADMIN_IDS`（默认 `561`） |
| H5 | 任意登录用户可注册 OAuth 客户端、任意回调/scope | 应用注册仅管理员；回调仅 HTTPS 或 localhost；scope 白名单；更新回调/scope 时撤销 consent 与 token |
| M1 | 扫码 poll 可被构造链接造成登录 CSRF | `qr/start` 设置 HttpOnly state cookie，`poll` 必须同浏览器 cookie 匹配 |
| M2 | 集市绑定 state 不与会话绑定、可重放 | 绑定回调要求当前会话 == state 中的用户；`setnx` 一次性 claim |
| M4 | 集市回调错误参数反射进 HTML | 输出 HTML 转义 |
| M5 | 管理员可越权管理超级管理员/全局用户 | 用户/日志管理改为 `requireSuperAdmin`；应用管理保留管理员 |
| M6 | 未认证 QR start 可刷 Redis 键 | 每 IP 每分钟最多 20 次 |
| M9 | `POST /api/identities` 可预占任意集市身份 | 路由已移除；外部身份只能走已验证的集市 OAuth 回调 |
| M12 | introspect 未认证可查询任意 token | 必须提供 `client_id` + 对应 secret（public client 只允许 client_id），且只能查询本 client 的 token |
| — | 新设备/长期未登录账号直接进入会话 | 新设备（无可信设备 cookie）或超过 `step_up_days`（默认 7 天）时，密码通过后只发一次性二次验证挑战，不发 session；邮箱码或动态人脸通过后才建会话、更新 `last_login_at`，并写入 180 天设备信任 |
| — | 人脸绕过密码 | 没有人脸登录接口；人脸只在已通过密码的二次验证或登录后的登记流程中使用 |
| — | 人脸照片留存 | UniSSO 不保存照片/帧；Upstream 只保留 512 维特征；审计不含图像/特征；解绑时无其他关联则删除 person |
| — | 人脸登记冒用已有脸 | probe 命中已有 person 后，必须验证当前账号邮箱码才允许关联；选择“不是”则重新活体采集新建 person |
| — | 明文密码 | 全部密码 bcrypt `$2b$12$`；数据库已核验无明文/非 bcrypt 行 |
| — | 日志/密钥权限 | `config.local.yml`、`secrets/*.pem` 改为 600 |

## 已核验为安全的部分

- SQL 拼接处统一 `Db::quote` / `Db::nullable`；未发现可注入点。
- OAuth 授权码一次性消费、PKCE S256 校验、redirect_uri 精确匹配、client 认证。
- JWT RS256 校验 `iss/aud/exp`，JWKS 只暴露公钥；refresh 轮换并撤销旧 token。
- 会话 Cookie HttpOnly/SameSite/Secure；CSRF token + Origin + `Sec-Fetch-Site` 校验。
- 主要对象（用户、consent、应用、关联账号）均有归属校验，未发现 IDOR。
- 头像/图标上传校验 MIME 魔数与大小；文件名用 UUID，不落用户输入。
- CSP、`X-Frame-Options: DENY`、`nosniff`、Referrer-Policy 已设置。

## 减法说明

按“SSO 只做身份”的要求，已移除：

- OAuth `groups` scope 与 `organizations` claim；
- 业务“用户组”页面和按用户组授权的接口；
- OAuth 返回用户组/组织权限的路径。

后续按应用管理需要恢复了 **组织（organization）**：它只用于应用归属、组织管理员/开发者
权限，不进入任何 OAuth claim，也不是应用内业务用户组。组织接口都做 org 归属校验：
组织管理员/开发者只能管理自己所属组织，超级管理员可管理全部。

## 残余风险 / 后续

1. **MySQL/Redis 单连接**：所有查询串行化。身份类接口在 100 并发下约
   185 req/s；建议后续换连接池/读写分离。这是性能问题，不是越权问题。
2. **全局限流**：只有登录/注册/绑定/QR start 等敏感接口有定向限流；静态与
   健康检查无全局限流，公网部署建议在 relay/nginx 层加总限流与 WAF。
3. **HTTP 传输**：`cpp-httplib` 每连接线程模型；`ab -k` 并发 ≥100 时出现
   `Length` 计数（连接回收），并发 50 与无 keep-alive 高并发为 0 failed。
4. **人脸模型许可/PAD**：`buffalo_l` 为非商业研究许可；PAD 阈值未大规模标定；
   动态动作挡不住实时换脸/虚拟摄像头。高价值账号建议叠加 TOTP/硬件密钥。
5. **外部依赖**：SSE OAuth、SMTP、Face 服务失败时的重试/告警只写日志；
   没有独立监控。
6. **审计日志**：管理操作和人脸事件写应用日志/`face_audits`，但还没有防篡改的
   独立审计存储；人脸审计建议保留 180 天。
7. **设备信任**：`unisso_device` 是 180 天 HttpOnly cookie；清除 cookie/无痕窗口
   会重新触发二次验证；当前没有设备列表/远程撤销页面。
