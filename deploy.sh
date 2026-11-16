#!/bin/sh
# UniSSO v2 runtime entry: build if needed, then run the C++ server in foreground.
set -eu
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT/backend"
# RECOVERY 2026-11-15: runtime libs on persistent disk; pod rebuild needs no apt
 export LD_LIBRARY_PATH=/home/cloud/deploy/unisso/libs/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}

# Build deps: prefer persisted .debs under $ROOT/devdebs (no network / apt).
ensure_build_deps() {
    if command -v cmake >/dev/null 2>&1 \
       && [ -f /usr/include/mysql/mysql.h ] \
       && [ -f /usr/include/hiredis/hiredis.h ] \
       && [ -f /usr/include/curl/curl.h ] \
       && [ -f /usr/include/httplib.h ]; then
        return 0
    fi
    if [ -d "$ROOT/devdebs" ]; then
        for d in "$ROOT/devdebs"/*.deb; do
            [ -f "$d" ] || continue
            sudo dpkg-deb -x "$d" / >/dev/null 2>&1 || true
        done
        sudo ldconfig >/dev/null 2>&1 || true
    fi
    command -v cmake >/dev/null 2>&1
}

NEED_BUILD=0
if [ ! -x build/unisso_v2 ]; then
    NEED_BUILD=1
elif [ -n "$(find src include main.cpp CMakeLists.txt -newer build/unisso_v2 -print -quit 2>/dev/null)" ]; then
    NEED_BUILD=1
fi

if [ "$NEED_BUILD" = "1" ]; then
    if ensure_build_deps; then
        mkdir -p build
        if ! (cd build && cmake .. -DCMAKE_BUILD_TYPE=Release >/dev/null && make -j2); then
            echo "[deploy] build failed; keeping existing binary if present" >&2
            [ -x build/unisso_v2 ] || exit 1
        fi
    else
        echo "[deploy] build deps unavailable; keeping existing binary if present" >&2
        [ -x build/unisso_v2 ] || exit 1
    fi
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
