---
name: unisso-integration
description: 把学院内部平台接入 UniSSO 无感登录（M2M 密码校验 + 共享会话 Cookie）。覆盖登录、静默 SSO、登出、错误契约、部署自检守则与验收清单。
---

# UniSSO 内部平台无感登录接入

内部平台（first-party）接入 = 后端 M2M 密码校验 + 共享会话 Cookie。用户永不跳转 unisso。
UniSSO 只负责身份（sub/sse_sub/internal/name/email/picture）；权限与角色由各平台自行判定。

## 前置

1. 在 https://sso.ssemarket.cn/admin/apps 由超管创建**内接**应用，拿到 client_id / client_secret（secret 只显示一次），打开"无感登录"开关（m2m_enabled，仅内接应用可用）。
2. 凭据下发到平台后端 env/config。**唯一可信来源是运行实例的 env**；仓库里的 env/compose 值视为可能过期——重建容器/进程前先从运行实例取（docker inspect / /proc/PID/environ）。

## 登录（无感主路径）

```
POST https://sso.ssemarket.cn/api/oauth/password/verify
Content-Type: application/json
{"client_id":"...","client_secret":"...","email":"...","password":"..."}
```

- 200：`user{sub,sse_sub,internal,name,email,picture}` + `sso_session` + `sso_cookie_name/sso_cookie_domain/sso_max_age`
- 平台按响应的 `sso_cookie_*` 三个字段种 Cookie（Domain=.ssemarket.cn，HttpOnly/Secure/Lax）。**以响应字段为准，不要硬编码名字**（2026-11-05 起默认名 `unisso_sid`，旧名 `sse_sso` 兜底到 2026-11-12）
- 错误契约：401 `invalid_credentials`（邮箱或密码错误）／403 `password_verify_forbidden`（client 不合规：查 client_id 是否配错、应用是否内接且开关开启）／429 `too_many_attempts`（同 ip+email 5 败锁 15 分钟，勿自动重试）

## 用户目录（查询全部用户资料）

```
POST https://sso.ssemarket.cn/api/oauth/users/list
Content-Type: application/json
{"client_id":"...","client_secret":"..."}
```

- 200：`{ok,total,users[]}`；users[] 每项含 `sub/sse_sub/internal/preferred_username/name/email/picture/student_id/active/created_at/last_login_at`（无密码哈希、无管理员标记）
- 权限与无感登录**完全一致**：内接应用 + 打开"无感登录"开关（m2m_enabled），否则 403 `password_verify_forbidden`；凭据错误 401 `invalid_client`

## 静默 SSO（换平台自动进）

```
POST https://sso.ssemarket.cn/api/oauth/session/verify   （Cookie 值服务端校验）
POST https://sso.ssemarket.cn/api/oauth/session/revoke   （登出）
```

平台读 Cookie **先 `unisso_sid` 再 `sse_sso`**；登出两个名字都按 `Domain=.ssemarket.cn; Path=/; Max-Age=0` 清。

## 前端 boot 规约（防"刷新才登录"）

页面加载即发一次 sso/try，路由守卫/首屏渲染 await 探测结果；全站单飞（共享同一 Promise）。三态：ok→进登录态并用结果重渲染当前视图；401→落登录页；503/网络错误→**保留本地会话不踢人**。

## 部署守则

1. 重建/重启后必须跑部署自检：用本平台 client 真实调一次 password/verify（E2E 账号 music-e2e-test@ssemarket.cn / Test123456）必须 200，否则回滚镜像。集市机样板：/root/SSE_Market/deploy-check-sso.sh。
2. unisso 403 日志带 `client= kind= m2m=` 一行，排错先看它。
3. 凭据来源错误是历史事故主因（compose.yml 过期值导致全站 403），切记以运行实例为准。

## 验收清单

- password/verify：正确密码 200；错误密码 401；假 client 403
- sso/try：无 Cookie 401 no_sso；假值双名 401 invalid_sso；真会话双名 200
- 冷启动带真 Cookie：无刷新直进登录态（真实 Chrome）
- 登出：双名都清；其它平台下次探测落登录页
