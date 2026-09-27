#!/usr/bin/env bash
# static-analysis.yml：对 src/ 下的全部 TU 运行 clang-tidy。
#
# 该任务整体 continue-on-error（check 集合尚未稳定，见
# docs/development/coding-style.md）；这里把 clang-tidy 的完整输出 tee 到
# 日志并统计 warning 数量，便于直接看到规模。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

sudo ln -sf "$(command -v clang-tidy-18)" /usr/local/bin/clang-tidy
set +e
find src -name '*.cpp' -print0 | xargs -0 -n1 -P2 \
  clang-tidy -p build/debug -header-filter='neko/' 2>&1 | tee /tmp/clang-tidy.log
# 不管退出码如何都统计一次，保证日志里能看到告警总数。
grep -c "warning:" /tmp/clang-tidy.log || true
