#!/usr/bin/env bash
set -euo pipefail

RELEASE=/home/cloud/releases/unisso-email-registration-20260908

export UNISSO_DEBUG=false
export UNISSO_REDIS_MEMORY_MODE=false
export UNISSO_ISSUER=https://sso.ssemarket.cn
export UNISSO_JWT_ALGORITHM=RS256
export UNISSO_SIGNING_PRIVATE_KEY_FILE="$RELEASE/secrets/signing-private.pem"
export UNISSO_SIGNING_PUBLIC_KEY_FILE="$RELEASE/secrets/signing-public.pem"
export UNISSO_SIGNING_KEY_ID=unisso-2026-09-08
export UNISSO_TRUSTED_PROXY_CIDRS='["10.42.0.1/32"]'
export UNISSO_ALLOWED_HOSTS='["sso.ssemarket.cn"]'
export UNISSO_CSRF_TRUSTED_ORIGINS='["https://sso.ssemarket.cn"]'
export UNISSO_CORS_ORIGINS='[]'
export FORCE_HTTPS=false

cd "$RELEASE"
exec ./deploy.sh
