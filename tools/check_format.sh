#!/usr/bin/env bash
# Verifies that all C/C++ sources are clang-format clean (CI gate).
#
# Usage: ./tools/check_format.sh
# Env:   CLANG_FORMAT=clang-format-18  (override the binary)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"

mapfile -t FILES < <(
  find "$ROOT/src" "$ROOT/tests" \
    \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' \) \
    | sort
)

status=0
for f in "${FILES[@]}"; do
  if ! "$CLANG_FORMAT" --dry-run --Werror "$f" >/dev/null 2>&1; then
    echo "Formatting error: ${f#"$ROOT/"}"
    status=1
  fi
done

if [[ $status -eq 0 ]]; then
  echo "All ${#FILES[@]} files are clang-format clean."
fi
exit "$status"
