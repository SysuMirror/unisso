# UniSSO 认证与授权安全审计

- **需求单**：SK-A726F6A7（缺陷 / 平台 unisso / normal）
- **审计对象**：`/home/cloud/repos/unisso`（快照 `63cc03f`）
- **审计分支**：`fix/yz-sk-a726f6a7`
- **审计方式**：只读源码审计（未修改任何业务代码）
- **审计范围**：登录流程、会话管理、OAuth2/OIDC token、CSRF、限流、密码策略与重置、相关配置与密钥管理
- **审计日期**：2026-11-23

> 说明：本文所有行号均以快照 `63cc03f` 为准，后续迭代可能发生偏移；引用时请以函数/代码片段为准。
> 本文不包含任何真实密钥、令牌、密码或用户凭据；示例值均为占位符。

---

## 0. 结论速览

整体安全基线**较好**：密码使用 bcrypt(cost=12)、随机数使用 OpenSSL CSPRNG、OAuth 授权码单次使用并强制 S256 PKCE、JWT 使用 RS256 且校验 `kid`、全局 CSRF token + Origin 校验、登录/重置统一错误信息、自助重置会撤销会话与 token。主要风险集中在**共享 SSO 会话凭据的设计**、**refresh token 生命周期**、**管理员操作与会话撤销不一致**、**若干限流缺口**。

| 编号 | 领域 | 级别 | 一句话结论 |
| --- | --- | --- | --- |
| F-01 | 会话 / 共享 SSO | **高** | 原始会话 ID 作为共享 Cookie 下发给全部 m2m 客户端，且 Cookie 由接入方 JS 写入（非 HttpOnly）、7 天滑动无绝对上限 |
| F-02 | Token | **高** | refresh token 轮换但无重用检测/家族撤销，7 天有效期内被窃即可长期续命 |
| F-03 | 会话 / 管理 | **高** | 超级管理员重置他人密码不撤销会话与 token（与自助重置行为不一致） |
| F-04 | 配置 / 提权 | **高** | 默认超级管理员集市 ID 硬编码并在每次启动时自动提权 |
| F-05 | 登录 / 限流 | **中** | 登录无按 IP/全局限流，且未知邮箱会同步调用外部集市登录（最长 20s）；`/api/oauth/token`、`introspect` 的 bcrypt 校验无限流 |
| F-06 | 会话 | **中** | 批量会话枚举/撤销被 `SCAN` 500 条上限截断，可能与“退出所有设备”承诺不符 |
| F-07 | 密码策略 | **中** | 口令最小长度 8、仅要求两类字符，无泄露口令库/新旧口令比对 |
| F-08 | Cookie / 缓存 | **中** | `Secure` 依赖 `X-Forwarded-Proto` 且无 `__Host-` 前缀；token 与身份响应缺 `Cache-Control: no-store` |
| F-09 | 信息暴露 | **中** | m2m `/users/list` 返回全量目录及 `is_admin/is_super_admin`，与代码注释“never admin flags”矛盾 |
| F-10 | 密钥 | **中** | 验证码/会话展示哈希的 HMAC 密钥在 `UNISSO_SECRET_KEY` 未配置时回落为公开常量 |
| F-11 | Token 撤销 | **中** | access token 撤销只在 userinfo/introspect 生效；本地验签的 RP 在 60 分钟内仍会接受 |
| F-12 | Token 撤销 | 低 | 删除/暂停应用不即时撤销已签发 access token（自然过期 ≤60 分钟） |
| F-13 | Host 校验 | 低 | 允许表为空时全放行，且任意原始 IP Host 放行，存在 DNS rebinding 面 |
| F-14 | 日志 | 低 | 未认证 `/api/logs/client` 可注入日志并放大存储 |
| F-15 | DoS | 低 | 单请求 body 上限 100MB 且每连接全量缓冲；慢连接占用 worker |
| F-16 | 信息暴露 | 低 | 登录/密码校验存在用户名枚举侧信道（SSE 分支、时间差） |
| F-17 | 运维 | 信息 | JWT 单一 `kid` 无轮换/历史公钥；无 `nbf` 与时钟偏移容忍 |
| F-18 | 邮件 | 低 | 注册/重置邮件在请求线程内同步发送（SMTP 20s 超时） |

---

## 1. 认证与登录流程

### 1.1 现状（正向）

- 密码校验：`backend/src/app/core.cpp:735-755`，bcrypt（libxcrypt，`$2b$`，cost 12），比较使用 `constantTimeEquals`。
- 登录入口：`backend/src/app/handlers_auth.cpp:93-188`。
  - 失败不区分“用户不存在/密码错误”，统一 `401 invalid_credentials`（:148-151）。
  - 登录成功前 `Cache::del(loginAttemptKey)` 清零失败计数（:177）。
- 登录失败计数与前缀：`handlers_auth.cpp:23-52`，5 次失败锁定 15 分钟，键为 `ip + identifier`。
- SSE（集市）账号回退：`handlers_auth.cpp:122-146`；本地校验失败且标识符含 `@` 时同步调用 `sse::login`。
- 用户名登录（不含 `@`）不会回退到 SSE（:112-114、:122），符合预期。

### 1.2 F-05 登录限流覆盖不足 + 外部同步调用放大（中）

**现状**

- 登录只有 `(ip, 提交的标识符)` 维度的 5 次/15 分钟锁定（`handlers_auth.cpp:23-52, 103-106`），**没有按 IP 的跨账号总量限制，也没有全局限流**。
- 本地账号不存在（或密码不匹配）且标识符是邮箱时，会在请求线程内**同步**调用外部集市登录：`handlers_auth.cpp:122-146`；外部 HTTP 客户端超时为连接 8s / 总 20s（`backend/src/app/sse.cpp:45-49`）。
- `POST /api/oauth/token` 与 `/api/oauth/introspect` 无速率限制；机密客户端的 secret 校验是 bcrypt（`handlers_oauth.cpp:169-185`，`core.cpp:749-755`）。
- Web 服务器使用 cpp-httplib，线程池 16（`backend/src/Server.cpp:11`）。

**风险场景与影响**

1. **凭据填充/撞库**：攻击者可用大量不同邮箱从同一 IP 发起尝试，每次仅受 `(ip,email)` 计数限制，可无限枚举账号与口令。
2. **worker 耗尽 / 可用性**：一次未知邮箱登录会同步等待外部集市最长 20s；16 个并发请求即可占满线程池，正常登录与全部 API 被阻塞。若上游集市故障或网络抖动，攻击者无需高带宽即可放大影响。
3. **CPU 放大**：知道任一机密 `client_id`（`client_id` 本身不是秘密）即可对 `/api/oauth/token` 持续提交错误 secret，每次触发一次 cost-12 bcrypt，且无速率限制。
4. **上游放大**：把本平台的登录流量转化为对集市 `/auth/login` 的撞库流量，可能触发上游风控/封禁。

**改进建议（按优先级/成本）**

1. **低成本的立即项**：为 `/api/auth/login` 增加按 IP 的固定窗口/滑动窗口限流（如 30 次/10 分钟），并在进入 `sse::login` 之前做一次独立的 `login_sse:` 计数器；对 `/api/oauth/token`、`/api/oauth/introspect` 以 `client_id + ip` 为键限流（如 60 次/分钟）。
2. **中成本**：把外部集市校验改为异步/带熔断（超时降至 3–5s，失败快速返回），或将“非本地账号”的 SSE 回退改成显式独立入口（避免任意登录请求都触发外部调用）。
3. **中成本**：对 bcrypt 校验失败增加服务端缓存（同一 `client_id + secret` 短时失败记忆）或改为 HMAC 预校验后再 bcrypt，避免无上限 CPU 消耗。
4. **低优先**：为登录增加指数退避/CAPTCHA（仅在异常流量下启用）。

### 1.3 F-16 用户枚举侧信道（低）

**现状**

- 邮箱不存在时走外部 SSE 分支；用户名不存在时直接返回（无 bcrypt）；而存在账号会执行 bcrypt（`handlers_auth.cpp:112-151`）。
- SSE 凭据正确但绑定冲突时返回专门的 `403 sse_bind_conflict`（:131-142），会暴露“该集市账号已绑定其他 UniSSO 账号”。

**影响**：可辅助枚举有效用户名/邮箱，以及集市身份与 UniSSO 账号的绑定关系。

**建议**：对 SSE 绑定冲突返回与普通失败相同的响应与状态码（仅在日志中区分）；统一未知用户与已知用户的响应耗时（例如未知用户也执行一次 dummy bcrypt）。

---

## 2. 会话管理

### 2.1 现状（正向）

- 会话 ID：`sess_` + 16 字节 CSPRNG（`core.cpp:1247-1269`，`randomHex` 使用 `RAND_bytes`，`core.cpp:235-249`）。
- Cookie：`unisso_session` 为 HttpOnly、`SameSite=Lax`、`Path=/`，未设置 `Domain`（host-only）（`core.cpp:1347-1350, 1176-1184`）。
- 登录即创建新会话（不沿用旧 ID），无会话固定问题（`handlers_auth.cpp:179-181`）。
- 禁用用户会使现存会话立即失效：`currentUser` 每次回查用户并检查 `isActive`（`core.cpp:1314-1315`）。
- 注销会删除当前会话及其 CSRF 记录（`core.cpp:1352-1359`）。
- 密码重置会遍历删除该用户全部会话，并撤销 access/refresh token（`handlers_password.cpp:84-96, 210-214`）。

### 2.2 F-01 共享 SSO 会话凭据设计（高）

**现状**

- m2m 接口 `POST /api/oauth/password/verify` 在密码校验成功后创建会话并**把原始 sid 明文返回**给调用方：`handlers_oauth.cpp:679-715`（`sso_session`、`sso_cookie_name`、`sso_cookie_domain`、`sso_max_age`，:668-675）。
- `POST /api/oauth/session/verify` 接受任意 sid，返回该用户身份并**滑动续期**：`handlers_oauth.cpp:717-767`，续期逻辑在 :753-760。
- 共享会话默认 168 小时（7 天）：`handlers_oauth.cpp:641-645`；Cookie 域默认 `.ssemarket.cn`：:647-649。
- 后端本身不写这个 Cookie；接入文档让各平台前端用 JS 写入 `unisso_sid`（`docs/api/password-verify.md:51-58, 74-81, 94`）。
- `sharedAuthorizeUser` 直接把该会话当作 OAuth 授权身份：`handlers_oauth.cpp:200-216`。

**风险场景与影响**

1. **原始 bearer 凭据横向扩散**：任何获得 `m2m_enabled` 的内部客户端都能拿到任意用户的完整 UniSSO 会话 ID。一个内部平台被攻陷（或日志/代理泄漏）即可冒充所有已登录用户访问 SSO 及其它接入方。
2. **非 HttpOnly、属性不可控**：共享 Cookie 由各平台 JS 写入，通常无法设置 HttpOnly；`Secure`/`SameSite` 取决于每个接入方实现。`.ssemarket.cn` 下任一子域的 XSS 都可窃取该 Cookie → 全域账号接管。
3. **滑动续期无绝对上限**：只要 7 天内调用过 `session/verify`，会话就再续 7 天；被盗会话理论上可无限续命。
4. **撤销语义弱**：`session/revoke` 只需持有 sid 即可删除会话（`handlers_oauth.cpp:769-782`），不校验调用方是否与该会话有关联，任意 m2m 客户端可造成拒绝服务。

**改进建议（按优先级/成本）**

1. **P0（中成本，建议优先）**：把“共享 Cookie 里放原始 sid”改为**短期、受众绑定的 SSO 断言**（如 5–15 分钟、`aud=接入方`、包含 `sub`/`iat`/`exp` 的签名 JWT 或一次性 ticket），接入方用断言换自己的本地会话，而不是持有 SSO 会话 ID。
2. **P0（低成本）**：若短期无法改造，至少让 UniSSO 自己在 `sso.ssemarket.cn` 上设置 **HttpOnly + Secure + SameSite=Lax 的 host-only Cookie**，并通过 SSO 重定向完成跨站登录；禁止接入方用 JS 写 SSO Cookie。文档同步更新。
3. **P1**：为共享会话增加**绝对过期上限**（如 7 天）与空闲超时（如 8–12 小时），`session/verify` 续期不得突破绝对上限。
4. **P1**：`session/revoke` 增加调用方与用户的关联校验（或改为由用户主动登出触发）。
5. **P2**：限制可获得 `sso_session` 的客户端范围（仅确需无感登录的一方），并在审计日志中记录每次签发/续期。

### 2.3 F-03 管理员重置密码不撤销会话（高）

**现状**

- 自助重置密码：删除该用户全部会话并撤销 access/refresh token（`handlers_password.cpp:84-96, 210-214`）。
- 超级管理员通过 `PUT /api/admin/users/{id}` 修改 `password` 时只更新 `password_hash`（`handlers_admin.cpp:687-695`），**没有** `revokeUserSessions`，也没有撤销 token。
- `DELETE /api/admin/users/{id}` 删除用户时同样未清理会话/token 行（`handlers_admin.cpp:710-739`；访问时会因用户不存在而失败，但 Redis 会话仍残留至 TTL）。

**风险场景与影响**：账号疑似被盗时，管理员“重置密码”是最常见的处置动作；但攻击者已建立的浏览器会话与 refresh token 在重置后仍可用（refresh 最长 7 天），处置不彻底。若管理员是把密码交给用户本人，也无法确保旧会话退出。

**改进建议（低成本，应尽快）**

- 在 `adminUsersUpdate` 检测到 `password` 非空、或 `is_active` 被置为 false、或 `is_admin/is_super_admin/is_developer` 变更时，调用与 `handlers_password.cpp` 相同的 `revokeUserSessions(userId)` 并撤销该用户全部 `access_tokens/refresh_tokens`。
- 将“撤销用户全部凭据”抽成 `authx::revokeAllForUser(userId)`，自助重置、管理员重置、封禁、删除统一调用。
- 删除用户时同时清理会话（即使失效兜底已存在，避免 Redis 残留与审计混淆）。

### 2.4 F-06 会话枚举/撤销的 500 条上限（中）

**现状**

- `Cache::scan` 默认 `maxKeys = 500`（`backend/src/app/core.h:197`）。
- 下列逻辑都只扫描前 500 个 `session:*` 键：
  - 自助重置撤销全部会话：`handlers_password.cpp:84-96`（`Cache::scan("session:*", 500)`）
  - 设备列表：`handlers_sessions.cpp:55-93`（:60）
  - 单设备撤销：`handlers_sessions.cpp:95-123`（:104）
  - 退出其他设备：`handlers_sessions.cpp:125-144`（:130）
- `SCAN` 不保证一次返回全部键，且这里固定上限。

**风险场景与影响**：当全站会话数超过 500（多平台共享 SSO 后很容易达到）时，“退出其他设备”“密码重置后全部下线”可能静默漏掉部分会话，安全承诺（docs/FEATURES.md 中“重置成功会删除该用户全部会话”）无法保证。攻击者会话可能幸存。

**改进建议**

- **低成本**：为每个用户维护一个 Redis Set `user_sessions:<userId>`（创建时 `SADD`，删除时 `SREM`），撤销/列表直接按集合遍历，复杂度与用户会话数成正比而非全站键数。
- 或在 `Cache::scan` 的调用处循环 `SCAN` 游标直到 0 或达到硬上限，并对超出上限的情况记录告警。
- 为用户会话数设置上限（如 20/用户），超限时淘汰最旧会话。

### 2.5 F-08 Cookie 属性与缓存（中）

**现状**

- `Secure` 仅在 `X-Forwarded-Proto`/`X-Forwarded-Scheme` 为 `https` 时添加（`core.cpp:1132-1135, 1176-1184`），且该判断不要求来自受信代理。
- Cookie 名为 `unisso_session` / `unisso_csrf_context`，无 `__Host-`/`__Secure-` 前缀。
- `securityHeaders` 设置了 CSP、XFO、nosniff、Referrer-Policy、HSTS（仅 https）（`core.cpp:1190-1207`）。
- `/api/oauth/token`（`handlers_oauth.cpp:425, 473`）与登录/me 等身份响应通过 `http::json` 返回，未设置 `Cache-Control: no-store`。

**风险场景与影响**：若反向代理漏配 `X-Forwarded-Proto`，会话 Cookie 会在明文 HTTP 下传输（内网/中继段被嗅探即会话接管）。共享缓存或浏览器历史缓存可能留存含 token 的响应。

**改进建议**

- **低成本**：在 `issuer` 为 `https://` 时强制 `Secure`（或增加 `UNISSO_COOKIE_SECURE=1` 显式开关），不要只依赖转发头。
- **低成本**：会话/CSRF Cookie 改用 `__Host-` 前缀（需保证 `Path=/`、无 `Domain`、`Secure`）。
- **低成本**：对 `/api/oauth/token`、`/api/auth/*`、`/api/oauth/userinfo` 等响应统一加 `Cache-Control: no-store, Pragma: no-cache`。

### 2.6 F-11 access token 撤销的传播范围（中）

**现状**

- 撤销以 `jti` 落库（`access_tokens/refresh_tokens`），`tokens::revoked` 查库且在记录缺失时按“已撤销”处理（fail-closed）：`backend/src/app/domain.cpp:657-664`。
- 但只有 `userinfo`（`handlers_oauth.cpp:533-559`）与 `introspect`（:480-531）调用 `tokens::revoked`；资源服务器若在本地验签 JWT，无法感知撤销，access token 在 60 分钟（`core.cpp:116-117` / `config.yml:39`）内仍被接受。

**风险场景与影响**：密码重置、consent 撤销、应用暂停后，外部 RP 仍可能凭旧 access token 访问最多 60 分钟。

**改进建议**

- 缩短 access token 有效期（如 15 分钟）并依赖 refresh 轮换。
- 在接入文档中明确：RP 必须调用 `/api/oauth/introspect` 或 userinfo 校验撤销状态；对高敏感 scope 强制在线校验。
- 长期：把撤销状态发布到资源服务器的共享缓存/事件总线。

### 2.7 F-12 应用删除/暂停后的 token（低）

**现状**：`appsReview` 的 suspend/reject 会置 `is_active=0`（`handlers_admin.cpp:331-343`），refresh/token 端点随后会因 `verifyClient` 拒绝（`handlers_oauth.cpp:169-185`）；但**已签发 access token** 在过期前仍可用于 userinfo（userinfo 不检查应用状态）。`appsDelete` 只删应用与 consent（`handlers_admin.cpp:431-442`），同样不撤销 token。

**建议**：删除/暂停应用时把该 `client_id` 的 access/refresh token 标记撤销（`UPDATE ... SET revoked=1 WHERE client_id=...`），与 `appsUpdate` 在 redirect/scope 变化时的做法一致（`handlers_admin.cpp:421-426`）。

---

## 3. CSRF 防护

### 3.1 现状（正向）

- 全局中间件：`backend/src/main.cpp:40-51` 对每个请求先加安全头、校验 Host、再校验 CSRF。
- `validateCsrf`（`core.cpp:1381-1427`）：
  - 所有非 `GET/HEAD/OPTIONS` 默认必须携带 token；
  - token 与 `session:<sid>` 或匿名上下文 `anonymous:<cookie>` 绑定，存 Redis，`constantTimeEquals` 比较（:1410-1425）；
  - Origin 必须在允许列表内（:1392-1404），`Sec-Fetch-Site: cross-site` 直接拒绝（:1405-1408）。
- 匿名 CSRF 上下文 Cookie 为 HttpOnly（`core.cpp:1367-1372`）。
- 前端仅通过同源请求读取 token 并放入 `X-CSRF-Token`（`frontend/src/api.js:8-36`）。
- 服务端不输出 `Access-Control-Allow-Origin`（`Server.cpp` 中仅有注释，未实际添加），跨源 JS 无法读取 `/api/csrf-token`，双提交模型成立。

### 3.2 观察与建议

- **正向**：登录接口 `POST /api/auth/login` 未在豁免列表中（`core.cpp:1384-1390`），因此登录 CSRF 已防护。
- **注意（信息）**：`SameSite=Lax` 把同站不同子域视为 same-site；若未来引入宽松 CORS 或 JSONP，兄弟子域可能具备发起请求的能力。当前 token 机制仍能阻断，建议保持“无 CORS 白名单”策略。
- **豁免面（低）**：`/api/logs/client` 被 CSRF 豁免（`core.cpp:1386`）且无需认证，仅靠 120 次/分钟限流（`handlers_logs.cpp:20-27`），可被用于日志注入/容量放大（见 F-14）。
- **建议**：在文档与代码注释中明确“CSRF 依赖 token，不依赖 SameSite”，避免后续把 token 校验移除。

---

## 4. OAuth2 / OIDC token

### 4.1 现状（正向）

- 授权码：256-bit 随机（`util::randomToken(32)`），`GETDEL` 单次消费，600 秒 TTL：`handlers_oauth.cpp:34-59`。
- PKCE：强制 `S256`，challenge 长度/字符集校验，常量时间比对：`handlers_oauth.cpp:24-32, 153-157, 407-412`。
- redirect_uri：与应用注册值精确匹配：`handlers_oauth.cpp:143-147, 403-406`。
- 客户端认证：机密客户端必须提供 secret（bcrypt 校验），公开客户端不得携带 secret：`handlers_oauth.cpp:169-185`。
- JWT：RS256，签名后校验 `kid`，始终使用 RSA 公钥验签（不存在 `alg=none`/HS256 混淆）：`domain.cpp:506-530`；`iss/aud/exp/iat/type/jti/sub` 均校验（:625-655）。
- 授权码换 token 时校验 client 一致、redirect 一致、PKCE、用户有效：`handlers_oauth.cpp:386-417`。
- consent 撤销会撤销对应用户+客户端的 token：`handlers_admin.cpp:539-559`（与 `tokens::revokeForConsent` 等价）。
- discovery 仅声明 `code` + `refresh_token`、`S256`、RS256：`handlers_oauth.cpp:562-584`。

### 4.2 F-02 refresh token 无重用检测（高）

**现状**

- refresh token 为 RS256 JWT，有效期 7 天（`domain.cpp:590-605`；`config.yml:40`）。
- 刷新时：校验 JWT → 查 `revoked(jti,"refresh")` → 校验 client → **撤销旧 refresh jti** → 签发全新 access+refresh：`handlers_oauth.cpp:429-474`（撤销在 :466，签发在 `tokens::issue`，`domain.cpp:666-711`）。
- 没有 token family / 重用检测：若已被轮换的 refresh token 被重放，仅返回 `invalid_grant`，不会撤销当前有效家族（`handlers_oauth.cpp:441-444`）。
- 刷新沿用原 scope，不做降权（:467）。

**风险场景与影响**

1. 攻击者窃取 refresh token 后可持续刷新（7 天内），即使合法用户也刷新过，攻击者仍持有最新链的一支；服务端无异常告警。
2. 若受害者与攻击者交替使用，双方会互相把对方的 token 轮换失效，表现为随机掉线而非“检测到重用并全面撤销”。
3. access token 60 分钟偏长，放大撤销延迟。

**改进建议（按优先级/成本）**

1. **P0（中成本）**：refresh token 引入 `family_id`（首次签发生成，后续轮换继承）。轮换时原子地“作废旧 jti + 写入新 jti”；若收到已被作废的 jti，**撤销整个 family** 的所有 refresh token 并记录安全事件。
2. **P1（低成本）**：把 refresh 有效期与“最后使用时间”绑定（如空闲 7 天 / 绝对 30 天），access token 降到 15–30 分钟。
3. **P1（低成本）**：refresh 时不再返回新 refresh 之外的敏感字段；对公开客户端（`isConfidential=false`）使用更短 refresh TTL。
4. **P2**：对同一用户+客户端的异常刷新频率/地理位置做告警。

### 4.3 F-09 m2m 用户目录与管理标志（中）

**现状**

- `oauthUsersList` 注释声明“never password hashes or admin flags”（`handlers_oauth.cpp:784-787`），但它使用 `userClaimsJson`（:651-666），其中包含 `is_admin`、`is_super_admin`（:663-664）；`usersList` 还会附加 `active/created_at/last_login_at`（:792-799）。
- `oauthPasswordVerify` 与 `oauthSessionVerify` 也返回同样的 claims（:712, :764）。
- 这些接口要求机密客户端 + `kind=internal` + `m2m_enabled`（`handlers_oauth.cpp:595-639`）。

**风险场景与影响**：所有启用无感登录的第一方平台都能拉取全量用户目录（邮箱、学号、姓名、管理员标志、是否院内、开发者标志、最后登录时间）。一旦某个内部平台被攻陷或误用，等于泄露全站用户清单与管理员名单，便于定向钓鱼/提权侦察。注释与实现不一致也会误导后续维护者。

**改进建议**

- 明确产品需求：若不需要管理员标志，从 `userClaimsJson` 中移除 `is_admin/is_super_admin`（或仅对超级管理员显式请求的调试接口返回）。
- `/users/list` 增加分页与最小化字段（默认不含 `is_admin`、`last_login_at`），并记录调用审计。
- 在 API 文档中明确“内部平台可见的字段”，与代码保持一致。

### 4.4 F-17 JWT 密钥生命周期（信息/低）

**现状**：单一 `kid`、单对密钥（`domain.cpp:488-504`），无历史公钥、无轮换流程；`verify` 只接受当前 `kid`（:527）。未校验 `nbf` 且无时钟偏移容忍（:642-649）。

**影响**：密钥轮换会使所有已签发 token 立即失效（运维风险）；RP 与服务端时钟偏差会导致边界请求失败。

**建议**：支持 JWKS 多公钥（保留旧公钥一个 token 生命周期）；增加 `nbf` 可选校验与 ±60s skew；把轮换步骤写入 runbook。

### 4.5 授权页与 consent（正向）

- 授权页强制显示同意屏，不复用旧 consent 静默发码（`handlers_oauth.cpp:276-293`）。
- `as_user_id` / `as_sse_identity` 只允许选择当前账号或已绑定身份（:332-351）。
- `oauthContext` 返回账号邮箱/姓名等给当前已登录用户本人，属预期。

---

## 5. 密码策略与重置

### 5.1 现状（正向）

- 哈希：bcrypt cost 12（`core.cpp:735-747`），常量时间比较（:749-755）。
- 强度校验：长度 8–128、至少两类字符、常见弱口令精确黑名单、禁止 4 个相同字符、禁止连续数字序列：`core.cpp:757-792`。
- 重置流程：
  - 6 位验证码，HMAC 存储，10 分钟有效，60 秒冷却；邮箱 5 次/10 分钟、IP 20 次/10 分钟：`handlers_password.cpp:30-61, 128-167`。
  - 确认环节 5 次尝试后作废验证码：`handlers_password.cpp:178-184`。
  - 成功重置后删除验证码、撤销全部会话与 token：`handlers_password.cpp:196-215`。
  - 无论邮箱是否存在都返回 `sent: true`，防止枚举（:148-166）。
- 注册：邮箱 3 次/小时、IP 10 次/小时、验证码 5 次尝试作废：`registration.cpp:149-155, 291-295`。
- 管理端创建/修改用户也走同一 `validateStrength`：`handlers_admin.cpp:604, 689`。

### 5.2 F-07 口令策略强度（中）

**现状**：最小 8 位、两类字符；未接入泄露口令库；未阻止“新密码 = 旧密码”；未阻止包含用户名/邮箱/学号；弱口令黑名单仅 12 个精确匹配（`core.cpp:771-777`）。

**风险场景与影响**：中央 SSO 一旦被撞库，影响面覆盖所有接入平台。8 位 + 两类的组合允许 `Passw0rd` 类弱口令；用户也可能把密码重置为原密码，使“重置即失效”处置失效。

**改进建议（按优先级/成本）**

1. **低成本**：重置/修改时校验新密码不等于旧密码（需要能拿到旧 hash 并 verify，重置流程可用 `user.passwordHash`）。
2. **低成本**：把最小长度提到 10–12，或引入口令短语（passphrase）模式；黑名单扩展到 top-10k 并做大小写/Leet 归一化。
3. **中成本**：接入 k-Anonymity 的泄露口令查询（HIBP range API / 离线库），仅发送 SHA-1 前缀。
4. **中成本**：禁止密码包含邮箱本地部分、用户名、学号。
5. 长期：支持 TOTP/WebAuthn 二次因素（当前代码已移除 step-up 相关表与流程，若产品仍需二次验证应重新设计）。

### 5.3 F-10 验证码/展示哈希的密钥回落（中）

**现状**

- `AppConfig::secretKey` 来自 `UNISSO_SECRET_KEY` 或 `jwt.secret`，默认空（`core.cpp:114`）。
- 验证码哈希与设备会话展示 ID 在 `secretKey` 为空时回落到公开的 `signingKeyId` 字面量：
  - 密码重置/注册验证码：`handlers_password.cpp:37-41`、`registration.cpp:136-140`
  - 会话展示哈希（`publicSessionId`）：`handlers_sessions.cpp:20-24`
- 验证码本身是 6 位数字（空间 90 万），在线尝试有 5 次限制，但哈希密钥公开会削弱“Redis 只读泄露”场景下的防护。

**风险场景与影响**：若 Redis 备份/只读访问泄露，攻击者可离线穷举验证码哈希（90 万次 HMAC 很快）并重置密码；会话展示哈希虽不可逆推 sid，但也失去密钥隔离。

**改进建议（低成本）**

- 启动时若 `UNISSO_SECRET_KEY` 为空，**拒绝启动**（fail-fast），或在 `secrets/` 下自动生成 32 字节随机密钥文件（600 权限，gitignored）并加载。
- 验证码改用 8 位字母数字（或至少 6 位 + 更严格限流），并继续使用独立密钥 HMAC。
- 在部署 runbook 中把 `UNISSO_SECRET_KEY` 列为必填项。

### 5.4 F-04 默认超级管理员硬编码提权（高）

**现状**

- `configuredSuperAdminIds()` 在环境变量 `UNISSO_SSE_SUPER_ADMIN_IDS` 缺失时默认返回 `"561"`（`backend/src/app/migrations.cpp:31-34`）。
- `migrations::run()` 在**每次启动**执行 `UPDATE users SET is_super_admin=1, is_admin=1 WHERE sse_user_id IN (...)`（:97-101），不在一次性 migration flag 之后。
- 只有显式设置 `-1` 才能禁用（:92）。

**风险场景与影响**

- 数据库恢复、测试库导入、或集市用户 ID 被回收/复用时，该 ID 会静默获得平台最高权限。
- 默认值使“未配置”与“已授权”无法区分；审计中难以证明超管来源。
- 每次启动重复提权，管理员在控制台取消后会被下一次重启覆盖。

**改进建议（低成本，应尽快）**

- 移除默认值：未设置 `UNISSO_SSE_SUPER_ADMIN_IDS` 时不做任何提权；文档要求显式配置。
- 把提权改为**一次性 migration**（用 `migration_flags` 标记），而不是每次启动执行；控制台成为唯一持续管理入口。
- 记录提权审计日志（操作者=启动配置、目标用户、时间）。

---

## 6. 限流与可用性

### 6.1 现有覆盖

| 接口 | 限制 | 位置 |
| --- | --- | --- |
| 登录 | 5 次/15 分钟，键 `ip+identifier` | `handlers_auth.cpp:23-52` |
| 密码重置发送 | 邮箱 5/10min，IP 20/10min，60s 冷却 | `handlers_password.cpp:47-61` |
| 密码重置确认 | 5 次后作废验证码 | `handlers_password.cpp:178-184` |
| 注册/重发 | 邮箱 3/60min，IP 10/60min，3/15min | `registration.cpp:149-155` |
| 注册验证 | 5 次后删除 pending | `registration.cpp:291-295` |
| m2m 密码校验 | `ip+email` 5 次/15min | `handlers_oauth.cpp:690-697` |
| 扫码启动/扫描/确认 | 20–60 次/分钟 | `handlers_qrlogin.cpp:71-75, 160-163, 221-223, 268-270` |
| 集市扫码启动 | 20 次/分钟/IP | `handlers_sse.cpp:99-102` |
| 前端日志 | 120 次/分钟/IP | `handlers_logs.cpp:20-27` |
| **`/api/oauth/token`** | **无** | `handlers_oauth.cpp:365-478` |
| **`/api/oauth/introspect`** | **无** | `handlers_oauth.cpp:480-531` |
| **`/api/oauth/session/verify`** | **无** | `handlers_oauth.cpp:717-767` |
| **管理员接口** | 无（仅权限校验） | `handlers_admin.cpp` |

### 6.2 F-05 / F-15 / F-19 的补充说明

- **F-05** 见 §1.2：登录缺 IP/全局限流 + 同步外部调用；token/introspect 的 bcrypt 无速率限制。
- **F-15（低）**：请求头上限 1MB、body 上限 100MB，且按 `Content-Length` 全量读入内存后再处理（`backend/src/Server.cpp:567-569, 625, 766`）。未认证请求即可占用大内存与连接；建议按路由设置更小的 body 上限（登录/重置 16KB，上传单独 5MB/2MB 已有业务校验）。
- **F-19（低）**：注册/重置邮件在请求线程内同步发送，SMTP 超时 20s（`registration.cpp:98`、`handlers_password.cpp:63-82`）。已有限流，但在高并发下仍会占用 worker；建议邮件发送改为队列/异步（失败重试），并将 SMTP 超时降至 5–10s。

### 6.3 建议的限流补全

1. `/api/oauth/token`：按 `client_id + ip` 60 次/分钟；对 `invalid_client` 单独计数并指数退避。
2. `/api/oauth/introspect`：按 `client_id + ip` 120 次/分钟。
3. `/api/oauth/session/verify`：按 `client_id + ip` 300 次/分钟（正常无感登录频率应远低于此）。
4. 登录：增加按 IP 30 次/10 分钟 + 全站熔断阈值；SSE 回退单独限流。
5. 管理员敏感操作（重置密码/改权限/删除用户/重置 secret）：按操作者 10 次/分钟，并写审计日志。

---

## 7. 配置、密钥与信息暴露

### 7.1 现状（正向）

- `backend/config.yml` 中数据库/Redis/SMTP 密码均为空，真实值来自环境变量（`config.yml:1-5`）。
- `backend/secrets/`、`config.local.yml`、`.env` 等在 `.gitignore` 中（`.gitignore:5, 22`）。
- 签名私钥加载失败时进程拒绝启动（`domain.cpp:488-502`、`main.cpp:32-35`）。
- `clientIp` 正确处理受信代理链，右侧起找第一个不可信跳（`core.cpp:1092-1122`）。
- 安全响应头齐全（CSP、X-Frame-Options、nosniff、Referrer-Policy、HSTS）（`core.cpp:1190-1207`）。
- 日志中不打印密码/secret；登录失败仅打印邮箱（`handlers_auth.cpp:143-145`）。

### 7.2 F-13 Host 校验（低）

**现状**：`allowHost` 在 `allowedHostsJson` 为空时直接放行；`localhost/127.0.0.1/[::1]` 与**任意数字开头/含冒号的原始 IP** 一律放行（`core.cpp:1429-1443`）。配置中有 `allowed_hosts`（`config.yml:86-88`），但原始 IP 分支始终生效。

**影响**：DNS rebinding 场景下，攻击者控制的域名解析到服务 IP 时可绕过 Host 白名单（Origin 校验和 CSRF token 仍提供第二层防护）。健康检查依赖原始 IP 是合理需求。

**建议**：保留仅对 `/health`、`/api/health` 的原始 IP 放行；其余路径要求 Host 在允许列表中。将 `allowedHostsJson` 为空视为配置错误（启动告警或 fail-fast）。

### 7.3 F-14 未认证日志端点（低）

**现状**：`POST /api/logs/client` 免认证、免 CSRF、120 次/分钟/IP，单条 message 1000 字符、stack 2000 字符（`handlers_logs.cpp:20-38`）。

**影响**：可被用于写入噪声/伪造前端错误，污染审计与排障；多 IP 情况下可放大日志体积。换行已被清洗，Vue 前端默认转义，未发现 XSS。

**建议**：要求登录后才接收 `stack`（或整体要求会话）；对 URL 字段做域名白名单；日志落盘前统一加来源标记；对未认证条目单独分文件并限制总量。

---

## 8. 正面控制清单（建议保留并在回归测试中覆盖）

1. bcrypt cost 12 + 常量时间比较（`core.cpp:735-755`）。
2. 全部 token/会话/验证码使用 OpenSSL `RAND_bytes`（`core.cpp:235-249`）。
3. 授权码 256-bit、`GETDEL` 单次消费（`handlers_oauth.cpp:47-59`）。
4. 强制 S256 PKCE、redirect_uri 精确匹配、client 绑定（`handlers_oauth.cpp:24-32, 386-412`）。
5. JWT RS256 + `kid` 校验、拒绝错误 kid、全字段校验（`domain.cpp:518-530, 625-655`）。
6. `revoked()` 记录缺失时 fail-closed（`domain.cpp:657-664`）。
7. 全局 CSRF token + Origin + Sec-Fetch-Site（`core.cpp:1381-1427`、`main.cpp:40-51`）。
8. 登录 CSRF 已防护、登录失败统一错误（`handlers_auth.cpp:148-151`）。
9. 会话 ID 登录时重新生成，无固定漏洞（`handlers_auth.cpp:179-181`）。
10. 禁用用户即时失效会话（`core.cpp:1314-1315`）。
11. 自助重置撤销全部会话与 token（`handlers_password.cpp:210-214`）。
12. consent 撤销撤销 token（`handlers_admin.cpp:539-559`）。
13. 应用 redirect/scope 变更时撤销 token（`handlers_admin.cpp:421-426`）。
14. 受信代理外的客户端不能伪造 `clientIp`（`core.cpp:1092-1122`）。
15. 不输出 CORS 头，CSRF token 不可跨源读取（`Server.cpp:793-799` 仅注释，无实际头）。
16. 密钥不入库、不落日志；签名私钥缺失拒绝启动（`domain.cpp:488-502`）。

---

## 9. 改进路线图（按风险/成本排序）

### P0（建议 1–2 周内）

1. **F-01**：停止把原始 sid 明送给接入方；改为 HttpOnly host-only SSO Cookie + 短期断言，或受众绑定的短期票据。
2. **F-03**：管理员重置密码/封禁/删除时统一撤销该用户全部会话与 token。
3. **F-04**：移除默认超管 ID，提权改为一次性 migration。
4. **F-02**：refresh token 家族化 + 重用检测，缩短 access TTL。

### P1（1 个月内）

5. **F-05**：补全 login/token/introspect 限流；SSE 回退异步化/熔断。
6. **F-06**：用户会话索引，消除 500 条扫描上限。
7. **F-10**：`UNISSO_SECRET_KEY` 必填或自动生成持久密钥。
8. **F-08**：强制 Secure、`__Host-` 前缀、`no-store`。
9. **F-09**：m2m 用户字段最小化与审计。

### P2（季度内）

10. **F-07**：口令策略增强（泄露库、新旧比对、长度）。
11. **F-11 / F-12**：缩短 access TTL，暂停/删除应用撤销 token，文档强调 introspection。
12. **F-13 / F-14 / F-15 / F-19**：Host 精确化、日志端点收敛、body 上限分路由、邮件异步化。
13. **F-17**：JWKS 多公钥与密钥轮换 runbook。

---

## 10. 审计方法与局限

- **方法**：通读认证/会话/token/密码/管理端相关源码（`backend/src/app/*.cpp`、`backend/src/app/core.cpp`、`backend/src/app/domain.cpp`、`backend/src/Server.cpp`、`backend/main.cpp`、前端 `frontend/src/api.js`、`backend/config.yml`、`docs/api/*`），逐项对照需求单的六类要求。
- **未执行**：动态渗透测试、生产流量分析、依赖漏洞扫描（SBOM/CVE）。本沙箱缺少 `cmake`、MariaDB、hiredis/mysqlclient 开发头文件，无法构建二进制并运行 `backend/tests/qualification/run.py`；因此本文结论均来自静态阅读。
- **未覆盖**：第三方依赖（OpenSSL/libcurl/cpp-httplib/libxcrypt）本身的 CVE、宿主平台（K3s/nginx/frps）配置、邮件服务商与集市上游的安全属性。
- **行号漂移**：以快照 `63cc03f` 为准，后续提交后请按函数名定位。
- **保密**：本文未记录任何真实密钥、令牌、密码或用户数据；示例仅为占位符。

---

## 附录 A：需求维度对照

| 需求维度 | 对应章节 | 结论要点 |
| --- | --- | --- |
| 登录流程（密码校验、失败处理） | §1 | 统一错误与 bcrypt 良好；缺 IP 限流、SSE 同步放大、枚举侧信道 |
| 会话管理（Cookie 属性、过期、滑动续期、注销/撤销、固定） | §2 | host cookie 属性良好；共享 SSO 凭据设计是最大风险；撤销扫描上限；管理员重置不撤销 |
| token（签发、轮换、撤销、存储） | §4 | 签发与 PKCE 强；refresh 缺重用检测；撤销传播有限；应用删除不撤销 |
| CSRF（token/Origin） | §3 | 实现完整；Login CSRF 已防护；需保持无 CORS |
| 限流（登录、密码校验、敏感接口） | §6 | 多数接口有，但登录缺 IP/全局，token/introspect 缺 |
| 密码策略（强度、哈希、重置） | §5 | bcrypt 12 良好；策略偏弱；重置流程设计良好但密钥回落风险 |
