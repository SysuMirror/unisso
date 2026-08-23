#!/bin/sh
# UniSSO - 统一单点登录系统部署入口
# 符合 ssesinfra 平台部署契约：
#   - 绑定 0.0.0.0:$PORT
#   - 长驻前台（supervisord 直接监督，崩溃自动重启）
#   - 环境感知（根据负载动态调整 worker 数）
#   - 幂等初始化（venv、数据库迁移）
#   - 自动 HTTPS 配置（可选）
set -eu

cd "$(dirname "$0")"

PORT="${PORT:-8080}"
VENV=".venv"

# 1) 一次性建虚拟环境并装依赖
if [ ! -x "$VENV/bin/python" ]; then
  echo "【UniSSO】首次初始化 venv"
  python3 -m venv "$VENV"
  "$VENV/bin/pip" install --quiet --upgrade pip
  "$VENV/bin/pip" install --quiet -r requirements.txt
fi

# 2) 数据库迁移（幂等：已迁移则跳过）
echo "【UniSSO】检查数据库迁移"
export PYTHONPATH="${PYTHONPATH:-$(pwd)}"
if ! "$VENV/bin/alembic" -c alembic.ini current 2>/dev/null | grep -q "head"; then
  echo "【UniSSO】执行数据库迁移"
  "$VENV/bin/alembic" -c alembic.ini upgrade head || echo "【UniSSO】迁移可能已执行或数据库未就绪"
fi

# 3) 自动 HTTPS 配置（如果用户指定了 HTTPS_CERT 但未生成）
if [ -n "${HTTPS_CERT:-}" ] && [ -n "${HTTPS_KEY:-}" ]; then
  if [ ! -f "$HTTPS_CERT" ] || [ ! -f "$HTTPS_KEY" ]; then
    echo "【UniSSO】生成自签名 HTTPS 证书..."
    CERT_DIR="$(dirname "$HTTPS_CERT")"
    mkdir -p "$CERT_DIR"
    openssl req -x509 -newkey rsa:2048 \
      -keyout "$HTTPS_KEY" \
      -out "$HTTPS_CERT" \
      -days 365 \
      -nodes \
      -subj "/C=CN/ST=Guangdong/L=Guangzhou/O=UniSSO/CN=unisso.local" \
      -addext "subjectAltName=DNS:localhost,DNS:unisso.local,IP:127.0.0.1" \
      2>/dev/null || echo "【UniSSO】证书生成失败，将使用 HTTP"
    chmod 600 "$HTTPS_KEY" 2>/dev/null || true
    chmod 644 "$HTTPS_CERT" 2>/dev/null || true
  fi
fi

# 4) 启动应用（环境感知，动态 worker 数）
export PORT
export PYTHONPATH="${PYTHONPATH:-$(pwd)}"
echo "【UniSSO】启动服务，端口 $PORT"
exec "$VENV/bin/python" start.py
