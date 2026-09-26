#!/usr/bin/env bash
# CI（ci.yml）格式门禁：把 runner 上的 clang-format-18 暴露为 clang-format，
# 再跑仓库自己的检查脚本（tools/check_format.sh，支持 CLANG_FORMAT 覆盖）。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

sudo ln -sf "$(command -v clang-format-18)" /usr/local/bin/clang-format
./tools/check_format.sh
