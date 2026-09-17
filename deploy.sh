#!/bin/sh
# Runtime only. Build dependencies and migrate in explicit release steps.
set -eu
cd "$(dirname "$0")"
if [ ! -x .venv/bin/python ]; then
    python3 -m venv .venv
fi
# Sync deps when requirements.txt changes (first run installs everything).
req_hash=".venv/.requirements.sha256"
current_hash="$(sha256sum requirements.txt | cut -d' ' -f1)"
if [ ! -f "$req_hash" ] || [ "$(cat "$req_hash")" != "$current_hash" ]; then
    .venv/bin/pip install -q --disable-pip-version-check -r requirements.txt
    echo "$current_hash" > "$req_hash"
fi
exec .venv/bin/python start.py
