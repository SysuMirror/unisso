---
name: unisso-devops
description: UniSSO (SysuMirror/unisso) 的开发与运维 runbook — 部署链路、线上环境拓扑、排障步骤、常见坑。当任务涉及本仓库的部署、线上服务异常、登录/注册/OAuth 问题、数据库或限流排查时使用。
---

# UniSSO 开发与运维 Runbook

> **v2 现状（2026-11）**：main 分支已是 v2 = **C++ 后端（cmake → `backend/build/unisso_v2`）+ Vue3 SPA**；下文凡提到 `app/*.py`、venv、alembic、`start.py`、`login.html` 的都是 **v1(Python) 时代**遗留，仅作历史参考。生产运行目录 `~/deploy/unisso`，由 yatterra push-to-deploy 管；改动/排障细节见记忆 [[unisso-pod-git-repos]]。

## 1. 线上拓扑（当前事实，勿凭猜测）

| 项 | 值 |
|---|---|
| 仓库 | `https://github.com/SysuMirror/unisso`（main 分支；YatTerra/unisso 是同一仓库的重定向） |
| 运行位置 | k3s pod `group-unisso-*`（namespace **`clouds`**），应用监听 pod 内 **8080** |
| 进程管理 | pod 内 cloud 用户的 supervisord：`~/.local/bin/supervisorctl -c ~/deploy/supervisord.conf`；程序名 `deploy-5829cb069e42fd20`（代码在 `~/deploy/unisso`） |
| 公网入口 | `https://sso.ssemarket.cn`（经中继/nginx 反代到 pod 8080） |
| 数据库 | `mysql.platform-infra.svc.cluster.local:3306`，库名 `unisso`（凭证由 yatterra 按 label=unisso 注入） |
| Redis | `redis.platform-infra.svc.cluster.local:6379`（session/CSRF/登录限流/注册 pending） |
| 部署平台 | yatterra（宿主机 `/opt/yatterra/web`，web 端口 8090，公网 24000） |
| 部署配置 | `/opt/yatterra/deploys.json` → `deploys.unisso[0]`：`auto_deploy: true`、`repo`、自定义 `env` dict（SMTP/UNISSO_* 都在这里） |
| Webhook secret | `/opt/yatterra/webhook.conf`（GitHub hook 用 HMAC-SHA256 签名，URL `https://ssemarket.cn:24000/deploy/webhook?source=github`） |
| 签名密钥 | pod 内 `~/deploy/unisso/secrets/signing-{private,public}.pem`（RS256，**不进仓库**） |

## 2. 部署链路

```
git push origin main
→ GitHub webhook（push 事件）
→ yatterra /deploy/webhook?source=github（验签 → 解析 repo+branch → find_by_repo_branch）
→ deploys.run(group="unisso", dep)：
    1. pod 内: cd ~/deploy/unisso && git fetch --all && git reset --hard origin/main
    2. 重写 ~/deploy/programs/deploy-<id>.conf（env = 平台注入 + dep["env"]）
    3. supervisorctl reread/update/restart
    4. 健康检查（dep.health = "/"）→ app 上报 ready
```

> **坑：第 1 步的 `git fetch` 跑在 `su -l cloud` 里，拿不到平台注入的代理 env**，pod 直连 `github.com:443` 一旦被断（如 2026-11-13），webhook 会**静默失败**：`deploys.run` 返回 `clone 失败: curl 28 … port 443`、`deploys.json` 的 `last_ref` 不变、`audit.log` 只有 `matches=1` 无报错。修法（对 pod 持久）：
> ```bash
> sudo kubectl -n clouds exec <pod> -- su -l cloud -c \
>   'git config --global http.proxy http://172.16.156.28:7890; git config --global https.proxy http://172.16.156.28:7890'
> ```
> 代理地址即 `siteconf.proxy_env_defaults()` 的 `PROXY_URL`（宿主 mihomo）。改完再手动触发一次 `deploys.run`（见 §3）。

**deploy.sh（仓库根目录）职责（v2）**：在 pod 内按 mtime 增量 `cmake` 编译 `backend/`（产物 `backend/build/unisso_v2`）；若 `frontend/` 源文件（含 `frontend/public/`、`index.html`）比 `backend/web/index.html` 新则 `npm install && npm run build` 重建 SPA；最后 `exec build/unisso_v2`。**改动的文件在 `git reset --hard` 后会重置 mtime → 触发重编译，重启后有约 10–40s 502（编译期），属预期。**（v1 的 venv/requirements/start.py 描述已过时。）

改 yatterra 侧代码（`/opt/yatterra/web/*.py`）后要 `sudo kill -HUP <gunicorn master pid>` graceful reload；`deploys.json` 是每次请求现读的，改完即生效不用重启。

> 在**进程内**调 `deploys.*`（不走 gunicorn）前必须先带上 yatterra 的 env，否则 `siteconf` 会回落到空状态 `/srv/yatterra`、`load()` 返回 `{"deploys":{}}`：`sudo bash -c 'set -a; . /opt/yatterra/.env; set +a; python3 …'`。

## 3. 常用命令速查

```bash
# pod 名（会变，用 label 解析）
sudo kubectl -n clouds get pod -l app=group-unisso

# 应用状态 / 日志
sudo kubectl exec -n clouds <pod> -- su -l cloud -c \
  "~/.local/bin/supervisorctl -c ~/deploy/supervisord.conf status"
sudo kubectl exec -n clouds <pod> -- tail -50 /home/cloud/deploy/unisso/logs/stderr.log
sudo kubectl exec -n clouds <pod> -- tail -50 /home/cloud/deploy/unisso/logs/stdout.log

# 审计日志（登录成功/失败、注册、webhook 触发都在这）
sudo kubectl exec -n clouds <pod> -- tail -50 /home/cloud/deploy/unisso/logs/audit.log

# 本地健康检查（必须带 Host 头，ALLOWED_HOSTS 只放行 sso.ssemarket.cn）
sudo kubectl exec -n clouds <pod> -- curl -s -o /dev/null -w "%{http_code}\n" \
  -H "Host: sso.ssemarket.cn" -H "X-Forwarded-Proto: https" http://127.0.0.1:8080/login

# 手动重部署（等价于 push 触发）——须先 source yatterra .env，否则读到空状态
sudo bash -c 'set -a; . /opt/yatterra/.env; set +a
python3 -c "
import sys; sys.path.insert(0,\"/opt/yatterra/web\"); import deploys
dep = deploys.list_for(\"unisso\")[0]
print(deploys.run(\"unisso\", dep))"'

# 查部署状态
sudo python3 -c "
import json; d=[x for x in json.load(open('/opt/yatterra/deploys.json'))['deploys']['unisso']][0]
print(d['last_status'], d['last_ref'][:8], d['last_msg'][-200:])"
```

## 4. 排障决策树

**登录报 invalid credentials（网页端）**
1. 查 audit.log 该 email 的记录：`凭证错误` = 密码真错了；`登录被锁定` = 限流。
2. 限流 key 是 `login_attempts:{ip}:{email}`（Redis，5 次/5 分钟 → 锁 15 分钟）。按 email 隔离，不会连坐。
3. 密码无法找回（bcrypt 单向），只能重置：更新 `users.password_hash`（用 `passlib CryptContext(schemes=["bcrypt"]).hash(新密码)`）。

**mirror（软工集市镜像）报 "invalid UniSSO credentials"**
- mirror 把 unisso 的 `invalid_credentials` 和 `too_many_attempts` 都映射成这句话（429 = 锁定，401 = 密码错）。
- mirror 是服务端代理，所有学生请求到 unisso 都是同一出口 IP；限流已按 ip+email 隔离，但**注册邮件发送**仍按 IP 限（`registration_ip_hourly_limit`）。

**应用起不来（supervisor FATAL）**
1. `tail stderr.log` 看最后一个 Traceback。
2. 历史坑：`email-validator is not installed` → venv 依赖没同步（deploy.sh 的 sha256 机制应自动处理，若失败手动 `.venv/bin/pip install -i https://mirrors.aliyun.com/pypi/simple/ -r requirements.txt`）。
3. `Production issuer must use HTTPS` → 缺 `UNISSO_ISSUER`（deploy.sh 已内置默认值）。
4. 签名密钥错误 → 检查 `~/deploy/unisso/secrets/` 是否存在且配对。
5. 端口被占 → 确认没有旧程序抢 8080（旧手工 release 的 conf 已移到 `~/deploy/programs-disabled/`，**不要移回来**）。

**webhook 没触发**
1. `gh api repos/SysuMirror/unisso/hooks/680722110/deliveries --jq '.[0] | {status_code}'`（200 才正常）。
2. yatterra 侧：`sudo grep deploy_webhook /opt/yatterra/audit.log | tail`（`matches=0` = deploys.json 的 repo/branch/auto_deploy 不匹配；`auth fail` = secret 不符）。
3. 偶发 500 曾出现过一次（疑似并发写 deploys.json），重推一次空提交即可验证。

## 5. 开发须知

- **配置全走环境变量**（`app/config.py`，pydantic `env_prefix="UNISSO_"`；`TRUST_PROXY`/`FORCE_HTTPS`/`MYSQL_*` 等无前缀）。加新配置项记得更新 README 的环境变量表。
- **本地开发**：`python dev_local.py`（SQLite）；不要把 `unisso.db`、`secrets/`、`.venv/` 提交进仓库（.gitignore 已排除）。
- **数据库迁移**：alembic（`alembic.ini` + `migrations/`），启动时自动 `upgrade head`。改 models 必须生成迁移文件。
- **安全约束**（改动前先读 `app/token_keys.py`、`app/request_security.py`）：
  - 生产 issuer 必须 HTTPS；RS256 私钥 ≥2048 位。
  - `X-Forwarded-For` 只信任 `UNISSO_TRUSTED_PROXY_CIDRS` 内的 hop，从右往左取第一个不可信 IP 作为 client_ip。
  - 登录/注册限流状态在 Redis，key 带 `REDIS_PREFIX`。
- **审计**：登录、注册、webhook 等安全事件写 `logs/audit.log`（JSON 行），排障优先看它而不是猜。
- 提交前自检：`python -m py_compile app/*.py`；涉及模板改动时确认 `templates/` 同步（登录/注册页在 `login.html`/`register.html`/`verify_email.html`）。

## 6. 已知历史坑（别再踩）

1. pod 网络不通 pypi.org —— 装依赖必须用国内镜像。
2. `deploy.sh` 曾经假设 venv 已预建，新依赖装不上导致 FATAL —— 现在按 requirements sha256 自动同步。
3. 生产 env（issuer/JWT/proxy）曾经只在 `start-production.sh` 里（手工 release 专用），deploy 程序拿不到 —— 现已并入 `deploy.sh`。
4. 登录限流曾经只按 IP，所有 mirror 用户共享出口 IP 会互相连坐 —— 现按 ip+email。
5. 旧手工 release（`unisso-email-registration-20260908`）已废弃，其 supervisord conf 在 `~/deploy/programs-disabled/`。
