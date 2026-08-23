#!/usr/bin/env bash
# UniSSO HTTPS 一键配置脚本
#
# 功能：
# 1. 生成自签名证书（用于本地测试或平台无反向代理时）
# 2. 自动检测平台是否提供 HTTPS 反向代理
# 3. 输出需要设置的环境变量
#
# 用法：
#   ./setup-https.sh          # 生成证书并输出配置
#   ./setup-https.sh --check  # 仅检测平台环境

set -euo pipefail

cd "$(dirname "$0")"

CERT_DIR="${CERT_DIR:-./certs}"
CERT_FILE="$CERT_DIR/cert.pem"
KEY_FILE="$CERT_DIR/key.pem"

# 颜色
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo "========================================"
echo "  UniSSO HTTPS 配置助手"
echo "========================================"
echo ""

# ==================== 场景检测 ====================

echo -e "${BLUE}[1/4] 检测部署场景...${NC}"

# 检测是否在 ssesinfra 平台
if [ -n "${DEPLOY_ID:-}" ] || [ -n "${PUBLIC_URL:-}" ]; then
    echo -e "${GREEN}✓ 检测到 ssesinfra 平台部署${NC}"
    PLATFORM="ssesinfra"
else
    echo -e "${YELLOW}⚠ 未检测到平台环境变量，可能是本地开发${NC}"
    PLATFORM="local"
fi

# 检测 PUBLIC_URL 是否以 https 开头
if [ -n "${PUBLIC_URL:-}" ]; then
    if echo "$PUBLIC_URL" | grep -q "^https://"; then
        echo -e "${GREEN}✓ 平台公网地址已启用 HTTPS: $PUBLIC_URL${NC}"
        HAS_PLATFORM_HTTPS="yes"
    else
        echo -e "${YELLOW}⚠ 平台公网地址未启用 HTTPS: $PUBLIC_URL${NC}"
        HAS_PLATFORM_HTTPS="no"
    fi
else
    HAS_PLATFORM_HTTPS="unknown"
fi

echo ""

# ==================== 证书生成 ====================

echo -e "${BLUE}[2/4] 检查证书...${NC}"

if [ -f "$CERT_FILE" ] && [ -f "$KEY_FILE" ]; then
    echo -e "${GREEN}✓ 已有证书:${NC}"
    echo "  证书: $CERT_FILE"
    echo "  密钥: $KEY_FILE"
    openssl x509 -in "$CERT_FILE" -noout -dates -subject 2>/dev/null | sed 's/^/  /' || true
else
    echo -e "${YELLOW}未找到证书，生成自签名证书...${NC}"
    mkdir -p "$CERT_DIR"

    # 生成证书（有效期 365 天）
    openssl req -x509 -newkey rsa:2048 \
        -keyout "$KEY_FILE" \
        -out "$CERT_FILE" \
        -days 365 \
        -nodes \
        -subj "/C=CN/ST=Guangdong/L=Guangzhou/O=UniSSO/CN=unisso.local" \
        -addext "subjectAltName=DNS:localhost,DNS:unisso.local,IP:127.0.0.1" \
        2>/dev/null

    chmod 600 "$KEY_FILE"
    chmod 644 "$CERT_FILE"

    echo -e "${GREEN}✓ 自签名证书已生成:${NC}"
    echo "  证书: $CERT_FILE"
    echo "  密钥: $KEY_FILE"
    echo ""
    echo -e "${YELLOW}注意：自签名证书会被浏览器标记为不安全，仅用于测试。${NC}"
    echo -e "${YELLOW}生产环境请使用 Let's Encrypt 或平台提供的证书。${NC}"
fi

echo ""

# ==================== 配置建议 ====================

echo -e "${BLUE}[3/4] 配置建议...${NC}"

echo ""
echo "根据你的场景，选择以下配置方式之一："
echo ""

if [ "$HAS_PLATFORM_HTTPS" = "yes" ]; then
    echo -e "${GREEN}【推荐】场景 A：平台已提供 HTTPS 反向代理${NC}"
    echo "平台（Nginx/Traefik）已处理 HTTPS，应用只需信任反向代理头。"
    echo ""
    echo "在「部署」页的环境变量中添加："
    echo ""
    echo "  TRUST_PROXY=true"
    echo "  FORCE_HTTPS=true"
    echo ""
    echo "这样应用会自动："
    echo "  - 识别 X-Forwarded-Proto: https 头"
    echo "  - 设置 Secure Cookie"
    echo "  - HTTP 请求自动 308 重定向到 HTTPS"
    echo ""

elif [ "$PLATFORM" = "ssesinfra" ] && [ "$HAS_PLATFORM_HTTPS" = "no" ]; then
    echo -e "${YELLOW}【场景 B：平台地址是 HTTP】${NC}"
    echo "平台公网地址当前是 HTTP，有两种选择："
    echo ""
    echo "  选项 B1：联系平台管理员开启 HTTPS（推荐）"
    echo "    平台层开启 HTTPS 后，按场景 A 配置即可。"
    echo ""
    echo "  选项 B2：应用自己监听 HTTPS（当前端口）"
    echo "    在「部署」页的环境变量中添加："
    echo ""
    echo "      HTTPS_CERT=/app/certs/cert.pem"
    echo "      HTTPS_KEY=/app/certs/key.pem"
    echo "      IS_HTTPS=true"
    echo ""
    echo "    然后在「构建命令」或部署前执行："
    echo "      ./setup-https.sh"
    echo ""

else
    echo -e "${GREEN}【场景 C：本地开发${NC}"
    echo ""
    echo "启动时指定证书路径："
    echo ""
    echo "  HTTPS_CERT=./certs/cert.pem \\"
    echo "  HTTPS_KEY=./certs/key.pem \\"
    echo "  UNISSO_SECRET_KEY=\"\$(openssl rand -base64 48)\" \\"
    echo "  python start.py"
    echo ""
    echo "或用 uvicorn 直接启动 HTTPS："
    echo ""
    echo "  uvicorn app.main:app \\"
    echo "    --host 0.0.0.0 \\"
    echo "    --port 8443 \\"
    echo "    --ssl-keyfile ./certs/key.pem \\"
    echo "    --ssl-certfile ./certs/cert.pem"
    echo ""
fi

# ==================== 验证命令 ====================

echo -e "${BLUE}[4/4] 验证命令...${NC}"
echo ""

if [ -f "$CERT_FILE" ]; then
    echo "测试证书："
    echo "  openssl x509 -in $CERT_FILE -text -noout | head -20"
    echo ""
    echo "测试 HTTPS 连接（应用启动后）："
    if command -v curl >/dev/null 2>&1; then
        echo "  curl -k -I https://localhost:8443/health"
    fi
    echo ""
fi

echo "========================================"
echo "  配置完成"
echo "========================================"
