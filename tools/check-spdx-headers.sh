#!/usr/bin/env bash
set -euo pipefail

root="$(git rev-parse --show-toplevel)"
cd "$root"

status=0

check_file() {
  local file="$1"
  local expected="$2"

  if [[ ! -s "$file" ]]; then
    return
  fi

  if ! head -n 5 "$file" | grep -Fqx "$expected"; then
    printf 'Missing SPDX header in %s: expected "%s"\n' "$file" "$expected" >&2
    status=1
  fi
}

while IFS= read -r -d '' file; do
  case "$file" in
    *.py)
      check_file "$file" "# SPDX-License-Identifier: AGPL-3.0-or-later"
      ;;
    *.c|*.cc|*.cpp|*.cxx|*.h|*.hh|*.hpp|*.hxx|*.ipp)
      check_file "$file" "// SPDX-License-Identifier: AGPL-3.0-or-later"
      ;;
  esac
done < <(
  git ls-files -z --cached --others --exclude-standard -- \
    '*.py' '*.c' '*.cc' '*.cpp' '*.cxx' '*.h' '*.hh' '*.hpp' '*.hxx' '*.ipp'
)

exit "$status"
