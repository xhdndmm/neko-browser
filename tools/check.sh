#!/usr/bin/env bash
# One-shot project health check: format, configure, build, test.
#
# Usage: ./tools/check.sh [preset]   (default: debug)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PRESET="${1:-debug}"

cd "$ROOT"

echo "==> Format check"
if command -v clang-format >/dev/null 2>&1; then
  ./tools/check_format.sh
else
  echo "    clang-format not installed; skipping format check."
fi

echo "==> Configure ($PRESET)"
cmake --preset "$PRESET"

echo "==> Build ($PRESET)"
cmake --build --preset "$PRESET"

echo "==> Test ($PRESET)"
ctest --preset "$PRESET"

echo "==> All checks passed ($PRESET)"
