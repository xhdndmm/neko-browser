#!/usr/bin/env bash
# release.yml：创建或更新 GitHub Release。
#
# 重新运行工作流时先尝试 upload --clobber，而不是让上传失败。
#
# 环境：
#   GH_TOKEN     gh CLI 读取的 token（workflow 里映射自 secrets.GITHUB_TOKEN）
#   TAG          标签名
#   PRERELEASE   "true" / "false"
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

: "${TAG:?TAG 未设置}"

flags=()
if [ "${PRERELEASE:-false}" = "true" ]; then
  flags+=(--prerelease)
fi

if gh release view "$TAG" >/dev/null 2>&1; then
  # 重新运行工作流时覆盖已有产物，而不是让上传失败。
  gh release upload "$TAG" dist/*.tar.gz dist/*.zip dist/SHA256SUMS --clobber
else
  gh release create "$TAG" \
    --verify-tag \
    --title "neko-browser $TAG" \
    --notes-file RELEASE-NOTES.md \
    "${flags[@]}" \
    dist/*.tar.gz dist/*.zip dist/SHA256SUMS
fi
