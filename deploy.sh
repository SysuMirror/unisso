#!/bin/sh
# Runtime only. Build dependencies and migrate in explicit release steps.
set -eu
cd "$(dirname "$0")"
if [ ! -x .venv/bin/python ]; then
    echo 'Missing release environment; install the locked dependencies before deployment.' >&2
    exit 1
fi
exec .venv/bin/python start.py
