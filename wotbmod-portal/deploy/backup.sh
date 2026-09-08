#!/usr/bin/env bash
# Consistent backup of the portal: the SQLite database (via the online backup
# API, safe while the server runs), the artifacts and the signing key.
#   deploy/backup.sh /opt/wotbmod-portal /var/backups/wotbmod
set -euo pipefail
ROOT="${1:-$(dirname "$0")/..}"
OUT="${2:-$ROOT/backups}"
STAMP="$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT/$STAMP"
python3 - "$ROOT/data/portal.sqlite3" "$OUT/$STAMP/portal.sqlite3" <<'EOF'
import sqlite3, sys
source = sqlite3.connect(sys.argv[1]); target = sqlite3.connect(sys.argv[2])
with target: source.backup(target)
source.close(); target.close()
EOF
tar -czf "$OUT/$STAMP/artifacts.tar.gz" -C "$ROOT/data" artifacts
cp "$ROOT/data/portal.key" "$OUT/$STAMP/portal.key"
chmod 600 "$OUT/$STAMP/portal.key"
echo "backup written to $OUT/$STAMP"
