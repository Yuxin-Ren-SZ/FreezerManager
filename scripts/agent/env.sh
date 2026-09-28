# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Per-agent environment for working in a FreezerManager worktree (or the main
# checkout). Source it from anywhere inside the checkout, in bash or zsh:
#
#   AGENT_SLOT=2 source scripts/agent/env.sh
#
# AGENT_SLOT: 0 = lead / main checkout (default), N = worker-N.
# See AGENTS.md → "Running several agents on one machine" for why each
# variable exists.

if ! _fmgr_top=$(git rev-parse --show-toplevel 2>/dev/null); then
  echo "env.sh: not inside a FreezerManager checkout" >&2
  return 1
fi
_fmgr_main=$(dirname "$(git rev-parse --path-format=absolute --git-common-dir)")

export FMGR_MAIN_CHECKOUT="$_fmgr_main"
export FMGR_WORKTREE="$_fmgr_top"

# Toolchain: share the main checkout's Python venv (conan/cmake/ninja) and its
# Conan package cache, so dependencies are built once, not once per worktree.
if [ -d "$_fmgr_main/.venv/bin" ]; then
  case ":$PATH:" in
    *":$_fmgr_main/.venv/bin:"*) ;;
    *) export PATH="$_fmgr_main/.venv/bin:$PATH" ;;
  esac
fi
if [ -d "$_fmgr_main/.conan" ]; then
  export CONAN_HOME="$_fmgr_main/.conan"
fi

# Tests write SQLite files into the temp dir under fixed-pattern names. A
# private temp dir per worktree stops agents from corrupting each other's runs.
export TMPDIR="$_fmgr_top/out/tmp"
mkdir -p "$TMPDIR"

# Share the machine's RAM between agents: -j3 builds, serial test runs (the
# integration tests are not parallel-safe within one worktree yet).
export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-3}"
export CTEST_PARALLEL_LEVEL=1

# Per-slot ports and database for running freezerd by hand, so demo servers of
# different agents never collide. Loopback only.
export AGENT_SLOT="${AGENT_SLOT:-0}"
export FMGR_LISTEN="127.0.0.1:$((50051 + 10 * AGENT_SLOT))"
export FMGR_REST_LISTEN="127.0.0.1:$((18080 + 10 * AGENT_SLOT))"
# Vite dev server for src/web (G-arch 12), same slot pattern as the ports above.
# `vite.config.ts` reads both of these; see doc/dev/web.md.
export FMGR_WEB_DEV_PORT="$((5173 + 10 * AGENT_SLOT))"
export FMGR_DB_PATH="${FMGR_DB_PATH:-$_fmgr_top/out/freezer.db}"

echo "FreezerManager env: slot $AGENT_SLOT, worktree $_fmgr_top"
echo "  CONAN_HOME=${CONAN_HOME:-<default>}  TMPDIR=$TMPDIR"
echo "  gRPC $FMGR_LISTEN  REST $FMGR_REST_LISTEN  build -j$CMAKE_BUILD_PARALLEL_LEVEL"
echo "  web dev http://127.0.0.1:$FMGR_WEB_DEV_PORT (proxies /api to $FMGR_REST_LISTEN)"

unset _fmgr_top _fmgr_main
