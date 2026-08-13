#!/usr/bin/env bash
# Formats all C/C++ sources with clang-format.
#
# Usage: ./tools/format.sh
# Env:   CLANG_FORMAT=clang-format-18  (override the binary)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"

mapfile -t FILES < <(
  find "$ROOT/src" "$ROOT/tests" \
    \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' \) \
    | sort
)

"$CLANG_FORMAT" -i "${FILES[@]}"
echo "Formatted ${#FILES[@]} files with $CLANG_FORMAT."
