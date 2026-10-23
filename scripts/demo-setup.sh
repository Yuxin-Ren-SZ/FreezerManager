#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# ──────────────────────────────────────────────────────────────────────────────
# FreezerManager Demo — one command that serves a seeded server AND the SPA.
#
#   bash scripts/demo-setup.sh              # start the demo
#   bash scripts/demo-setup.sh --reset      # start with a freshly seeded database
#   bash scripts/demo-setup.sh --totp-code  # print the current TOTP code for the
#                                           # demo admin, right before sign-in
#
# What it does:
#   1. Starts freezerd briefly on the demo database to run migrations, then
#      stops it again (only the server process it started — never a pkill).
#   2. On the first run (or with --reset) seeds demo data with the venv python
#      (argon2 required; the script fails loudly with the one-time fix when it
#      is missing, rather than half-running). Later runs reuse the database.
#   3. Gives the demo admin a fixed TOTP secret, so the demo shows the real
#      sign-in + second-factor flow.
#   4. Starts freezerd and the Vite dev server on ONE matching loopback port
#      pair: Vite proxies /api to freezerd's REST listener, and this script
#      exports the same FMGR_* variables both sides read (vite.config.ts and
#      freezerd), so they cannot disagree.
#   5. Starts the Qt client only if its binary exists — and only claims to
#      when it did.
#   6. Stays in the foreground; Ctrl-C stops exactly the processes it started.
#
# Prerequisite: the server binary must be built in this checkout first
# (`cmake --build --preset dev`); the script says so instead of building.
#
# Ports (loopback only, never 0.0.0.0): the demo defaults avoid the per-agent
# slot pattern (50051/18080/5173 + 10·N). When an agent has sourced
# scripts/agent/env.sh first, its slot's REST/web ports are used instead and
# the Vite proxy follows automatically.
#
# Nothing here deletes anything without --reset, and --reset names the exact
# files it deletes. Nothing here touches data/freezer.db, the main checkout's
# database, or any process this script did not start.
# ──────────────────────────────────────────────────────────────────────────────
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SERVER="$ROOT/out/build/dev/src/server/freezerd"
QT="$ROOT/out/build/dev/src/qt/freezermanager-qt"
WEB_DIR="$ROOT/src/web"
LOGDIR="$ROOT/out/demo"

# The main checkout owns the shared toolchain (.venv, Conan cache, Node 22).
# env.sh exports FMGR_MAIN_CHECKOUT, but this script must also work in a worktree
# when env.sh was never sourced, so derive it from git when the variable is
# absent. From a worktree, --git-common-dir is the main checkout's .git.
MAIN_CHECKOUT="${FMGR_MAIN_CHECKOUT:-}"
if [[ -z "$MAIN_CHECKOUT" ]]; then
  _common_dir="$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)"
  [[ -n "$_common_dir" ]] && MAIN_CHECKOUT="$(dirname "$_common_dir")"
  unset _common_dir
fi

# The demo database lives under out/ (gitignored, per-checkout), never in data/.
DB="${FMGR_DEMO_DB_PATH:-$ROOT/out/demo/freezer.db}"
SEED_MARKER="$DB.demo-seeded"
SEED_MARKER_VALUE="freezermanager-demo-seeded-v1"
case "$DB" in
  */data/freezer.db | data/freezer.db)
    printf '[demo] ERROR: refusing to use a data/freezer.db path as the demo database: %s\n' "$DB" >&2
    printf '  That is a checkout database, not the demo one. Point FMGR_DEMO_DB_PATH under out/.\n' >&2
    exit 1
    ;;
esac

# The demo admin's fixed TOTP secret (RFC 4648 base32; the RFC 6238 test
# vector). Fixed on purpose: a demo needs a code the presenter can reproduce.
DEMO_TOTP_SECRET="JBSWY3DPEHPK3PXP"
DEMO_ADMIN_EMAIL="admin@freezer.local"

# The demo ONLY PHI master key (base64 of 32 bytes). The seeded CSF `diagnosis`
# value is encrypted under it (#143), so the seed and freezerd must use the same
# key or the admin's detail page shows no PHI and the phi.read gate looks broken.
# Fixed on purpose, like the TOTP secret: a demo must survive --reset, restarts
# and a fresh shell. It encrypts synthetic data in out/demo/ only — never use it
# for real PHI, and generate a real key (openssl rand -base64 32) outside the
# demo. Exported before the migration/seed/server steps so all three inherit it.
export FMGR_MASTER_KEK='RnJlZXplck1hbmFnZXJEZW1vS2V5MzJCeXRlcyEhISE='

log() { printf '[demo] %s\n' "$*"; }
fail() {
  printf '[demo] ERROR: %s\n' "$*" >&2
  exit 1
}

# One consistent loopback port pair. Agent env (scripts/agent/env.sh) wins if
# present, so a worker's demo lands on its slot ports; otherwise the fixed demo
# ports. FMGR_WEB_DEV_PORT is the port Vite binds and FMGR_REST_LISTEN is where
# Vite proxies /api — they must agree, and here they always do because this
# script exports both for its children.
REST_LISTEN="${FMGR_REST_LISTEN:-127.0.0.1:18090}"
WEB_PORT="${FMGR_WEB_DEV_PORT:-5190}"
GRPC_LISTEN="${FMGR_LISTEN:-127.0.0.1:50090}"
REST_PORT="${REST_LISTEN##*:}"

# This is a demo on a shared machine: it must never be reachable off-box. The
# gateway and Vite bind what this script passes them, so refuse anything but a
# loopback listener rather than trusting the caller's environment.
case "$REST_LISTEN" in
  127.0.0.1:* | localhost:*) ;;
  *) fail "FMGR_REST_LISTEN must be a loopback listener (127.0.0.1:PORT), got: $REST_LISTEN" ;;
esac
case "$GRPC_LISTEN" in
  127.0.0.1:* | localhost:*) ;;
  *) fail "FMGR_LISTEN must be a loopback listener (127.0.0.1:PORT), got: $GRPC_LISTEN" ;;
esac
case "$WEB_PORT" in
  '' | *[!0-9]*) fail "FMGR_WEB_DEV_PORT must be a port number, got: $WEB_PORT" ;;
esac

# Browser sessions over plain http://127.0.0.1 need non-Secure cookies: Safari
# will not store a Secure cookie from http://127.0.0.1, which would make the
# sign-in look broken. freezerd refuses this flag in production, so a demo
# cannot silently turn it on there.
export FMGR_DEV_INSECURE_COOKIES=1

SERVER_PID=""
VITE_PID=""
QT_PID=""

cleanup() {
  local pid
  for pid in "$VITE_PID" "$QT_PID" "$SERVER_PID"; do
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
      log "stopping process $pid"
      kill "$pid" 2>/dev/null || true
    fi
  done
  sleep 1
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# ── Helpers ────────────────────────────────────────────────────────────────

find_seed_python() {
  # The seed needs argon2 (argon2-cffi), which the system python3 (3.9.6) does
  # not have. The main checkout's venv does; scripts/agent/env.sh exports
  # FMGR_MAIN_CHECKOUT when sourced, and the block above derives it otherwise,
  # so this works in a worktree even without env.sh.
  if [[ -n "$MAIN_CHECKOUT" && -x "$MAIN_CHECKOUT/.venv/bin/python3" ]]; then
    printf '%s\n' "$MAIN_CHECKOUT/.venv/bin/python3"
    return 0
  fi
  if [[ -x "$ROOT/.venv/bin/python3" ]]; then
    printf '%s\n' "$ROOT/.venv/bin/python3"
    return 0
  fi
  printf '%s\n' "$(command -v python3 || true)"
}

verify_seed_python() {
  local py="$1"
  [[ -n "$py" && -x "$py" ]] || fail "no python3 found; create a venv: python3 -m venv .venv"
  if ! "$py" -c 'import argon2' 2>/dev/null; then
    fail "seed python ($py) has no argon2 — the seed would half-run without it.
  One-time fix:  <main checkout>/.venv/bin/pip install argon2-cffi
  (scripts/agent/env.sh already puts that venv first on PATH.)"
  fi
}

port_in_use() {
  # macOS `lsof` form; a non-empty answer means something is listening.
  lsof -nP -iTCP:"$1" -sTCP:LISTEN 2>/dev/null
}

find_freezerctl() {
  local candidate
  if [[ -n "${FMGR_FREEZERCTL:-}" ]]; then
    candidate="$FMGR_FREEZERCTL"
  elif [[ -x "$ROOT/out/build/dev/src/cli/freezerctl" ]]; then
    candidate="$ROOT/out/build/dev/src/cli/freezerctl"
  else
    candidate="$(command -v freezerctl || true)"
  fi
  [[ -n "$candidate" && -f "$candidate" && -x "$candidate" ]] \
    || fail "freezerctl not found or not executable: ${candidate:-<not found>}
  Build it first:  cmake --build --preset dev
  or point FMGR_FREEZERCTL at the binary."
  printf '%s\n' "$candidate"
}

ensure_child_alive() {
  local child_pid="$1" what="$2" logfile="$3" status
  if ! kill -0 "$child_pid" 2>/dev/null; then
    if wait "$child_pid"; then status=0; else status=$?; fi
    fail "$what exited before becoming ready (exit $status; see $logfile)"
  fi
}

wait_for_url() {
  local url="$1" what="$2" child_pid="$3" logfile="$4" tries=30
  for _ in $(seq 1 "$tries"); do
    ensure_child_alive "$child_pid" "$what" "$logfile"
    if curl -sf --max-time 2 "$url" >/dev/null 2>&1; then
      ensure_child_alive "$child_pid" "$what" "$logfile"
      return 0
    fi
    sleep 1
  done
  fail "$what did not become ready in time at $url"
}

ensure_port_free() {
  local port="$1" who="$2"
  if port_in_use "$port" >/dev/null; then
    fail "port $port is already in use ($who would land on it).
  Find the holder with:  lsof -nP -iTCP:$port -sTCP:LISTEN
  This script never kills other processes — stop the holder yourself, or run
  with FMGR_REST_LISTEN / FMGR_WEB_DEV_PORT / FMGR_LISTEN set to free ports."
  fi
}

find_node() {
  # Node 22 is pinned by the repo's .nvmrc; this machine keeps it under
  # out/tools/node22 (installed in the main checkout, shared by worktrees).
  local node_dir
  for node_dir in "$ROOT/out/tools/node22/bin" "$MAIN_CHECKOUT/out/tools/node22/bin"; do
    if [[ -n "$node_dir" && -x "$node_dir/node" ]]; then
      export PATH="$node_dir:$PATH"
      break
    fi
  done
  command -v node >/dev/null 2>&1 || fail "no node on PATH (need Node 22: see $ROOT/.nvmrc)"
  # An inherited NODE_ENV=production makes npm skip devDependencies and Vite
  # fail to start (doc/dev/web.md). Never run the dev server under it.
  unset NODE_ENV
}

# ── --totp-code: print the current code for the demo admin ─────────────────

if [[ "${1:-}" == "--totp-code" ]]; then
  PY="$(find_seed_python)"
  [[ -n "$PY" && -x "$PY" ]] || PY="$(command -v python3 || true)"
  [[ -n "$PY" && -x "$PY" ]] || fail "no python3 found"
  "$PY" - "$DEMO_TOTP_SECRET" <<'PYEOF'
import base64
import hashlib
import hmac
import struct
import sys
import time

secret = sys.argv[1]
key = base64.b32decode(secret + "=" * ((8 - len(secret) % 8) % 8))
counter = int(time.time()) // 30
digest = hmac.new(key, struct.pack(">Q", counter), hashlib.sha1).digest()
offset = digest[-1] & 0x0F
code = (struct.unpack(">I", digest[offset:offset + 4])[0] & 0x7FFFFFFF) % 10**6
print(f"{code:06d}")
PYEOF
  exit 0
fi

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  printf 'Usage: bash scripts/demo-setup.sh [--reset] [--totp-code]\n'
  exit 0
fi

RESET=0
if [[ "${1:-}" == "--reset" ]]; then
  RESET=1
elif [[ -n "${1:-}" ]]; then
  fail "unknown argument: $1 (use --reset or --totp-code)"
fi

# ── 0. Preflight: ports, binary, python, node ──────────────────────────────

log "demo database: $DB"
command -v lsof >/dev/null 2>&1 || fail "lsof is required to verify demo ports are free; install lsof and retry"
ensure_port_free "$REST_PORT" "freezerd"
ensure_port_free "$WEB_PORT" "the Vite dev server"
ensure_port_free "${GRPC_LISTEN##*:}" "freezerd gRPC"

[[ -x "$SERVER" ]] || fail "server binary not built: $SERVER
  Build it first in this checkout:  cmake --build --preset dev"

PY="$(find_seed_python)"
verify_seed_python "$PY"
FREEZERCTL="$(find_freezerctl)"
export FMGR_FREEZERCTL="$FREEZERCTL"
find_node
log "seed python: $PY"
log "freezerctl: $FREEZERCTL"
log "node: $(command -v node) ($(node --version))"

mkdir -p "$LOGDIR"

# ── 1. Fresh database (opt-in reset only) ──────────────────────────────────

# Decided before migrations run: the server creates the file, so testing for it
# afterwards would always say "it already existed" and skip the seed.
FRESH=0
if [[ "$RESET" == 1 && ( -e "$DB" || -e "$SEED_MARKER" ) ]]; then
  log "reset requested — deleting exactly:"
  log "  $DB"
  log "  $DB-wal"
  log "  $DB-shm"
  log "  $SEED_MARKER"
  rm -f "$DB" "$DB-wal" "$DB-shm" "$SEED_MARKER"
fi

LEGACY_SEED=0
if [[ -e "$DB" && "$RESET" != 1 ]]; then
  MARKER_CONTENT=""
  if [[ -f "$SEED_MARKER" ]]; then
    IFS= read -r MARKER_CONTENT < "$SEED_MARKER" || true
  fi
  if [[ "$MARKER_CONTENT" == "$SEED_MARKER_VALUE" ]]; then
    log "demo seed completion marker found"
  elif "$PY" - "$DB" <<'PYEOF'
import sqlite3
import sys

# LEGACY_DEMO_SEED_CHECK
conn = sqlite3.connect(sys.argv[1])
try:
    user_count = conn.execute(
        "SELECT count(*) FROM users WHERE id = '00000000-0000-b001-0000-000000000001'"
    ).fetchone()[0]
    lab_count = conn.execute(
        "SELECT count(*) FROM labs WHERE id IN ("
        "'00000000-0000-a001-0000-000000000001', "
        "'00000000-0000-a001-0000-000000000002')"
    ).fetchone()[0]
    neuro_samples = conn.execute(
        "SELECT count(*) FROM samples WHERE lab_id = '00000000-0000-a001-0000-000000000001'"
    ).fetchone()[0]
    onco_samples = conn.execute(
        "SELECT count(*) FROM samples WHERE lab_id = '00000000-0000-a001-0000-000000000002'"
    ).fetchone()[0]
    complete = user_count == 1 and lab_count == 2 and neuro_samples >= 20 and onco_samples >= 12
except sqlite3.Error:
    complete = False
finally:
    conn.close()
sys.exit(0 if complete else 1)
PYEOF
  then
    LEGACY_SEED=1
    log "complete legacy demo sample coverage found; a completion marker will be added after TOTP setup"
  else
    fail "existing demo database is incomplete or its seed state is unknown; it was left untouched. Review or back it up, then run with --reset to replace it with a complete demo database."
  fi
fi

if [[ -e "$DB" ]]; then
  log "reusing the existing demo database (no --reset given; nothing deleted)"
else
  FRESH=1
  log "no demo database yet — migrations and seeding will run"
fi

# ── 2. Migrations: start freezerd, wait for health, stop it ────────────────

export FMGR_DB_PATH="$DB"
export FMGR_LISTEN="$GRPC_LISTEN"
export FMGR_REST_LISTEN="$REST_LISTEN"

"$SERVER" >>"$LOGDIR/freezerd-migrate.log" 2>&1 &
SERVER_PID=$!
wait_for_url "http://$REST_LISTEN/api/v1/health" "freezerd (migration phase)" "$SERVER_PID" "$LOGDIR/freezerd-migrate.log"
kill "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""
log "migrations applied"

# ── 3. Seed demo data (venv python with argon2) ────────────────────────────

if [[ "$FRESH" == 1 ]]; then
  log "seeding demo data..."
  FREEZER_DB_PATH="$DB" FMGR_FREEZERCTL="$FREEZERCTL" "$PY" "$ROOT/scripts/seed_demo.py"
else
  log "database already seeded; skipping seed (use --reset for a fresh one)"
fi

# Give the demo admin a fixed TOTP secret so the demo shows the real MFA flow
# (a user without a secret signs in with the password alone). Run on every
# start: it is idempotent, and an older demo database may predate the secret.
"$PY" - "$DB" "$DEMO_TOTP_SECRET" "$DEMO_ADMIN_EMAIL" <<'PYEOF'
import sqlite3
import sys

db_path, secret, email = sys.argv[1], sys.argv[2], sys.argv[3]
conn = sqlite3.connect(db_path)
cur = conn.execute(
    "UPDATE users SET totp_secret_enc = ? WHERE primary_email = ?",
    (secret, email),
)
conn.commit()
if cur.rowcount != 1:
    sys.exit(f"expected exactly one {email} row to enable TOTP on, updated {cur.rowcount}")
print(f"  TOTP enabled for {email} ({cur.rowcount} row)")
PYEOF

# Mark only after both the seed and the idempotent TOTP update completed. Legacy
# markerless databases are marked after validating their fixed seeded identities.
if [[ "$FRESH" == 1 || "$LEGACY_SEED" == 1 ]]; then
  marker_tmp="$SEED_MARKER.tmp.$$"
  printf '%s\n' "$SEED_MARKER_VALUE" > "$marker_tmp"
  mv -f "$marker_tmp" "$SEED_MARKER"
  log "demo seed completion marker written: $SEED_MARKER"
fi

# ── 4. freezerd, for real this time ────────────────────────────────────────

"$SERVER" >>"$LOGDIR/freezerd.log" 2>&1 &
SERVER_PID=$!
wait_for_url "http://$REST_LISTEN/api/v1/health" "freezerd" "$SERVER_PID" "$LOGDIR/freezerd.log"
log "freezerd: gRPC $GRPC_LISTEN, REST http://$REST_LISTEN (pid $SERVER_PID)"

# ── 5. Qt client — only if it is actually built ────────────────────────────

if [[ -x "$QT" ]]; then
  "$QT" >>"$LOGDIR/qt.log" 2>&1 &
  QT_PID=$!
  log "Qt client: started (pid $QT_PID)"
else
  log "Qt client: not started — $QT does not exist (Qt6 is not installed on this machine)"
fi

# ── 6. Vite dev server (proxies /api to $REST_LISTEN) ──────────────────────

if [[ ! -d "$WEB_DIR/node_modules" ]]; then
  log "installing web dependencies (npm ci, first run only)..."
  (cd "$WEB_DIR" && npm ci --cache "$ROOT/out/npm-cache" >>"$LOGDIR/npm-ci.log" 2>&1)
fi

# Generate the proto/icon inputs once, then run Vite directly instead of through
# `npm run dev`: the PID this script records is then the process that owns the
# port, so Ctrl-C cannot leave an orphaned Vite behind.
(cd "$WEB_DIR" && npm run gen >>"$LOGDIR/vite.log" 2>&1) || fail "npm run gen failed (see $LOGDIR/vite.log)"
export FMGR_WEB_DEV_PORT="$WEB_PORT"
(cd "$WEB_DIR" && exec env NODE_ENV=development ./node_modules/.bin/vite >>"$LOGDIR/vite.log" 2>&1) &
VITE_PID=$!
wait_for_url "http://127.0.0.1:$WEB_PORT/" "the Vite dev server" "$VITE_PID" "$LOGDIR/vite.log"
# The proxy must answer too — this check is what catches a port disagreement
# between the script and vite.config.ts.
ensure_child_alive "$SERVER_PID" "freezerd" "$LOGDIR/freezerd.log"
ensure_child_alive "$VITE_PID" "the Vite dev server" "$LOGDIR/vite.log"
PROXIED_HEALTH="$(curl -sf --max-time 5 "http://127.0.0.1:$WEB_PORT/api/v1/health" || true)"
ensure_child_alive "$SERVER_PID" "freezerd" "$LOGDIR/freezerd.log"
ensure_child_alive "$VITE_PID" "the Vite dev server" "$LOGDIR/vite.log"
if [[ "$PROXIED_HEALTH" != *'"status":"ok"'* ]]; then
  echo >&2
  fail "Vite is up but /api/v1/health through the proxy failed — the /api proxy
  target ($REST_LISTEN) does not match the server. Check $LOGDIR/vite.log."
fi
echo
log "SPA: http://127.0.0.1:$WEB_PORT (Vite, pid $VITE_PID) — /api proxied to $REST_LISTEN"

# ── 7. Summary ─────────────────────────────────────────────────────────────

cat <<EOF

============================================================
 Demo is ready:  http://127.0.0.1:$WEB_PORT
============================================================
 Sign in (TOTP is enabled for the SystemAdmin):

   email:    $DEMO_ADMIN_EMAIL
   password: admin
   TOTP:     bash scripts/demo-setup.sh --totp-code
             (or scan into any authenticator app:
              otpauth://totp/FreezerManager%20Demo:$DEMO_ADMIN_EMAIL?secret=$DEMO_TOTP_SECRET&issuer=FreezerManager%20Demo&algorithm=SHA1&digits=6&period=30)

 Other users (password-only sign-in, all password: admin):
   wang@neuro.local    (Neuro LabAdmin)
   chen@onco.local     (Onco LabAdmin)
   li@neuro.local      (Neuro Member)
   zhang@neuro.local   (ReadOnly)

 Database: $DB   (--reset deletes exactly this file before seeding)
 Logs:     $LOGDIR/{freezerd,vite}.log
 Stop:     press Ctrl-C here — only the processes started above are stopped.
============================================================
EOF

# Stay in the foreground; Ctrl-C runs cleanup and stops exactly our PIDs.
wait "$SERVER_PID" "$VITE_PID"
