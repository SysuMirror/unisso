#!/bin/sh
# UniSSO v2 runtime entry: build if needed, then run the C++ server in foreground.
set -eu
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT/backend"
# RECOVERY 2026-11-15: runtime libs on persistent disk; pod rebuild needs no apt
export LD_LIBRARY_PATH="$ROOT/libs/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"

# The platform environment takes precedence. Keep Pod-local writer IDs on the
# persistent disk when the platform does not supply this variable.
if [ "${UNISSO_DEVELOPER_WRITE_CLIENTS+x}" != x ] \
   && [ -f "$ROOT/backend/developer-write-clients.local" ]; then
    UNISSO_DEVELOPER_WRITE_CLIENTS=$(cat "$ROOT/backend/developer-write-clients.local")
    export UNISSO_DEVELOPER_WRITE_CLIENTS
fi

build_deps_ready() {
    cmake --version >/dev/null 2>&1 \
        && make --version >/dev/null 2>&1 \
        && printf '%s\n' '#include <mysql.h>' '#include <hiredis/hiredis.h>' \
             '#include <curl/curl.h>' '#include <httplib.h>' \
             '#include <openssl/ssl.h>' '#include <zlib.h>' '#include <crypt.h>' \
           | c++ -std=c++20 -I/usr/include/mysql -E -x c++ - >/dev/null 2>&1
}

# Build deps: prefer persisted .debs under $ROOT/devdebs (no network / apt).
ensure_build_deps() {
    if build_deps_ready; then
        return 0
    fi
    if [ -d "$ROOT/devdebs" ]; then
        for d in "$ROOT/devdebs"/*.deb; do
            [ -f "$d" ] || continue
            if ! sudo -n dpkg-deb -x "$d" / >/dev/null; then
                echo "[deploy] failed to restore dependency: $d" >&2
                return 1
            fi
        done
        sudo -n ldconfig >/dev/null 2>&1 || true
    fi
    build_deps_ready
}

NEED_BUILD=0
if [ ! -x build/unisso_v2 ]; then
    NEED_BUILD=1
elif [ -n "$(find src include main.cpp CMakeLists.txt -newer build/unisso_v2 -print -quit 2>/dev/null)" ]; then
    NEED_BUILD=1
fi

if [ "$NEED_BUILD" = "1" ]; then
    if ensure_build_deps; then
        # Link away from the last good executable; replace it only on success.
        mkdir -p build-next build
        if (cd build-next && cmake .. -DCMAKE_BUILD_TYPE=Release >/dev/null && make -j2) \
           && ldd build-next/unisso_v2 >/dev/null 2>&1 \
           && ! ldd build-next/unisso_v2 2>&1 | grep -q 'not found'; then
            cp build-next/unisso_v2 build/unisso_v2.next
            mv -f build/unisso_v2.next build/unisso_v2
        else
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
