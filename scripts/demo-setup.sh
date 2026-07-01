#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────────────────
# FreezerManager Demo Setup
#
# One command to set up everything:
#   bash scripts/demo-setup.sh
#
# What it does:
#   1. Kill any running freezerd / Qt client
#   2. Wipe old DB
#   3. Start freezerd briefly to run migrations, then kill it
#   4. Seed demo data
#   5. Start freezerd on localhost:8080
#   6. Start Qt client
# ──────────────────────────────────────────────────────────────────────────────
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DB="$ROOT/data/freezer.db"
SERVER="$ROOT/out/build/dev/src/server/freezerd"
QT="$ROOT/out/build/dev/src/qt/freezermanager-qt"
SEED="python3 $ROOT/scripts/seed_demo.py"

echo "============================================"
echo " FreezerManager Demo Setup"
echo "============================================"

# ── 1. Kill existing processes ──
echo "[1/5] Stopping existing processes..."
pkill -f freezerd     2>/dev/null || true
pkill -f freezermanager-qt 2>/dev/null || true
sleep 1

# ── 2. Wipe old DB ──
echo "[2/5] Wiping old database..."
rm -f "$DB" "$DB-wal" "$DB-shm"

# ── 3. Start server to migrate, then kill ──
echo "[3/5] Running migrations..."
FMGR_DB_PATH="$DB" FMGR_REST_LISTEN=127.0.0.1:8081 "$SERVER" &
SERVER_PID=$!
sleep 3

# Verify server is healthy
if ! curl -sf http://127.0.0.1:8081/api/v1/health > /dev/null 2>&1; then
    echo "ERROR: server failed to start for migration"
    kill $SERVER_PID 2>/dev/null || true
    exit 1
fi

kill $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true
echo "  Migrations applied."

# ── 4. Seed demo data ──
echo "[4/5] Seeding demo data..."
$SEED

# ── 5. Start server + Qt client ──
echo "[5/5] Starting server + Qt client..."

# Start server on port 8080
FMGR_DB_PATH="$DB" FMGR_REST_LISTEN=127.0.0.1:8080 "$SERVER" &
sleep 2

# Verify
if curl -sf http://127.0.0.1:8080/api/v1/health > /dev/null 2>&1; then
    echo "  Server: http://127.0.0.1:8080 ✓"
else
    echo "  WARNING: server health check failed"
fi

# Start Qt client
QT_QPA_PLATFORM=xcb "$QT" &
echo "  Qt client: started"

echo
echo "============================================"
echo " Demo is ready!"
echo ""
echo " Login:  admin@freezer.local"
echo " Pass:   admin"
echo ""
echo " Other users (all password: admin):"
echo "   wang@neuro.local    (Neuro LabAdmin)"
echo "   chen@onco.local     (Onco LabAdmin)"
echo "   li@neuro.local      (Neuro Member)"
echo "   zhang@neuro.local   (ReadOnly)"
echo "============================================"
