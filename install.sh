#!/bin/sh
# Rootless installation only; no sudo, autostart or firewall changes.
set -eu
if ! command -v python3 >/dev/null 2>&1; then
    printf '%s\n' 'Missing native dependency: python3 (>= 3.10). Install it yourself, then retry.' >&2
    exit 1
fi
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec python3 "$SCRIPT_DIR/install.py" "$@"
