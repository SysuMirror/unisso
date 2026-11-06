# UniSSO v2 — Vue 3 + C++

统一身份认证（SSO）：只负责**身份认证与 OAuth2/OIDC 身份断言**，不做业务权限、
用户组或应用内授权。应用自己的角色、用户组、资源权限由应用管理。

- 前端：Vue 3 + Vite（`frontend/`），构建到 `backend/web/`
- 后端：C++20，HTTP 传输使用发行版 `cpp-httplib`（原先 vendored 框架的传输层
  在并发下会卡死，已替换；业务/路由/JSON/DB/Redis 代码保留）
- 存储：MySQL 8 + Redis ACL + 本地文件（头像/图标）
- 协议：OAuth2 Authorization Code + PKCE(S256)、OIDC discovery/JWKS、RS256

## 公网地址

- 用户入口 / 管理页：`https://sso.ssemarket.cn`
- OIDC issuer（写入 token 的 `iss`）：`https://sso.ssemarket.cn`
- Discovery：`https://sso.ssemarket.cn/.well-known/openid-configuration`
- 人脸服务：`https://ai.ssemarket.cn/face`
- 隐私政策：`https://sso.ssemarket.cn/privacy`（联系人：`ssemarket@163.com`）
- 开发者文档：`https://sso.ssemarket.cn/docs`（免登录，外部平台跳转授权接入说明）
- 开放平台：`https://sso.ssemarket.cn/developer`（免登录，端点/scope 速查与申请入口）

## 目录

```
unisso-v2/
├── backend/
│   ├── config.yml          # 非敏感默认值
│   ├── config.local.yml    # 本机密钥（gitignored，权限 600）
│   ├── secrets/            # RS256 私钥/公钥（gitignored，权限 600）
│   ├── data/uploads/       # 头像与应用图标
│   ├── web/                # Vite 构建产物（gitignored）
│   ├── include/ src/       # HTTP 框架 + src/app 业务代码
│   └── build/unisso_v2
├── frontend/
├── deploy.sh               # 前台入口（supervisor 使用）
└── repoint.sh              # 平台重部署后切回 v2
```

## 配置优先级

**环境变量 > config.local.yml > config.yml > 内置默认值**

- MySQL / Redis 使用平台注入的 `MYSQL_*` / `REDIS_*`。
- Redis ACL 用户名/键前缀来自 `REDIS_USER` / `REDIS_PREFIX`，所有键写入
  `$REDIS_PREFIX:` 下。
- SMTP、SSE OAuth 密钥放 `config.local.yml` 或环境变量，**不要**写进 `config.yml`。
- OAuth 应用的回调地址只能是 `https://…`，或本地开发用的
  `http://localhost…` / `http://127.0.0.1…`。

## 构建与运行

```bash
cd frontend && npm install && npm run build   # 输出到 ../backend/web
cd ../backend && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j2
cd .. && ./deploy.sh
```

系统依赖（容器重建后由 `deploy.sh` 自动安装）：
`cmake libssl-dev default-libmysqlclient-dev libhiredis-dev zlib1g-dev
libcurl4-openssl-dev libcrypt-dev libcpp-httplib-dev`。

## 功能边界

**SSO 负责**

- 邮箱注册/验证码、密码登录、邮箱验证码登录、忘记密码重置、集市（SSE）账号登录
- 扫码登录分两种：**手机扫码**（二维码跳转到 UniSSO 手机端确认页，手机已登录
  即可一键批准 PC 登录，未登录则先登录再确认）、**集市扫码**（集市 OAuth 授权）
- 浏览器会话、CSRF、登录限流、密码 bcrypt（`$2b$12$`）存储
- 二次验证：**新设备**或超过 `security.step_up_days`（默认 7 天）未登录的账号，
  密码通过后不直接发会话，必须再通过邮箱验证码或动态人脸；人脸只做二次验证，
  **没有刷脸登录入口**
- 账号关联：不分主次、可关联多个、双向显示；OAuth 同意时可选择本次授权使用的账号（权限不合并）
- 组织（仅用于应用管理）：超级管理员创建组织；组织管理员可设置本组织其他管理员/开发者；
  开发者可注册并管理本组织应用；组织角色不进入 OAuth claims，不是业务用户组
- OAuth2/OIDC：授权码 + PKCE、token、refresh 轮换、userinfo、introspect、JWKS
- 我的授权、应用注册（对外应用：**学院成员自助注册、提交即生效**；内接应用：平台管理员）、
  超级管理员的用户与日志管理
- 应用管理页可下载接入 Skill（`unisso-integration-SKILL.md`），仅应用管理者可见
- 登录设备管理：查看当前账号的有效会话、退出指定设备、退出其他所有设备
- 头像上传、日志写盘与 Web 查看、SSE 外部身份绑定

**SSO 不负责**

- 应用内角色、用户组、资源/菜单权限（应用自己存、自己判）
- `groups` / `organizations` scope 与 claim：已移除；OAuth 只返回身份属性
- 应用创建用户组：应用管理页没有该功能，也不再发布相关 scope

OAuth 返回的 claim：`sub`、`sso_sub`、`sse_sub`、`preferred_username`、`name`、
`picture`、`student_id`、`email`、`email_verified`、`roles`、`internal`。

- `sub` / `sso_sub`：当前 UniSSO 账号的 OAuth 用户 id（两者相同）。
- `sse_sub`：该账号绑定的集市 OAuth 用户 id；未绑定集市时不存在。
- 这两个 id **可能不同**，不要互相替代。应用用 `sso_sub` 做本地身份主键，
  需要对接集市时再用 `sse_sub`。
- token 响应、userinfo、id_token 都会同步返回这两个 id；`internal` 表示集市认证的院内身份。

## 新设备 / 长期未登录的二次验证

- 触发：当前浏览器没有可信设备 cookie，或 `last_login_at` 超过
  `security.step_up_days`（默认 `7`，环境变量 `UNISSO_STEP_UP_DAYS`）。
- 密码正确但设备不受信或 `last_login_at` 超过阈值时，服务端**不创建会话、不下发 session cookie**，
  返回 `step_up_required` + 一次性 token + `methods`。
- 邮箱验证码：5 次发送/10 分钟、60 秒重发间隔、5 次错误后作废，验证成功后
  才更新 `last_login_at` 并创建会话。
- 二次验证挑战与 HttpOnly cookie 绑定，token 过期 10 分钟；不能只凭 token 使用。
- 人脸只作为二次验证的备选方式，**没有刷脸登录入口**；已登记人脸的用户可选择
  人脸或邮箱验证码，人脸必须通过动态活体和当前账号匹配。未登记人脸的用户在
  个人中心会看到登记建议。
- 有已登录设备时，可自动把确认请求推送到已登录设备：已登录页面会自动弹出
  “允许登录/拒绝”确认框，新设备轮询到批准后自动建会话，不需要发邮箱。
- 用户可在个人中心 → 登录保护关闭二次验证；关闭后新设备/长期未登录只凭密码登录，
  页面会明确提示风险。默认开启。
- 二次验证成功后，用户可勾选“在这台设备上信任 X 天”（默认 180 天，
  `security.trust_device_days` / `UNISSO_TRUST_DEVICE_DAYS`）；只有勾选才写入
  `trusted_devices` 并设置设备 cookie，勾选时会明确显示风险。未勾选则下次登录
  仍需要二次验证。即使已信任，超过 `step_up_days` 未登录仍会要求验证。
- 个人中心“登录设备”可查看每条有效会话，单独退出任意设备；退出设备最后一台会话时
  同时删除该设备信任，下次登录必须重新二次验证。当前设备也可单独开启/取消长期信任。

## 权限模型

| 角色 | 能力 |
|---|---|
| 超级管理员（`is_super_admin` / 平台管理员） | 创建组织、全局用户/日志、所有组织与应用 |
| 组织管理员 | 管理本组织成员；可设置其他管理员/开发者；管理本组织应用 |
| 组织开发者 | 注册并管理本组织应用 |
| 学院成员（`is_internal`） | 自助注册**对外授权应用**（提交即生效，无需审核）；管理自己注册的应用（编辑 / 重置 secret / 删除） |
| 普通成员 | 个人资料、关联账号、外部身份、授权记录 |

组织只服务于 **应用归属和应用管理**：不进入 OAuth `groups`/`organizations` claim，
不是应用的业务用户组。普通成员不能创建应用；组织管理员/开发者只能管理自己所属组织。

### 应用接入：注册即生效

- **对外授权应用**：任何学院成员（`is_internal=1`）可在「个人中心」自助注册，**提交即生效**，
  当场发放 `client_id` / `client_secret`（secret 只显示一次）；每人应用总数上限 20。
- 发起人（所有者）可随时在个人中心编辑、重置 secret、删除自己的应用。
- 超级管理员在「应用管理」页可看到**全部应用及其发起人**，并可事后**暂停 / 驳回 / 删除**。
- **内接应用**：仍由平台管理员注册（`POST /api/apps`），不对外开放。

## 安全

- 所有密码只存 bcrypt 哈希；不存明文。
- 路径穿越、恶意 JSON 深度、伪造 `X-Forwarded-For`、账号关联爆破、开放 OAuth
  客户端注册、二维码/绑定 state 劫持、管理端越权等已修复；详见 `SECURITY.md`。
- 会话 Cookie：HttpOnly、SameSite=Lax、HTTPS 下 Secure；CSRF token 与 Origin 校验。
- RS256 签名密钥、SMTP/SSE 密钥不进 git，文件权限 600。

## 测试账号

```
邮箱：test@mail2.sysu.edu.cn
密码：Unisso@Test2026
权限：普通用户（已验证）
```

## 压测（2026-10-25，2 vCPU / 4 GiB）

`ab`，无 keep-alive，本机直连 8080；全部 0 failed：

| 场景 | 并发 | 请求数 | 吞吐 |
|---|---:|---:|---:|
| 静态 `/login` | 200 | 20,000 | 4,735 req/s |
| `/api/health`（MySQL+Redis） | 100 | 10,000 | 2,938 req/s |
| `/api/auth/me`（会话+MySQL） | 100 | 10,000 | 185 req/s |
| `/api/auth/sse/qr/status` | 100 | 10,000 | 7,049 req/s |
| keep-alive `/api/health` | 50 | 3,000 | 1,649 req/s |

混合负载：4 个端点各 5,000 请求、并发 50（共 20,000）同时压测，合计约
2,300 req/s，**0 failed**，压测后健康检查正常。

对照：原 Python 版本在 `c=100` 时被全局限流，`/login` 5,000 请求中 4,800 个
返回 429、`/api/health` 全部 429；其 1,600/1,400 req/s 是拒绝路径的吞吐，不是
成功页面的吞吐，不能与上表直接比较。C++ 版未设置全局限流，仅对登录/注册/绑定等
敏感接口做定向限流。

## 已知限制

- MySQL/Redis 目前是单连接 + 互斥串行化。身份类接口（如 `/api/auth/me`）在
  高并发下受数据库串行影响；静态/健康/QR 不受影响。后续可换连接池。
- `cpp-httplib` 是每连接线程模型；`ab -k` 在并发 ≥100 时偶发 `Length` 计数
  （服务端关闭空闲 keep-alive 连接），并发 50 及无 keep-alive 高并发均为 0 failed。
  生产浏览器连接数很少，不构成实际瓶颈。
- 人脸二次验证已接入动态活体；真人样本的登记/命中已有脸/二次验证成功场景
  还需要 2 个测试账号和 2 张已同意的人脸做验收（见 `docs/FEATURES.md`）。
- Face 服务模型 `buffalo_l` 为非商业研究许可；商业生产前需授权或换模型；
  PAD 阈值仍需真实样本标定。
- 平台 push-to-deploy 会覆盖 `~/deploy/unisso/deploy.sh`；执行 `./repoint.sh` 切回。
- 旧 Python 版保留在 `~/deploy/unisso`，回滚入口 `deploy.sh.python-backup`。

## 回滚

```bash
cp ~/deploy/unisso/deploy.sh.python-backup ~/deploy/unisso/deploy.sh
~/.local/bin/supervisorctl -c ~/deploy/supervisord.conf restart deploy-5829cb069e42fd20
```

## 文档

- 完整功能说明：`docs/FEATURES.md`（登录、设备信任、二次验证、账号关联、
  OAuth、人脸登记/活体、接口、配置、运维、验收清单）。
- 应用接入说明与安全 checklist：
  `.claude/skills/unisso-integration/SKILL.md`。
- 安全修复记录：`SECURITY.md`。
