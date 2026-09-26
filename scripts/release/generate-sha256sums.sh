#!/usr/bin/env bash
# release.yml：把各构建任务上传的 .sha256 合并成 SHA256SUMS 并自校验。
#
# 前置：dist/ 已由 actions/download-artifact 填入（各任务的 .sha256 汇总）。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT/dist"

cat ./*.sha256 > SHA256SUMS
sha256sum --check SHA256SUMS
cat SHA256SUMS
