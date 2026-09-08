#!/usr/bin/env bash
# WotbMod portal, local run on Linux/macOS (see run.cmd for Windows).
set -euo pipefail
cd "$(dirname "$0")"
command -v python3 >/dev/null || { echo "Python 3.11+ is required"; exit 1; }
[ -f config.json ] || { cp config.example.json config.json; echo "Created config.json; change admin_password before hosting."; }
mkdir -p data
[ -f data/portal.key ] || python3 tools/portal_keygen.py --out data/portal.key --key-id blitzforge-portal-2026
python3 backend/portal_server.py init
if [ "${1:-}" = "seed" ]; then python3 seed_demo.py; fi
exec python3 backend/portal_server.py serve "${@:2}"
