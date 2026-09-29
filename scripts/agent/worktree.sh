#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Create an isolated worktree + branch for one GitHub issue and configure its
# dev build against the shared Conan cache.
#
#   scripts/agent/worktree.sh <issue> <slug> [type]
#
#   issue  GitHub issue number, e.g. 42
#   slug   short kebab-case name, e.g. sse-sample-watch
#   type   feat | fix | docs | test | refactor | ci | chore   (default: feat)
#
# Result: branch <type>/<issue>-<slug> cut from origin/main, checked out at
# <main checkout>/.worktrees/<issue>-<slug>, with out/build/dev configured.
# Remove when the PR is merged:
#   git worktree remove .worktrees/<issue>-<slug> && git branch -D <branch>
set -euo pipefail

die() {
  echo "worktree.sh: $*" >&2
  exit 1
}

[ $# -ge 2 ] || die "usage: $0 <issue> <slug> [feat|fix|docs|test|refactor|ci|chore]"
issue=$1
slug=$2
type=${3:-feat}
[[ $issue =~ ^[0-9]+$ ]] || die "issue must be a number, got '$issue'"
[[ $slug =~ ^[a-z0-9][a-z0-9-]*$ ]] || die "slug must be kebab-case, got '$slug'"
[[ $type =~ ^(feat|fix|docs|test|refactor|ci|chore)$ ]] || die "unknown type '$type'"

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
main=$(dirname "$(git rev-parse --path-format=absolute --git-common-dir)")
branch="$type/$issue-$slug"
wt="$main/.worktrees/$issue-$slug"

[ ! -e "$wt" ] || die "$wt already exists"
git -C "$main" rev-parse --verify --quiet "refs/heads/$branch" >/dev/null &&
  die "branch $branch already exists"

git -C "$main" fetch --quiet origin main
git -C "$main" worktree add --quiet -b "$branch" "$wt" origin/main
cd "$wt"

# shellcheck source=scripts/agent/env.sh
source "$script_dir/env.sh"

# --build=never: never compile dependencies here. If a package is missing, the
# lock:deps holder (or the lead) builds it once in the shared cache. The helper
# also keeps Conan from writing a repository-root CMakeUserPresets.json, which
# is what made `cmake --preset asan` fail after a second install (#36).
"$script_dir/conan-install.sh" dev ||
  die "conan install for dev failed (see $wt/out/conan-install-dev.log)"
cmake --preset dev >out/cmake-configure.log 2>&1 ||
  die "cmake configure failed (see $wt/out/cmake-configure.log)"

cat <<EOF

Worktree ready.
  path:   $wt
  branch: $branch
Next:
  cd $wt
  AGENT_SLOT=<N> source scripts/agent/env.sh
  cmake --build --preset dev && ctest --preset dev
EOF
