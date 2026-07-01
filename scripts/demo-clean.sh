#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────────────────
# FreezerManager Demo Cleanup
#
#   bash scripts/demo-clean.sh         kill server + Qt client
#   bash scripts/demo-clean.sh --wipe  also delete the database
# ──────────────────────────────────────────────────────────────────────────────
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DB="$ROOT/data/freezer.db"

echo "Stopping FreezerManager processes..."
pkill -f freezerd          2>/dev/null || true
pkill -f freezermanager-qt 2>/dev/null || true
sleep 1
echo "  All stopped."

if [[ "${1:-}" == "--wipe" ]]; then
    rm -f "$DB" "$DB-wal" "$DB-shm"
    echo "  Database wiped."
fi
