# UniSSO - 中山大学统一身份认证平台

基于 OAuth2/OIDC 标准的统一身份认证平台，主身份为**中山大学邮箱**（`*.mail*.sysu.edu.cn`），支持绑定集市账号、GitHub、微信等外部身份，为 ssesinfra 学生分组开发平台提供单点登录和第三方应用授权能力。

> [English README](README_EN.md)

## 核心设计

```
┌─────────────────────────────────────────────────────────────┐
│                      UniSSO 身份中心                         │
│  ┌─────────────┐    ┌─────────────────────────────────────┐ │
│  │  主身份      │    │  外部身份绑定 (UserIdentity)        │ │
│  │  *.mail*.   │───▶│  • ssemarket (集市账号)             │ │
│  │  sysu.edu.cn│    │  • github                           │ │
│  │  (邮箱+密码)│    │  • wechat (微信)                    │ │
│  └─────────────┘    │  • custom:xxx (其他应用)            │ │
│                     └─────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
                              │
              ┌───────────────┼───────────────┐
              ▼               ▼               ▼
        ┌─────────┐    ┌─────────┐    ┌─────────┐
        │ 应用 A  │    │ 应用 B  │    │ 应用 C  │
        └─────────┘    └─────────┘    └─────────┘
```

### 身份体系

| 层级 | 说明 | 示例 |
|------|------|------|
| **主身份** | 中山大学邮箱，唯一标识 | `xxx@mail2.sysu.edu.cn` |
| **显示名** | 昵称/姓名，可修改 | `student` / `张三` |
| **外部身份** | 绑定的第三方账号 | 集市账号 `market_user` |

## 功能特性

- **中大邮箱登录**：注册/登录必须使用 `*.mail*.sysu.edu.cn` 邮箱
- **外部身份绑定**：可绑定集市账号、GitHub、微信等
- **OAuth2 授权服务器**：标准 Authorization Code Flow + PKCE
- **OIDC 支持**：提供 UserInfo 端点和 OpenID Discovery
- **应用注册管理**：开发者可注册应用，获取 client_id/client_secret
- **用户授权控制**：用户可查看、修改、撤销对各应用的授权
- **RBAC 权限模型**：基于角色的权限控制
- **ssesinfra 一键部署**：完全符合平台部署契约

## 技术架构

| 组件 | 文件 | 职责 |
|------|------|------|
| `app/auth.py` | 认证模块 | 邮箱登录、Session 管理、外部身份绑定、权限检查 |
| `app/oauth2_server.py` | OAuth2 服务器 | 授权码、Token 颁发/验证、Consent 管理 |
| `app/routes.py` | 路由 | 页面 + API 端点 |
| `app/models.py` | 数据模型 | User(主身份)、UserIdentity(外部绑定)、Application 等 |
| `app/middleware.py` | 安全中间件 | 安全头、限流、Host 验证、HTTPS 检测 |
| `app/audit.py` | 审计日志 | 安全事件记录与脱敏 |
| `app/security.py` | 安全工具 | 密码策略、JWT、CSRF、PKCE |
| `app/platform.py` | 平台感知 | 宿主机上下文、负载感知、动态 worker 调整 |
| `app/storage.py` | 存储模块 | MinIO 文件上传/下载 |

### 数据模型

- **User**：用户表，主身份为 `email`（sysu 邮箱）
- **UserIdentity**：外部身份绑定表（集市账号、GitHub、微信等）
- **Application**：OAuth2 客户端应用
- **UserConsent**：用户授权记录
- **Role / Permission**：RBAC 角色权限

## 部署方式

### 在 ssesinfra 平台一键部署

1. 在平台「部署」卡填写：

| 字段 | 值 |
|------|-----|
| 仓库地址 | `https://github.com/SysuMirror/unisso.git` |
| 分支 | `main` |
| 子目录 | 留空 |
| 类型 | 常驻服务（崩了重启） |

2. 在「数据库」页创建 label=组名 的 MySQL 凭证（**必须**，用于持久化用户数据）
3. 在「部署」页添加环境变量（**必须**）：

```
UNISSO_SECRET_KEY=你的强随机密钥（openssl rand -base64 48）
UNISSO_ADMIN_PASSWORD=管理员强密码
```

4. 点「部署」

### Push-to-Deploy（已接通，推荐）

向 `main` 分支 push 即自动重部署，无需手动操作：

```
git push origin main
  → GitHub webhook → https://ssemarket.cn:24000/deploy/webhook?source=github
  → yatterra 匹配 repo+branch（auto_deploy=true）
  → pod 内 git fetch + reset --hard origin/main → 重启 deploy 程序
  → 健康检查通过后上报 ready（全程约 1 分钟）
```

`deploy.sh`（pod 内的运行入口）会自动完成：
- venv 不存在则创建；`requirements.txt` 的 sha256 变化时增量安装依赖（**用阿里云 pypi 镜像**，pod 网络不通 pypi.org）
- 设置生产 env：`UNISSO_ISSUER=https://sso.ssemarket.cn`、RS256 签名、trusted proxy、allowed hosts
- 读取 `./secrets/signing-{private,public}.pem`（**不进仓库**，pod 内手工放置）
- `exec .venv/bin/python start.py`

> 运维/排障手册见 `.claude/skills/unisso-devops/SKILL.md`（供 AI 和人类使用的完整 runbook）。

### 环境变量

平台自动注入：

| 变量 | 说明 |
|------|------|
| `PORT` | 服务端口（默认 8080） |
| `MYSQL_HOST/PORT/USER/PASSWORD/DB` | MySQL 连接信息 |
| `REDIS_HOST/PORT/USER/PASSWORD/PREFIX` | Redis 连接信息（可选） |
| `DEPLOY_ID/REPORT_URL/REPORT_TOKEN` | 平台上报信息 |

可选配置：

| 变量 | 说明 |
|------|------|
| `UNISSO_SECRET_KEY` | JWT 签名密钥（**生产环境必须设置，>=32位**） |
| `UNISSO_ADMIN_EMAIL` | 初始管理员邮箱（**必须是 sysu 邮箱格式**） |
| `UNISSO_ADMIN_PASSWORD` | 初始管理员密码（**必须设置才会创建管理员**） |
| `UNISSO_ADMIN_USERNAME` | 初始管理员用户名（可选，默认 `admin`） |
| `TRUST_PROXY` | `true` 时信任反向代理的 HTTPS 头 |
| `FORCE_HTTPS` | `true` 时 HTTP 请求自动 308 重定向到 HTTPS |
| `HTTPS_CERT/HTTPS_KEY` | 自签名证书路径（应用直接监听 HTTPS 时使用） |
| `UNISSO_ISSUER` | OIDC 规范 issuer（生产必须 HTTPS，如 `https://sso.ssemarket.cn`） |
| `UNISSO_JWT_ALGORITHM` | 默认 `RS256`；RS256 需配 `UNISSO_SIGNING_PRIVATE_KEY_FILE`/`_PUBLIC_KEY_FILE`/`_KEY_ID` |
| `UNISSO_ALLOWED_HOSTS` | JSON 数组，Host 头白名单（如 `["sso.ssemarket.cn"]`） |
| `UNISSO_TRUSTED_PROXY_CIDRS` | JSON 数组，可信代理网段（X-Forwarded-For 解析用） |
| `UNISSO_CSRF_TRUSTED_ORIGINS` | JSON 数组，CSRF 信任来源 |

邮箱注册（可选，需 SMTP）：

| 变量 | 说明 |
|------|------|
| `UNISSO_EMAIL_REGISTRATION_ENABLED` | `true` 开启邮箱验证注册 |
| `UNISSO_SMTP_HOST/PORT/USERNAME/PASSWORD` | SMTP 服务器与凭证 |
| `UNISSO_SMTP_FROM` / `UNISSO_SMTP_FROM_NAME` | 发件人地址/名称 |
| `UNISSO_SMTP_USE_TLS` / `UNISSO_SMTP_STARTTLS` | 465 用 `USE_TLS=true`；587 用 `STARTTLS=true` |

### HTTPS 配置（自动）

#### 场景 A：平台有 HTTPS 反向代理（推荐）

在「部署」页添加环境变量：

```
TRUST_PROXY=true
FORCE_HTTPS=true
UNISSO_SECRET_KEY=你的强随机密钥
```

应用会自动：识别 `X-Forwarded-Proto: https` 头、设置 Secure Cookie、HTTP 请求自动 308 重定向到 HTTPS、添加 HSTS 响应头。

#### 场景 B：应用自己监听 HTTPS

在「部署」页添加环境变量：

```
HTTPS_CERT=/app/certs/cert.pem
HTTPS_KEY=/app/certs/key.pem
UNISSO_SECRET_KEY=你的强随机密钥
```

`deploy.sh` 会自动生成自签名证书并启动 HTTPS。

### 首次部署

- 自动执行数据库迁移（`alembic upgrade head`）
- 自动创建默认角色、权限
- 如果设置了 `UNISSO_ADMIN_EMAIL` + `UNISSO_ADMIN_PASSWORD`，自动创建管理员账号

## OAuth2 接入指南

### 1. 注册应用

登录 UniSSO → 应用管理 → 注册应用，填写：
- 应用名称
- 回调地址（如 `http://localhost:3000/callback`）
- 需要的 Scopes（如 `openid profile email`）

获得 `client_id` 和 `client_secret`。

### 2. 授权流程（Authorization Code + PKCE）

**Step 1: 生成 PKCE**
```python
import secrets, hashlib, base64

verifier = base64.urlsafe_b64encode(secrets.token_bytes(32)).decode().rstrip('=')
challenge = base64.urlsafe_b64encode(
    hashlib.sha256(verifier.encode()).digest()
).decode().rstrip('=')
```

**Step 2: 引导用户到授权页**
```
GET /authorize?response_type=code
    &client_id=YOUR_CLIENT_ID
    &redirect_uri=http://localhost:3000/callback
    &scope=openid profile
    &state=random_state
    &code_challenge=CHALLENGE
    &code_challenge_method=S256
```

**Step 3: 用 code 换 token**
```bash
curl -X POST /api/oauth/token \
  -H "Content-Type: application/x-www-form-urlencoded" \
  -d "grant_type=authorization_code" \
  -d "code=CODE_FROM_CALLBACK" \
  -d "redirect_uri=http://localhost:3000/callback" \
  -d "client_id=YOUR_CLIENT_ID" \
  -d "client_secret=YOUR_SECRET" \
  -d "code_verifier=VERIFIER"
```

**Step 4: 获取用户信息**
```bash
curl /api/oauth/userinfo \
  -H "Authorization: Bearer ACCESS_TOKEN"
```

返回示例：
```json
{
  "sub": "用户ID",
  "preferred_username": "student",
  "email": "student@mail2.sysu.edu.cn",
  "name": "Student Name",
  "roles": ["user"]
}
```

### 3. OIDC Discovery

```bash
GET /.well-known/openid-configuration
```

## API 端点汇总

### 认证
- `POST /api/auth/login` - 登录（email + password）
- `POST /api/auth/logout` - 登出
- `POST /api/auth/register` - 注册（email 必须是 sysu 邮箱）
- `GET /api/auth/me` - 当前用户信息

### 外部身份绑定
- `GET /api/identities` - 列出已绑定的外部身份
- `POST /api/identities` - 绑定外部身份
- `DELETE /api/identities/{id}` - 解绑外部身份

### OAuth2
- `GET /authorize` - 授权页面
- `POST /api/oauth/authorize` - 确认授权
- `POST /api/oauth/token` - 获取 Token
- `POST /api/oauth/introspect` - Token 验证
- `GET /api/oauth/userinfo` - 用户信息（OIDC）
- `GET /.well-known/openid-configuration` - OIDC 配置

### 应用管理
- `GET /api/apps` - 列出应用
- `POST /api/apps` - 注册应用
- `GET/PUT/DELETE /api/apps/{id}` - 应用 CRUD
- `POST /api/apps/{id}/reset-secret` - 重置密钥

### 用户授权管理
- `GET /api/consents` - 我的授权列表
- `PUT /api/consents/{id}` - 修改授权范围
- `DELETE /api/consents/{id}` - 撤销授权

### 管理员
- `GET /api/admin/users` - 用户列表
- `POST /api/admin/users` - 创建用户
- `PUT/DELETE /api/admin/users/{id}` - 用户管理

### 平台探针
- `GET /health` - 健康检查
- `GET /env` - 环境变量状态
- `GET /db` - 数据库连通性
- `GET /redis` - Redis 连通性
- `GET /minio` - MinIO 连通性
- `GET /platform` - 平台信息

## 本地开发

```bash
# 1. 创建虚拟环境
python3 -m venv .venv
source .venv/bin/activate

# 2. 安装依赖
pip install -r requirements.txt

# 3. 运行迁移
alembic upgrade head

# 4. 启动服务（开发模式）
UNISSO_SECRET_KEY="$(openssl rand -base64 48)" \
  uvicorn app.main:app --reload --port 8080

# 或用生产启动器（带环境感知）
UNISSO_SECRET_KEY="$(openssl rand -base64 48)" \
  python start.py

# 5. 本地 HTTPS（可选）
./setup-https.sh
UNISSO_SECRET_KEY="$(openssl rand -base64 48)" \
HTTPS_CERT=./certs/cert.pem \
HTTPS_KEY=./certs/key.pem \
python start.py
```

## 安全加固（已内置）

### 1. 密码安全
- **bcrypt 哈希**（限制 72 字节，统一截断）
- **密码复杂度策略**：最少 8 位，必须包含大小写字母、数字、特殊字符
- **弱密码检测**：禁止常见弱密码和连续字符序列

### 2. 登录保护
- **失败次数限制**：5 分钟内失败 5 次锁定 15 分钟
- **审计日志**：记录所有登录/注册/授权/管理员操作（`logs/audit.log`）
- **脱敏处理**：日志中密码、token 等敏感字段自动脱敏

### 3. HTTP 安全头
- `X-Content-Type-Options: nosniff`
- `X-Frame-Options: DENY`
- `X-XSS-Protection: 1; mode=block`
- `Content-Security-Policy`
- `Referrer-Policy: strict-origin-when-cross-origin`
- `Permissions-Policy`
- `Strict-Transport-Security`（HTTPS 环境自动启用）

### 4. Cookie 安全
- `HttpOnly` + `Secure`（HTTPS 时）+ `SameSite=Lax` + `Path=/`

### 5. OAuth2 安全
- **PKCE 强制**：所有授权码流程必须携带 code_challenge
- **redirect_uri 严格匹配**：防止开放重定向漏洞
- **授权码一次性使用**：使用后立即失效

### 6. JWT 安全
- **固定算法 HS256**：防止 algorithm confusion attack
- **aud/iss/jti 声明**：包含 audience、issuer、唯一标识
- **密钥长度检查**：生产环境强制要求 >= 32 位非默认密钥

### 7. 限流保护
- 登录端点：1 分钟 10 次
- 注册端点：1 分钟 5 次
- 授权端点：1 分钟 20 次
- Token 端点：1 分钟 30 次

### 8. 文件上传安全
- **MIME 类型白名单**：仅允许 JPEG/PNG/GIF/WebP/SVG
- **魔数检查**：验证文件头防止伪装扩展名
- **大小限制**：头像 5MB，图标 2MB

### 9. 管理员安全
- **默认密码检测**：启动时如果使用默认密码，打印安全警告
- **操作审计**：所有管理员操作记录审计日志

## 安全注意事项

1. **生产环境必须设置 `UNISSO_SECRET_KEY`**（至少 32 位随机字符串）
2. **修改默认管理员密码**（通过 `UNISSO_ADMIN_PASSWORD` 环境变量）
3. **启用 HTTPS**：平台有反向代理时设置 `TRUST_PROXY=true FORCE_HTTPS=true`
4. **定期轮换密钥**：重置应用的 client_secret
5. **查看审计日志**：`tail -f logs/audit.log`

## 技术栈

- **FastAPI** - Web 框架
- **SQLAlchemy 2.0 + Alembic** - ORM 和数据库迁移
- **Redis** - Session、缓存（可选，无 Redis 时自动降级到内存存储）
- **PyJWT** - JWT Token
- **Passlib + bcrypt** - 密码哈希
- **Jinja2** - 服务端模板
- **MinIO** - 对象存储（头像/图标）

## License

MIT License - 详见 [LICENSE](LICENSE) 文件。
