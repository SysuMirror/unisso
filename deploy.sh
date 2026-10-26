#!/bin/sh
# UniSSO v2 runtime entry: build if needed, then run the C++ server in foreground.
set -eu
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT/backend"

# Build dependencies live in the ephemeral container layer, not /home/cloud.
if ! command -v cmake >/dev/null 2>&1 \
   || [ ! -f /usr/include/mysql/mysql.h ] \
   || [ ! -f /usr/include/hiredis/hiredis.h ] \
   || [ ! -f /usr/include/curl/curl.h ] \
   || [ ! -f /usr/include/httplib.h ]; then
    sudo apt-get update -qq >/dev/null 2>&1 || true
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
        cmake libssl-dev default-libmysqlclient-dev libhiredis-dev zlib1g-dev \
        libcurl4-openssl-dev libcrypt-dev libcpp-httplib-dev >/dev/null 2>&1 || true
fi

# Rebuild when any backend source is newer than the binary.
if [ ! -x build/unisso_v2 ] || \
   [ -n "$(find src include main.cpp CMakeLists.txt -newer build/unisso_v2 -print -quit 2>/dev/null)" ]; then
    mkdir -p build
    (cd build && cmake .. -DCMAKE_BUILD_TYPE=Release >/dev/null && make -j2)
fi

# Build the SPA if the static bundle is missing.
if [ ! -f web/index.html ] && [ -d "$ROOT/frontend" ]; then
    if [ -s "$HOME/.nvm/nvm.sh" ]; then . "$HOME/.nvm/nvm.sh"; fi
    (cd "$ROOT/frontend" && npm install --no-audit --no-fund && npm run build)
fi

exec build/unisso_v2
