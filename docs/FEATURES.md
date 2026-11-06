# UniSSO v2 功能说明

本文档说明当前部署的全部功能、接口、配置、安全边界和运维方式。

- 用户入口：`https://sso.ssemarket.cn`
- OIDC issuer：`https://sso.ssemarket.cn`
- Discovery：`https://sso.ssemarket.cn/.well-known/openid-configuration`
- 人脸服务：`https://ai.ssemarket.cn/face`
- 后端日志：`/home/cloud/deploy/logs/`

> 总原则：UniSSO 只做身份认证。**人脸不是登录方式**，只作为密码登录后的
> 二次验证（新设备 / 长期未登录）或账号内的人脸登记；应用自己的角色、用户组、
> 资源权限仍由应用自己管理。
>
> 上游人脸文档中描述的“刷脸直接登录 / 多账号账号选择器”**不适用于本策略**；
> 本部署没有刷脸登录入口，也没有“人脸命中多个账号后选择账号登录”的路径。
> 人脸只会验证当前已登录（或刚通过密码）的账号。

---

## 1. 登录方式

### 1.1 邮箱密码

- bcrypt `$2b$12$` 存储，不存明文。
- 登录失败按 IP + 邮箱计数，5 次后锁定 15 分钟。
- 邮箱验证码登录不是独立入口；注册、二次验证、人脸关联确认会发验证码。

### 1.2 集市（SSE）账号密码

本地密码不匹配时回退到集市 `/auth/login`；成功后把集市身份映射为内部账号
（`is_internal=1`、`sse_user_id`），头像只同步一次。

### 1.3 扫码登录（两种方式，登录页可切换）

登录页扫码面板有「手机扫码」和「集市扫码」两个切换标签，各自提示适用范围，
避免“手机登录了统一身份认证却没登录集市”时扫错码。

**手机扫码（UniSSO 手机端确认，默认）**

- 二维码内容是 `https://sso.ssemarket.cn/qr/<id>`；`id` 为 32 位随机 token，
  挑战存 Redis `qr_login:<id>`（TTL 300 秒）。
- 手机打开链接：未登录时先走正常登录（登录后自动回到确认页，即“检查手机
  登录状态”），已登录则直接看到确认页。
- 确认页展示发起端信息（系统 · 浏览器、IP、发起时间，UA 只做粗粒度归纳，
  不显示原始 UA），用户点「确认登录」或「拒绝本次登录」。
- 桌面端 `qr/start` 设置 HttpOnly 绑定 cookie；`qr/poll` 必须同浏览器匹配，
  否则 403 `qr_state_mismatch`（防止他人轮询领取会话）。
- 手机**一打开链接就上报** `qr/scan`（公开接口，未登录跳去登录时也会上报），
  PC 轮询（1.2 秒/次）随即切换为“已扫码，请在手机上确认登录”状态页；
  两种二维码面板都显示 5 分钟倒计时，本地倒计时与 Redis TTL 同时失效。
- 路由级代码分割：手机端只需下载核心运行时 + 3.6KB 的确认页 chunk，
  不再拖整个 237KB 单包；静态响应由传输层 brotli/gzip 压缩（48KB 核心包
  实际传输约 16KB）。
- 状态机：`pending → scanned（手机首次打开）→ confirmed / denied`；
  确认/拒绝用 `setnx` 一次性裁决，重复确认幂等，改判返回 409。
- 领取：`qr/poll` 对 confirmed 做一次性 claim（`qr_login_claim` setnx），随后
  用**桌面端**的 UA/IP 创建会话并下发 cookie；若该浏览器已有对应账号的可信
  设备 cookie 则一并绑定 device_hash。此后 `qr/<id>` 记录删除，重复轮询为
  `expired`。
- **领取也要过二次验证闸**（与密码登录同一判定，仅对开启登录保护的账号生效，见 1.4）：桌面端属于新设备 /
  账号长期未登录时，`qr/poll` 返回 `step_up_required` + `step_up_token`，前端
  跳转二次验证页，验证通过才真正建会话。此类的挑战**不提供 `device` 方式**
  （刚在手机上批准的那台设备不能自己给自己二次批准），只认邮箱/人脸；
  账号确实没有任何可用第二因素时，与密码登录一致退回直接放行。
- 限流：start 10 次/分钟/IP，确认+拒绝 20 次/分钟/用户。

**集市扫码**

- 二维码内容是集市 OAuth 授权页；手机需已登录集市账号。
- 桌面端 `qr/start` 设置 HttpOnly state cookie；`qr/poll` 必须同浏览器 cookie 匹配。
- 回调一次性 claim（`sse_qr_claim` setnx），重复扫码不会覆盖第一次批准。
- 手机回调**只记录授权**（`sse_qr:<state>` 存 `user_id`），不再在手机端建会话、
  不再自动信任手机浏览器；桌面端 `qr/poll` 一次性 claim（`sse_qr_poll` setnx）后
  用**桌面端自己的** UA/IP/设备 cookie 走与密码登录相同的二次验证闸（1.4）再建
  会话——此前“手机扫码 = 免 2FA 且桌面会话记手机 IP”的行为已废弃。

### 1.4 设备信任与二次验证（重点）

**登录保护是可选功能**：`users.step_up_enabled` **默认关闭（0）**，用户在
个人中心 → 登录保护自行开启；关闭时仪表盘会展示提示条引导开启。带管理员
权限创建的账号默认开。以下判定**只对已开启登录保护的账号生效**——关闭时
密码正确即直接建会话（邮箱验证码登录等原有路径本就不额外要求二次验证）。
开启后，密码正确但属于以下情况时**不直接创建会话、不下发 `unisso_session`**：

1. **当前设备是新设备**：浏览器没有 `unisso_device` cookie，该 cookie 不属于
   当前账号，**或当前来源 IP 不在这台设备记录过的地址名单里**；
2. **长期未登录**：`last_login_at` 超过 `security.step_up_days`（默认 7 天）。

设备判定 = 设备 cookie **且** IP：`trusted_devices` 除了 `token_hash` 还维护
`ip`（最近一次认证地址）与 `known_ips`（最近 8 个地址的 JSON 名单）。可信设备
出现在没见过的 IP（换网络、校园网 NAT 漂移、cookie 被带到别处）会被当作新设备
再做一次二次验证；验证通过（或勾选信任）后当前 IP 记入名单。登录页所有入口
（密码 / 手机扫码领取 / 集市扫码领取）共用这一判定。

来源 IP 的取法：只有当**直连对端**落在 `server.trusted_proxy_cidrs`
（默认回环、`10.42.0.0/16`、中继所在的 `172.18.0.0/16`）内时才采信
`X-Forwarded-For` / `X-Real-IP`，并按从右往左跳过可信段取第一个真实客户端
地址；公网入口经中继转发时记录的是用户真实公网 IP，不再是中继地址。

服务端返回：

```json
{
  "ok": true,
  "step_up_required": true,
  "step_up_token": "...",
  "methods": ["email", "face"],
  "email_hint": "t***@example.com",
  "trust_days": 180,
  "next": "/",
  "expires_in": 600
}
```

- `methods` 只包含可用方式：邮箱可用则 `email`；已登记人脸且人脸服务启用则 `face`。
- 同时下发 10 分钟 HttpOnly `unisso_stepup` cookie；token 必须与 cookie 一致，
  token 也会返回给前端用于 body。
- 邮箱验证码：5 次发送/10 分钟、60 秒重发间隔、5 次错误后挑战作废。
- 人脸验证：见第 5 节，必须动态活体 + 与当前账号已登记的人脸匹配。
- **验证成功前不更新 `last_login_at`**，也不创建 session。
- 验证成功后：
  - 创建 session；
  - 更新 `last_login_at`；
  - **只有用户勾选“在这台设备上信任 X 天”**才写入 `trusted_devices` 并设置
    `unisso_device` cookie（默认 `security.trust_device_days: 180`）；
  - 未勾选时不建立长期信任，下次登录仍会要求二次验证。
- 勾选长期信任时页面会明确提示风险：只应在个人设备开启；开启后能使用该设备的人
  可直接进入账号；可随时在“个人中心 → 登录设备”取消。
- 即使设备已信任，账号超过 `security.step_up_days`（默认 7 天）未登录仍会要求二次验证。
- 老客户端未提交 `trust_device` 字段时按“信任”处理，避免升级后行为突变。

> 新设备第一次登录会要求二次验证；勾选长期信任后该设备在有效期内被记住。
> 清除浏览器 cookie / 无痕窗口会被视为新设备；已信任设备出现在完全没见过的
> IP 同样按新设备处理（多一次二次验证，通过后名单自动更新）。
> 部署 IP 判定后，旧的已信任设备首次登录会要求一次二次验证（旧记录只存过
> 中继地址），属预期行为。

### 1.5 已登录设备确认（推送式二次验证）

当用户还有其他有效登录设备时，二次验证方式会多一个 `device`：

- 新设备密码通过后，服务端自动向已登录设备推送一条“登录确认”请求；请求 10 分钟有效。
- 已登录设备上的全局组件每 3.5 秒轮询一次，有待确认请求时**自动弹出**确认框，
  显示请求设备/IP/时间，不需要用户自己去找入口。
- 已登录设备可以“允许登录”或“拒绝”；批准后，新设备自动收到 status=approved，
  服务端创建 session 并设置 cookie，新设备自动进入下一步。
- 请求是一次性的；批准/拒绝/取消/过期后不再弹出。多个请求按顺序显示。
- 新设备的“在这台设备上信任 X 天”选择会随确认结果一起生效。
- 没有任何其他有效会话时不提供 device 方式，只能邮箱/人脸。
- **扫码领取产生的二次验证挑战不提供 device 方式**（挑战 JSON 里的
  `device_allowed=false`，`step-up/methods`、`device/start` 都按它执行）：
  批准扫码的手机本身就是“另一台已登录设备”，不能再用它自我确认。
- 全局轮询只在已登录页面运行；二次验证页本身不会收到弹窗。

登录保护**默认关闭**（`step_up_enabled=0`）：列默认值为 0，新注册账号即关；
2026-11 升级时对存量非管理员账号做一次性 opt-out（`migration_flags` 里的
`step_up_default_off_2026_11` 标记保证只执行一次，之后不会覆盖用户主动开启）；
管理员账号建号时默认开。用户可随时在个人中心 → 登录保护开启/关闭；关闭后
新设备/长期未登录只凭密码即可登录，页面与仪表盘会明确提示风险并引导开启。

### 1.6 邮箱验证码登录与密码重置

- 登录页支持「邮箱验证码登录」：输入已注册邮箱，发送 6 位验证码，验证后直接创建
  session，不需要密码。验证码 10 分钟有效，60 秒重发间隔，5 次错误作废。
- 登录页提供「忘记密码？」：邮箱验证码通过后设置新密码。密码需满足强度要求。
  重置成功会删除该用户全部会话、设备信任、access/refresh token，并取消待处理的
  登录确认请求，防止旧会话继续使用。
- 两个接口对不存在的邮箱/不可投递邮箱返回同样的成功响应，避免枚举账号；发送和
  尝试都有按邮箱/IP 的限流。
- 邮箱验证码登录本身就是邮箱持有证明，不再额外要求二次验证；它不会自动写入
  设备长期信任。但对**已经**可信的设备，成功登录会把当前 IP 刷新进它的
  `known_ips` 名单（`noteDeviceIp`），避免校园网换 IP 后白白多一次二次验证。
- 如果账号没有本地密码（例如纯 SSE 账号），仍可先用邮箱验证码登录，再通过重置
  设置本地密码。

### 1.7 邮箱注册

- SMTP 验证码，15 分钟有效；发送/尝试限流。
- 支持邀约码（SSE CDKey + 集市 `ValiCode`）两步注册。
- 待验证记录只存 bcrypt 密码哈希和验证码哈希。

---

## 2. 账号关联（对称、多对多）

- **不再有主账号/附属账号**。
- A 关联 B 后，B 立刻看到 A；任一方可以解除。
- 一个账号可以关联多个账号；A–B、B–C 不会自动让 A 看到 C（每条边独立）。
- 绑定需要验证对方账号：对方密码或对方邮箱验证码，二选一。
- 绑定关系存 `account_peer_links`，两个 UUID 排序后唯一，一对账号只有一条边。
- OAuth 同意页：当前账号 + 直接关联账号，可选择“以哪个账号授权”；
  一次授权只对应一个账号，权限不合并。

---

## 3. OAuth2 / OIDC

- Authorization Code + PKCE S256。
- **每次授权都必须显示同意页并点击“允许”**；即使存在历史 consent，也不会静默
  重新签发 code（防止用户看到一连串跳转却没有确认过程）。
- 端点：
  - `GET /authorize`（SPA 同意页）
  - `GET /api/oauth/authorize/context`
  - `POST /api/oauth/authorize`
  - `POST /api/oauth/token`
  - `POST /api/oauth/introspect`（需要 client 认证）
  - `GET /api/oauth/userinfo`
  - `GET /.well-known/openid-configuration`
  - `GET /.well-known/jwks.json`
- access token / id_token 为 RS256；refresh token 轮换，旧 token 撤销。
- 应用注册允许平台管理员、组织管理员或组织开发者；回调地址只允许 HTTPS 或 localhost；scope 白名单。
- 更新回调地址/scope 会撤销该应用的 consent 与 token。

### 3.1 claims

| claim | 含义 |
|---|---|
| `sub` | 当前 UniSSO 账号的 OAuth id |
| `sso_sub` | 同上（显式别名） |
| `sse_sub` | 该账号绑定的集市 OAuth id；没有集市身份时不返回；**可能和 `sso_sub` 不同** |
| `preferred_username` / `name` / `picture` / `student_id` | 按 scope 返回 |
| `email` / `email_verified` | 按 `email` scope |
| `roles` | 全局角色（按 `roles` scope） |
| `internal` | 是否集市认证的院内身份 |

- **没有 `groups` / `organizations` claim**；应用的用户组/权限自己管理。
- token 响应、userinfo、id_token 都返回 `sso_sub` / `sse_sub`。
- 账号选择器返回的 `accounts[].is_current` 标记打开同意页的账号；其余是直接关联账号。

### 3.2 公开文档与开放平台（免登录）

第三方开发者拿到 UniSSO 地址后无需账号即可查阅接入方式：

- 页面（SPA 路由，站点导航与页脚均有入口）：
  - `/docs` 开发者文档：在线渲染外部平台（跳转授权）接入说明，可直接下载 `SKILL.md`。
  - `/developer` 开放平台：三类接入方式、端点速查、scope 白名单与含义、错误契约、
    PKCE 快速开始；「注册接入」已登录跳个人中心、未登录跳登录页。
- 公开只读接口（**不鉴权**，无用户数据、无副作用）：
  - `GET /api/public/docs` — 发布元数据：`issuer`、`endpoints`
    （authorize/token/userinfo/introspect/discovery/jwks）、`scopes` 与 `scope_catalog`、
    `grants`、`code_challenge_methods`、`docs` 列表。
  - `GET /api/public/docs/skill?kind=external` — 外部接入文档 markdown（inline 返回，
    供页面直接渲染）；`kind` 只接受 `external`（缺省同），其它值一律 `400 invalid_request`。
- **只公开外部（第三方跳转授权）那一篇**；内部/无感登录文档仍只由
  `GET /api/apps/integration-skill`（应用管理者）下载，公开面不涉及内部接入细节。
- 文档文件**每请求热读**，直接改
  `.claude/skills/unisso-integration-external/SKILL.md` 即时生效（无需重启/重构建）。
- scope 白名单、端点均取后端同一来源（`scopes::whitelist()` / `AppConfig`），
  不会与 `/api/apps` 校验、OIDC discovery 漂移。

---

## 4. 人脸服务接入

### 4.1 密钥与配置

- key 文件：`backend/secrets/face-api.key`（600，gitignored，不写日志/前端）。
- 配置：`backend/config.yml` 的 `face.*`；环境变量优先。
- 当前 key：`read,write`，30 rps，20,000 persons，365 天。
- 人脸服务**不保存原图**：只在内存中处理，保留 512 维特征；图片接口固定 404；
  删除 person 时特征与 MinIO 备份异步删除。

### 4.2 人脸只做二次验证

- **没有刷脸登录接口**，人脸识别不能直接创建 session。
- 人脸只出现在两个地方：
  1. 新设备/长期未登录的密码登录二次验证；
  2. 个人中心的人脸登记/解绑（登记后才能在二次验证中选用人脸）。

### 4.3 人脸登记流程

登记必须已登录，且勾选同意说明。

1. **probe（识别是否已登记）**
   - 后端创建动态活体会话，返回随机动作（转头/抬头/低头）。
   - 前端引导动作，连续采集 8 帧 JPEG（质量 0.9，至少 6 帧可用），发送后端。
   - 后端调 `POST /face/v1/recognize/live`，服务端同时校验：
     - 动作幅度；
     - 帧间同一人；
     - 每帧被动 PAD；
     - 通过后用最佳帧做 1:N。
2. **命中已有 person**
   - 前端问“这是你本人吗？”
   - **是**：必须验证**当前账号邮箱**（验证码），通过后才把该 person 关联到当前账号；
     不重复录入人脸。
   - **否**：重新进入第 3 步，新录一张脸。
   - 如果该 person 已关联当前账号：直接提示已绑定，不再发验证码。
3. **未命中 / 选择“不是”**
   - 重新创建活体会话，按随机动作再采集一次。
   - 后端调 `POST /face/v1/persons/live` 新建 person，返回 `person_id`。
   - 写 `face_persons` + `user_faces`，完成登记。

数据模型：

- `face_persons`：一个物理人 = 一个 Face person，`external_id=unisso_face_<random>`。
- `user_faces`：账号 ↔ person 多对多；一个账号可多张脸，一张脸可被多个账号关联。
- `face_audits`：事件、person id、分数、IP、时间；**不含图片/特征**。

### 4.4 二次验证中的人脸

- 仅当当前账号已登记人脸且 Face 服务可用时，`methods` 才包含 `face`；
  二次验证页加载时会重新查询一次，已解绑人脸后不会再显示人脸入口。
- 人脸验证在独立弹窗内完成，不挤在邮箱表单旁边；没有邮箱时人脸按钮为主操作，
  有邮箱时作为“或”的备选。
- 流程：密码已通过 -> 创建活体会话 -> 采集 -> `recognize/live`（登录阈值 0.6）
  -> 命中 person 必须已关联当前账号 -> 成功创建 session + 信任设备。
- 如果人脸不匹配、活体失败或服务不可用，仍可使用邮箱验证码；不自动降级登录。
- 管理员/超级管理员没有特殊绕过；仍需先有密码，再通过二次验证。

### 4.5 动态活体与前端采集

- 动作由服务端随机下发：`turn_left` / `turn_right` / `look_up` / `look_down`；
  前端不能自选动作。
- 采集要求：HTTPS + `getUserMedia`，前置摄像头，**不镜像**；8 帧 JPEG（质量 0.9），
  分辨率 640×480，间隔 400 ms（约 3.2 秒），覆盖整个转头/抬头过程。
  前端过滤明显过暗/过曝/模糊帧后，保留动作起点、终点和中间最清晰的 4 帧，
  **按时间顺序发送 6 帧**；既覆盖整个动作，又避免把过渡期的模糊帧发给上游造成
  `identity_mismatch`（2 个低质量帧即可触发 `identity_outliers`）。
  后端 multipart 请求体上限已从 8 MiB 调整为 24 MiB；Upstream 单帧上限 10 MB。
- 摄像头权限：`FaceCapture` 先显示“允许使用摄像头”按钮，点击后触发浏览器原生
  权限弹窗；拒绝则不能采集，但可以回退邮箱验证码。页面 Permissions-Policy 已允许
  `camera=(self)`。微信内置浏览器可能限制连续采集，会提示改用系统浏览器。
- 前端 `FaceCapture` 组件：无倒计时，点击即开始；按动作提示采集、显示进度、
  失败留在弹窗内可重取。提示要求动作幅度大（转头约 45°、抬头/低头角度明显）。
- 上游失败响应（含 `motion_range`、`motion_required`、`identity_median`、
  `pad_min`、可用帧数、`request_id`）写入 `/home/cloud/deploy/logs/backend.log`，
  管理端日志页可直接查看，便于按时间定位。
- 服务端限制：会话 120 秒、一次性；失败必须重新取会话。
- 阈值：登录 0.6，登记识别 0.5；多张脸直接拒绝，不自动取最大脸。

### 4.6 错误码

| code | 含义 | 前端提示 |
|---|---|---|
| `no_face` | 没检测到/太小/太糊 | 调整光线、正对镜头 |
| `multiple_faces` | 多张脸 | 画面只保留一个人 |
| `challenge_not_completed` | 未完成动作/静态照片 | 按提示转头/抬头 |
| `spoof_suspected` | 翻拍/屏幕 | 不要使用照片/屏幕 |
| `identity_mismatch` | 帧间换人或与账号不匹配 | 重新采集或改用邮箱 |
| `too_few_frames` | 少于 3 帧（Web 客户端要求至少 6 帧可用） | 放慢动作、重新采集 |
| `session_expired` / `missing_session` | 会话过期/缺失 | 重新开始 |
| `conflict` | 会话已使用 | 重新开始 |
| `rate_limited` / `overloaded` | 上游限流/繁忙 | 退避或改用邮箱 |
| `face_service_unavailable` | 服务未配置/不可达 | 使用邮箱验证码 |

### 4.7 隐私与合规

- 不保存人脸照片；Upstream 只保留 512 维特征，删除 person 时特征和备份一起删除。
- 本地只保存 person 关联和审计（不含图像/特征）。
- 首次登记需阅读《人脸识别服务协议》并勾选同意；协议在弹窗中，向下滑动阅读 5 秒
  后自动勾选。协议含数据控制者（SSE Market 统一身份认证运营团队，地址中国广东省
  广州市）、处理者、保存期限、删除方式、用户权利和联系方式。
- 数据控制者：SSE Market 统一身份认证运营团队；地址：中国广东省广州市；
  隐私负责人/联系方式：`ssemarket@163.com`；正式隐私政策：
  `https://sso.ssemarket.cn/privacy`。
- 处理者：本平台 SSE-Strata 人脸服务；只保留 512 维特征，不保存照片。
- 人脸特征保留至解绑或注销；审计记录保留 180 天；照片不保存。
- 解绑：删除 `user_faces`；若该 person 无其他账号关联，调用 Face `DELETE` 删除特征。
- 账号注销：管理员删除用户时同时删除其账号关联、可信设备与人脸审计；人脸 person
  的清理按“无其他关联才删除”规则处理。

### 4.8 已知风险（必须知悉）

1. `buffalo_l` 权重为非商业研究用途；商业生产前需商业授权或替换同维度模型
   （替换后需清库重建）。
2. PAD 阈值 0.3 尚未用真实摄像头样本大规模标定；建议先小范围灰度。
3. 动态动作能挡静态照片/简单重放，但无法绝对防御实时换脸/虚拟摄像头；
   高价值账号建议叠加 TOTP/硬件密钥。
4. Face 服务 GPU 资源有限；繁忙时可能落 CPU，延迟升高；失败应回退邮箱验证码。

---

## 4.9 组织与应用管理（仅用于应用归属）

组织和业务用户组是两件事：组织不进入 OAuth claims，不是应用内的用户组/权限系统。

### 角色

- **超级管理员 / 平台管理员**：创建组织；管理所有组织的成员和应用；全局用户/日志。
- **组织管理员**：查看本组织成员；添加成员；设置其他成员为管理员/开发者/成员；
  管理本组织应用；不能管理其他组织。
- **组织开发者**：注册/编辑/删除本组织应用；不能管理组织成员。
- **组织成员**：没有组织管理或应用管理权限，只能在授权时选择自己的账号。

### 创建组织 / 设置组织管理员

1. 超级管理员登录 → 「组织」→「新建组织」，创建者自动成为该组织管理员。
2. 进入组织 → 输入成员邮箱、选择「管理员」→ 添加；
   或把已有成员的职责下拉改为「管理员」。
3. 组织管理员可以继续把其他成员设为「管理员」或「开发者」。
4. 组织至少保留一名管理员；最后一名管理员不能被降级或移除；也不能移除自己。

### 应用归属

- 应用注册时选择所属组织；平台管理员可选任意组织，组织管理员/开发者只能选自己管理的组织。
- 应用列表：
  - 超级管理员/平台管理员：所有应用，并显示每个应用的**发起人**；
  - 组织管理员/开发者：本组织应用 + 自己拥有的应用；
  - 组织成员：只显示自己拥有的应用（通常为空或只有历史测试应用）。
- 组织管理员/开发者可以管理本组织应用的详情、回调、scope、secret 和删除。

### 应用注册（对外：自助注册即生效）

- **对外授权应用**（`kind=external`）由**学院成员**（`is_internal=1`）在「个人中心 → 我的接入应用」
  自助注册，**提交即生效**（无审核），当场发放 `client_id` / `client_secret`（**secret 只显示一次**）；
  每人应用总数上限 20（超出返回 `429 too_many_applications`）。
- 发起人（`owner_id`）即所有者，可自助编辑、重置 secret、删除**自己的**应用；
  非本人访问返回 `404 not_found`（不泄露存在性）。
- 超级管理员保留事后管理：`POST /api/apps/{id}/review` 可 `suspend`（暂停，token 接口直接拒）/ `reject` / `approve`（恢复），
  并可删除任意应用。
- **内接应用**（`kind=internal`）仍由平台管理员用 `POST /api/apps` 注册，不对外开放。

> 默认组织 `SSE` 已存在；`sse_561` 超级管理员是其管理员，普通测试账号默认只是成员。
> 把测试账号提升为组织管理员后，刷新页面才能看到「组织」导航和本组织全部应用。

## 5. 个人中心 / 管理功能

- 超级管理员：在「用户」页选择用户 →「设为超管」。超级管理员不能取消自己，系统
  始终保留至少一名可用的超级管理员；取消超管不会自动取消原有的平台管理员身份。
- 组织管理员：超级管理员在「组织」页新建组织后，输入已注册成员邮箱、角色选「管理员」
  添加；组织管理员登录后可在同一页把其他成员设为管理员/开发者。组织至少保留一名
  管理员，组织角色不进入 OAuth claims。
- 个人中心：基本资料、头像、登录设备、对称账号关联、人脸二次验证登记/解绑、外部身份、
  我的授权、**我的接入应用**。显示名和学号可自助编辑；邮箱、角色、OAuth 身份标识为只读。
- 登录设备：列出当前账号所有有效会话（设备/浏览器、IP、登录时间、最近活动），
  每一条都支持**单独退出**，也可一键退出其他所有设备。会话 ID 不直接返回，
  接口只返回 HMAC 后的设备标识；撤销后该设备下一次请求即 401。
- 退出最后一个与该设备关联的会话时，同时删除 `trusted_devices` 中该设备的信任，
  下次登录必须重新二次验证；如果同一设备还有其他有效会话，则保留信任。
- 当前设备可单独开启/取消长期信任；开启前必须确认风险提示。取消后本设备下次登录
  需要重新验证。
- 应用管理：平台管理员、组织管理员、组织开发者可注册/编辑/重置 secret/上传图标；不提供“创建业务用户组”能力。
- 我的接入应用（个人中心）：学院成员可自助注册**对外授权应用**（提交即生效，发放一次性凭据），
  并可对自己注册的应用编辑 / 重置 Secret / 删除；普通成员无法创建应用。
- 应用管理页（超级管理员）：列出全部应用并显示**发起人**（`owner_name` / `owner_email`），
  对外应用可事后暂停（`suspend`）/ 驳回 / 删除。
- 应用管理页提供「下载接入 Skill」：平台管理员、组织管理员/开发者、应用所有者可下载
  `unisso-integration-SKILL.md`（内容来自 `.claude/skills/unisso-integration/SKILL.md`，
  可用 `UNISSO_INTEGRATION_SKILL` 覆盖路径）；普通成员返回 403。
- 用户管理/日志：仅超级管理员；日志在 `/home/cloud/deploy/logs/`，Web 管理页
  `/admin/logs` 可查看后端/前端日志。
- 头像：本地 `data/uploads`；大小/类型校验，UUID 文件名。
- 日志：异步文件日志，8 MB 轮转；客户端错误转发有限流和脱敏。

---

## 6. 主要 API

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/api/auth/login` | 密码/SSE 密码；可能返回 `step_up_required` |
| POST | `/api/auth/email-code/start` | 发送邮箱登录验证码 |
| POST | `/api/auth/email-code/verify` | 邮箱验证码登录 |
| POST | `/api/auth/password-reset/start` | 发送密码重置验证码 |
| POST | `/api/auth/password-reset/confirm` | 验证码 + 新密码，重置并撤销旧会话 |
| POST | `/api/auth/step-up/methods` | 重新读取当前账号可用的二次验证方式（人脸解绑后立即隐藏） |
| POST | `/api/auth/step-up/email` | 发送二次验证邮箱码 |
| POST | `/api/auth/step-up/verify` | 邮箱码验证并建会话 |
| POST | `/api/auth/step-up/face/session` | 创建人脸二次验证活体会话 |
| POST | `/api/auth/step-up/face/verify` | 人脸活体 + 当前账号匹配 |
| POST | `/api/auth/step-up/device/start` | 新设备发起“已登录设备确认” |
| POST | `/api/auth/step-up/device/status` | 新设备轮询确认状态；批准后建 session |
| GET | `/api/auth/step-up/device/pending` | 已登录设备拉取待确认请求 |
| POST | `/api/auth/step-up/device/{id}/decide` | 已登录设备允许/拒绝 |
| POST | `/api/auth/step-up/cancel` | 取消二次验证 |
| PUT | `/api/me/profile` | 修改当前账号显示名/学号；可切换 `step_up_enabled` |
| GET | `/api/apps/integration-skill` | 下载 SSO 应用接入 Skill（仅应用管理者；`?kind=external` 取外部文档） |
| GET | `/api/public/docs` | 公开：开放平台端点/scope 元数据（免登录） |
| GET | `/api/public/docs/skill` | 公开：外部平台接入文档 markdown（免登录，只接受 `kind=external`） |
| GET | `/api/account/sessions` | 当前账号的有效登录设备 |
| DELETE | `/api/account/sessions/{id}` | 单独退出指定设备（不能退出当前设备；同时移除设备信任） |
| POST | `/api/account/sessions/revoke-others` | 退出其他所有设备并移除对应信任 |
| GET | `/api/account/device-trust` | 当前设备的长期信任状态 |
| POST | `/api/account/device-trust` | 开启/取消当前设备的长期信任 |
| GET/POST | `/api/account/links` | 对称账号关联列表/新建 |
| POST | `/api/account/links/send-code` | 给对方账号发关联验证码 |
| DELETE | `/api/account/links/{id}` | 解除关联 |
| GET | `/api/account/faces/status` | 人脸服务状态、已登记数量 |
| GET | `/api/account/faces` | 已登记人脸列表（不含特征/图片） |
| POST | `/api/account/faces/session` | 登记 probe/enroll 活体会话 |
| POST | `/api/account/faces/probe` | 识别是否已登记（动态活体 + 1:N） |
| POST | `/api/account/faces/send-code` | 命中已有脸时，验证当前账号邮箱 |
| POST | `/api/account/faces/confirm` | 邮箱验证后关联已有 person |
| POST | `/api/account/faces/enroll` | 动态活体新建 person 并关联 |
| DELETE | `/api/account/faces/{id}` | 解绑；最后一个关联时删除 person |
| GET | `/api/oauth/authorize/context` | 同意页上下文/账号选择 |
| POST | `/api/oauth/authorize` | 同意/拒绝，选择 `as_user_id` |
| POST | `/api/oauth/token` | 授权码/refresh |
| GET | `/api/oauth/userinfo` | 身份 claims |
| POST | `/api/oauth/introspect` | 需 client 认证 |

所有会改变状态的接口都需要 CSRF token（`X-CSRF-Token`）。

---

## 7. 配置

`backend/config.yml`（环境变量覆盖）：

```yaml
server:
  # 仅这些网段的直连对端可以供给 X-Forwarded-For / X-Real-IP
  trusted_proxy_cidrs: [127.0.0.0/8, ::1, 10.42.0.0/16, 172.18.0.0/16]
security:
  step_up_days: 7
  trust_device_days: 180
face:
  base_url: https://ai.ssemarket.cn/face
  api_key_file: secrets/face-api.key
  login_threshold: 0.6
  bind_threshold: 0.5
  timeout_seconds: 30
```

常用环境变量：

| 变量 | 说明 |
|---|---|
| `UNISSO_STEP_UP_DAYS` | 长期未登录阈值；0 关闭长期阈值（新设备仍要求） |
| `UNISSO_TRUST_DEVICE_DAYS` | 用户勾选后设备长期信任天数（默认 180） |
| `UNISSO_TRUSTED_PROXY_CIDRS` | 逗号分隔；可采信转发头的直连网段（覆盖配置文件） |
| `UNISSO_FACE_API_KEY_FILE` | 人脸 key 文件路径 |
| `UNISSO_FACE_BASE_URL` | Face 服务地址 |
| `UNISSO_FACE_LOGIN_THRESHOLD` | 登录/二次验证阈值（代码默认 0.6） |
| `UNISSO_FACE_BIND_THRESHOLD` | 登记识别阈值（代码默认 0.5） |
| `UNISSO_FACE_TIMEOUT_SECONDS` | Face 请求超时 |

环境变量会覆盖配置文件；修改 `config.yml` / 环境变量后重启生效。

---

## 8. 联调与验收

需要准备（尚未提供）：

- 2 个测试账号；
- 2 张已同意的真人脸，或批准的合成脸；
- 场景：
  - A：账号 1、账号 2 绑定同一张脸，验证多对多；
  - B：账号 1 绑定两张脸，验证都能通过二次验证；
  - C：未登记脸的人脸 → 不能作为二次验证；
  - D：手机照片对着摄像头 → `challenge_not_completed` / `spoof_suspected`；
  - E：新设备首次密码登录 → 二次验证；验证后同设备再次登录 → 不重复；
  - F：7 天未登录 → 即使设备已信任也要二次验证；
  - G：人脸登记时命中已有 person 且选择“是” → 必须邮箱验证码；
  - H：选择“不是” → 重新采集并新建 person。

当前已验证：

- 新设备密码登录返回 `step_up_required`，且 `/api/auth/me` 401；
- 邮箱二次验证成功后建会话、写 `trusted_devices`、设 device cookie；
- 同设备再次登录不再二次验证；长期未登录即使设备受信也会二次验证；
- Face 服务 key 生效，`/face/v1/me` 200；
- 活体会话、6–8 帧 multipart、上游 `no_face` 错误链路；
- 用本地 mock 人脸服务跑通了：登记 probe 命中 → 邮箱验证码确认 → 关联；
  新 person 登记；人脸二次验证成功建会话；删除人脸；
- 邮箱发送与测试码校验、账号关联多对多。

待真实人脸样本验收：真人活体动作通过/拒绝、登记命中已有脸的邮箱确认、
人脸二次验证成功（代码链路已用 mock 跑通，真实上游识别成功形状仍需样本确认）。

---

## 9. 运维

- 查看健康：`GET /api/health`；Face 健康：`GET https://ai.ssemarket.cn/face/health`。
- 查看日志：`/home/cloud/deploy/logs/backend.log`、`frontend.log`，或 `/admin/logs`。
- key 轮换：管理员 revoke 旧 key -> 新 key 写入 `secrets/face-api.key` -> 重启。
  轮换后旧 person 仍在该 key 命名空间下；若换了新 key，底库不共享，需要重新登记
  （或让服务管理员迁移）。
- 删除用户：`adminUsersDelete` 会清理账号关联；人脸 person 按“无其他关联才删除”
  规则处理。
- 人脸服务不可用：二次验证自动只显示邮箱；不会放行纯人脸。
- 平台重部署覆盖 `deploy.sh`：执行 `repoint.sh` 切回 v2。
- 回滚 Python：`cp deploy.sh.python-backup deploy.sh` 后重启 supervisor。

---

## 10. 安全边界（摘要）

- 人脸不是登录因子；没有刷脸登录入口。
- 新设备/长期未登录必须二次验证；密码通过也不发 session。
- 人脸二次验证必须动态活体 + 当前账号已登记 person 的 1:N 匹配；两者都过才建会话。
- 登记命中已有 person 时，必须验证当前账号邮箱后才关联。
- 人脸照片不保存；本地只存关联和审计。
- 账号关联对称多对多，不再有主/附属；OAuth 一次授权只选一个账号，权限不合并。
- 公开面仅 `/docs`、`/developer` 两个页面与 `/api/public/docs*` 两条只读接口：
  不鉴权但也不含任何用户数据、不接受写操作，发布的正是给第三方的外部接入文档；
  内部接入文档仍需应用管理者身份下载。
- 详细安全修复与残余风险见 `SECURITY.md`。
