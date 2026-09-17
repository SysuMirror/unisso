#!/bin/sh
# Runtime entry for yatterra deploys: build/sync venv, then start the app.
# Signing keys live in ./secrets (NOT committed); provision them on first run.
set -eu
cd "$(dirname "$0")"

if [ ! -x .venv/bin/python ]; then
    python3 -m venv .venv
fi
# Sync deps when requirements.txt changes (first run installs everything).
req_hash=".venv/.requirements.sha256"
current_hash="$(sha256sum requirements.txt | cut -d' ' -f1)"
if [ ! -f "$req_hash" ] || [ "$(cat "$req_hash")" != "$current_hash" ]; then
    .venv/bin/pip install -q --disable-pip-version-check \
        -i https://mirrors.aliyun.com/pypi/simple/ -r requirements.txt
    echo "$current_hash" > "$req_hash"
fi

# Production identity/env (mirrors start-production.sh, portable paths)
export UNISSO_DEBUG=false
export UNISSO_REDIS_MEMORY_MODE=false
export UNISSO_ISSUER="${UNISSO_ISSUER:-https://sso.ssemarket.cn}"
export UNISSO_JWT_ALGORITHM="${UNISSO_JWT_ALGORITHM:-RS256}"
export UNISSO_SIGNING_PRIVATE_KEY_FILE="${UNISSO_SIGNING_PRIVATE_KEY_FILE:-$PWD/secrets/signing-private.pem}"
export UNISSO_SIGNING_PUBLIC_KEY_FILE="${UNISSO_SIGNING_PUBLIC_KEY_FILE:-$PWD/secrets/signing-public.pem}"
export UNISSO_SIGNING_KEY_ID="${UNISSO_SIGNING_KEY_ID:-unisso-2026-09-08}"
export UNISSO_TRUSTED_PROXY_CIDRS='["10.42.0.1/32"]'
export UNISSO_ALLOWED_HOSTS='["sso.ssemarket.cn"]'
export UNISSO_CSRF_TRUSTED_ORIGINS='["https://sso.ssemarket.cn"]'
export UNISSO_CORS_ORIGINS='[]'
export FORCE_HTTPS=false

exec .venv/bin/python start.py
