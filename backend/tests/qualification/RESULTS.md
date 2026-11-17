# Join → UniSSO → Develop 验证记录

2026-11-17：完成正常链路模拟、相关异常验证和修复回归，60 项检查通过。

## 环境与边界

- 源码基准：`6d279dc03e43b5cc036505bd757de72a2ca51158`；修改分支：`codex/join-develop-chain`。
- Windows WSL Ubuntu 22.04，GCC 11.4、C++20；真实 UniSSO 后端处理 HTTP 请求。
- 独立 MariaDB 10.6.23、Redis 6.0.16，随机本地端口、临时数据目录、测试账号、客户端和签名密钥。没有连接生产数据库或 Redis。
- Join 由测试客户端发起写资格请求；Develop 由测试客户端读取身份，再模拟后端业务访问判断。
- `userinfo` 使用测试密钥签发的 access token，验证相同 token 能读取资格变化；本轮没有执行完整授权码、PKCE、刷新令牌流程。
- 测试结束时已停止测试进程。逐项结果见 `results-20261117.json`。

这是本地隔离集成测试，尚未在平台 Dev、生产 MySQL 或真实 Join、Develop 上联调，也未发布。

## 基线观察与修复

| 场景 | 修改前实测 | 修改后实测 |
| --- | --- | --- |
| Develop 客户端写资格 | 200，允许写入 | 403，要求独立写权限 |
| 公共客户端不带密钥写资格 | 200，允许写入 | 401，M2M 要求机密客户端及有效密钥 |
| 未传资格值 | 200，默认授予 | 400，必须显式传值 |
| Redis 停止时登录 | 200，返回未存储的会话 | 503，不返回成功会话 |
| Redis 停止时验证会话 | 401，误报会话无效 | 503，表示依赖不可用 |
| 数据库停止时写资格 | 401，误报客户端无效 | 503，表示依赖不可用 |

其他已验证的修复：会话续期、注销写入失败返回 503；资格写入后的确认读取失败返回 503；Redis 连接出错后可在后续请求重连。

数据库查询的检查模式在同一次加锁操作内报告失败，避免从共享的“最后错误”推断请求结果。现有查询默认行为保留；本次链路显式启用检查模式。

## 验证结果

- 无资格账号登录后，模拟 Develop 业务返回 403；Join 授予后返回 200；撤销后重新返回 403。
- 已有 SSO 会话和同一 access token 重新读取时，均能看到资格变化。重复授权不增加用户记录。
- Develop、外部应用、未启用 M2M 的应用、待审核应用均不能写资格。停用客户端、错误密钥、公共客户端、未知客户端被拒绝。
- Basic 认证、邮箱和集市 ID 定位、旧参数名 `developer` 可用；缺失参数、非法资格值、未知用户分别返回对应错误。
- SQL 写入错误保留旧值并返回 503；写入已提交、确认读取失败也返回 503，恢复后可重复设置目标值。
- Redis 拒绝会话写入或删除时返回 503；失败的续期、注销没有删除原会话。
- 数据库停止期间，登录、会话读取、写资格、userinfo 均返回 503；恢复后相同会话和 token 可继续读取。
- Redis 停止期间，登录、会话读取和注销返回 503；资格写入仍可通过数据库完成。
- 测试 Redis 关闭持久化，重启后旧会话确实丢失，此时返回 401。新登录成功，UniSSO 无需重启。
- 未配置资格写入名单时拒绝写入，内部应用登录仍可用。

## 后续部署需要配置的内容

设置 `UNISSO_DEVELOPER_WRITE_CLIENTS=<Join 的实际 client_id>`；多个值用逗号分隔，空值表示不允许应用写资格。调用方同时必须是已批准、启用 M2M 的内部机密客户端，并提供有效密钥。历史登录白名单不能赋予资格写权限。

写请求必须显式提供 `is_developer: true` 或 `false`，也保留 `developer` 别名及 `1` / `0`。旧调用若省略此值，需要同步调整。

503 表示本次请求无法确认结果。对于资格设置，可以恢复后查询，或重试相同目标值；写操作可能已经提交。下游应区分 503 与无效身份、无资格。

## 复跑

Linux 上准备 CMake、C++20 编译器、MySQL 客户端开发库、hiredis、OpenSSL、zlib、curl、crypt、cpp-httplib，以及 MariaDB 和 Redis 可执行文件，然后运行：

```sh
cmake -S backend -B backend/build
cmake --build backend/build -j4
python3 backend/tests/qualification/run.py \
  --binary backend/build/unisso_v2 \
  --results /tmp/qualification-results.json
```

测试脚本自行建立隔离数据库和客户端，不读取部署凭据；数据库必须支持 MariaDB 的 `--skip-grant-tables` 空目录启动方式。本轮以 root 在 WSL 执行，服务通过随机 loopback 端口访问。

本轮运行环境将依赖解压到 WSL 的 `/home/a1784/.cache/unisso-qualification-20261117/deps`，测试命令另加 `--deps-root` 指向该目录。Ubuntu 22.04 系统 cpp-httplib 0.10.3 与现有代码不匹配，本轮使用官方 v0.14.3 header-only 构建，未更改业务源码或仓库 CMake 以适配旧版本。

构建通过；`git diff --check` 通过。编译器仍报告原有的 Redis 构造函数成员初始化顺序等警告，本轮没有扩大处理范围。
