#!/usr/bin/env bash
# CI（ci.yml）coverage：采集 + 过滤 + 打印。
#
# 过滤掉系统库、FetchContent 依赖与测试自身，只保留项目源码的覆盖率。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT/build/coverage"

# GCC 15 的 gcov 数据在模板/内联较多的 TU 上会触发 lcov 2.x 的
# negative/inconsistent（负计数、行号不匹配）——编译器侧噪声，已确认只影响
# 统计精度；出现真实解析错误（数据损坏、格式错误）时仍会失败。
lcov --capture --directory . --output-file coverage.info \
  --ignore-errors mismatch,inconsistent,negative,gcov
lcov --remove coverage.info --output-file coverage-filtered.info \
  --ignore-errors unused,inconsistent \
  '/usr/*' '*/_deps/*' '*/tests/*' '*/build/*'
lcov --list coverage-filtered.info
