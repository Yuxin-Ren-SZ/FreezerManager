#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Install one CMake preset's Conan dependencies into out/conan/<preset>, from
# inside a worktree (see AGENTS.md §4):
#
#   scripts/agent/conan-install.sh <preset> [build_type]
#
#   preset      dev | asan | ubsan | tsan | coverage | release | ...
#               any configure preset defined in CMakePresets.json
#   build_type  Debug (default) | Release | RelWithDebInfo | MinSizeRel
#
# Why this script exists (#36): `conan install --output-folder=out/conan/<preset>`
# appends that folder to a single repository-root CMakeUserPresets.json, and
# Conan names its presets after the build type, so every Debug folder emits a
# preset called `conan-debug`. After a second install the aggregate defines that
# name twice and the next `cmake --preset asan` aborts with
#
#     CMake Error: Duplicate preset: "conan-debug"
#
# CMakePresets.json wires the Conan toolchain file in directly and never uses
# Conan's presets, so the aggregate is dead weight. This script therefore tells
# Conan not to write it:
#
#     -c tools.cmake.cmaketoolchain:user_presets=""
#
# (Conan 2.32 has no per-folder preset namespace — `CMakeToolchain.presets_prefix`
# is hardcoded to "conan" and is not a conf. `tools/run-tests.sh` already
# disables the aggregate the same way.)
#
# --build=never: never compile a dependency here. Missing packages are built
# once, by whoever holds `lock:deps` (AGENTS.md §3).
set -euo pipefail

die() {
  echo "conan-install.sh: $*" >&2
  exit 1
}

[ $# -ge 1 ] || die "usage: $0 <preset> [build_type]"
preset=$1
build_type=${2:-Debug}
[[ $preset =~ ^[a-z0-9][a-z0-9-]*$ ]] || die "preset must be kebab-case, got '$preset'"
[[ $build_type =~ ^(Debug|Release|RelWithDebInfo|MinSizeRel)$ ]] ||
  die "unknown build type '$build_type'"

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
top=$(git rev-parse --show-toplevel 2>/dev/null) || die "not inside a FreezerManager checkout"
cd "$top"

# Wire up the shared toolchain if the caller has not sourced env.sh already
# (AGENTS.md §1 tells agents to, so this is only for a bare invocation).
if [ -z "${CONAN_HOME:-}" ]; then
  # shellcheck source=scripts/agent/env.sh
  source "$script_dir/env.sh"
fi

# True when CMakeUserPresets.json is Conan's own aggregate: Conan marks the file
# with a "conan" vendor entry and refuses to overwrite one without it. We apply
# the same test before deleting, so a hand-written presets file is never lost.
is_conan_generated() {
  command -v python3 >/dev/null 2>&1 || return 1
  python3 - "$1" <<'PY'
import json
import sys

try:
    with open(sys.argv[1], encoding="utf-8") as handle:
        data = json.load(handle)
except (OSError, ValueError):
    sys.exit(1)
sys.exit(0 if "conan" in data.get("vendor", {}) else 1)
PY
}

# Worktrees created before this script existed still carry the stale aggregate;
# drop it so the workaround is never needed again, even in an old worktree.
if [ -f CMakeUserPresets.json ]; then
  if is_conan_generated CMakeUserPresets.json; then
    echo "conan-install.sh: dropping the stale Conan-generated CMakeUserPresets.json"
    rm -f CMakeUserPresets.json
  else
    echo "conan-install.sh: CMakeUserPresets.json is not Conan's — leaving it alone." >&2
    echo "  If 'cmake --preset' fails with Duplicate preset, remove it yourself." >&2
  fi
fi

log="$top/out/conan-install-$preset.log"
mkdir -p "$top/out/conan"
conan install . --lockfile=conan.lock --output-folder="out/conan/$preset" \
  --build=never -s "build_type=$build_type" -s compiler.cppstd=20 \
  -c 'tools.cmake.cmaketoolchain:user_presets=' >"$log" 2>&1 ||
  die "conan install failed (see $log). A missing package must be built by the lock:deps holder."

echo "conan-install.sh: out/conan/$preset ready ($build_type)"
echo "  next: cmake --preset $preset && cmake --build --preset $preset"
