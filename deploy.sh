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

# Build the SPA when the bundle is missing or any frontend source (incl.
# frontend/public — llms.txt, robots.txt, index.html meta) is newer than it.
if [ -d "$ROOT/frontend" ]; then
    spa_stale=0
    if [ ! -f web/index.html ]; then
        spa_stale=1
    elif [ -n "$(find "$ROOT/frontend/src" "$ROOT/frontend/public" \
                   "$ROOT/frontend/index.html" "$ROOT/frontend/package.json" \
                   "$ROOT/frontend/vite.config.js" \
                   -newer web/index.html -print -quit 2>/dev/null)" ]; then
        spa_stale=1
    fi
    if [ "$spa_stale" = "1" ]; then
        if [ -s "$HOME/.nvm/nvm.sh" ]; then . "$HOME/.nvm/nvm.sh"; fi
        (cd "$ROOT/frontend" && npm install --no-audit --no-fund && npm run build)
    fi
fi

exec build/unisso_v2
