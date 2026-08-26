#!/usr/bin/env python3
"""UniSSO 本地开发启动器

线上部署：平台注入环境变量 → python start.py（gunicorn，见 compose/平台契约）
本地开发：python dev_local.py [--mysql] [--port 8000]

两种本地模式：
- 默认：SQLite + Redis 内存回退（零外部依赖，开箱即用）
- --mysql：读取项目根目录 .env.local（已被 .gitignore 排除）中的远程 MySQL 凭据

.env.local 格式（KEY=VALUE，每行一条）：
    MYSQL_HOST=175.178.90.215
    MYSQL_PORT=3506
    MYSQL_USER=root
    MYSQL_PASSWORD=xxxx
    MYSQL_DB=unisso
"""
import os
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, ROOT)

MYSQL_KEYS = ("MYSQL_HOST", "MYSQL_PORT", "MYSQL_USER", "MYSQL_PASSWORD", "MYSQL_DB")


def load_env_local() -> bool:
    """读取 .env.local 注入环境变量（已存在的不覆盖）"""
    path = os.path.join(ROOT, ".env.local")
    if not os.path.isfile(path):
        return False
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            os.environ.setdefault(k.strip(), v.strip())
    return True


def main():
    args = sys.argv[1:]
    use_mysql = "--mysql" in args
    port = args[args.index("--port") + 1] if "--port" in args else "8000"

    # 本地开发默认值（注意：Settings 读取带 UNISSO_ 前缀的变量）
    os.environ.setdefault("UNISSO_SECRET_KEY", "local-dev-secret-key-32-chars-minimum!!")
    os.environ.setdefault("UNISSO_DEBUG", "true")
    os.environ.setdefault("UNISSO_ADMIN_EMAIL", "admin@mail2.sysu.edu.cn")
    os.environ.setdefault("UNISSO_ADMIN_PASSWORD", "Admin123456!")

    if use_mysql:
        if not load_env_local():
            print("【dev_local】--mysql 需要项目根目录的 .env.local，包含：")
            print("  " + " / ".join(MYSQL_KEYS))
            sys.exit(1)
        print(f"【dev_local】模式=MySQL({os.environ.get('MYSQL_HOST')}:{os.environ.get('MYSQL_PORT')}"
              f"/{os.environ.get('MYSQL_DB')})")
    else:
        # 清掉可能残留的 MYSQL_* 环境变量，确保走 SQLite
        for k in MYSQL_KEYS:
            os.environ.pop(k, None)
        print("【dev_local】模式=SQLite(./unisso.db)，Redis 内存回退")

    print(f"【dev_local】http://127.0.0.1:{port}（UNISSO_DEBUG=true）")

    import uvicorn
    uvicorn.run("app.main:app", host="127.0.0.1", port=int(port))


if __name__ == "__main__":
    main()
