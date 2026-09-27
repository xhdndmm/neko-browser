#!/usr/bin/env bash
# CI（ci.yml）coverage：采集 + 过滤 + 打印。
#
# 过滤掉系统库、FetchContent 依赖与测试自身，只保留项目源码的覆盖率。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT/build/coverage"

lcov --capture --directory . --output-file coverage.info --ignore-errors mismatch,negative,gcov,gcov
lcov --remove coverage.info --output-file coverage-filtered.info \
  --ignore-errors unused \
  '/usr/*' '*/_deps/*' '*/tests/*' '*/build/*'
lcov --list coverage-filtered.info
