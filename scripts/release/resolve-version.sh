#!/usr/bin/env bash
# release.yml `prepare`：解析发布版本并校验标签与 CMakeLists.txt 一致。
#
# 尽早失败，避免版本不对时还白跑 6 个构建任务。
#
# 输出（$GITHUB_OUTPUT）：
#   version     去掉 v 前缀的版本（可含 -<prerelease>）
#   tag         标签名；workflow_dispatch 演练时为空
#   prerelease  "true"/"false"
#
# 环境：GITHUB_REF_TYPE、GITHUB_REF_NAME、GITHUB_OUTPUT（均由 Actions 提供）
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

: "${GITHUB_REF_TYPE:?GITHUB_REF_TYPE 未设置：本脚本在 GitHub Actions 中运行}"
: "${GITHUB_OUTPUT:?GITHUB_OUTPUT 未设置：本脚本在 GitHub Actions 中运行}"

cmake_version=$(grep -m1 -E '^[[:space:]]*VERSION[[:space:]]+[0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt | awk '{print $2}')
if [ -z "$cmake_version" ]; then
  echo "::error::无法从 CMakeLists.txt 解析 project(VERSION ...)"
  exit 1
fi

if [ "$GITHUB_REF_TYPE" = "tag" ]; then
  tag="${GITHUB_REF_NAME}"
  if ! [[ "$tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$ ]]; then
    echo "::error::标签 '$tag' 不是 vX.Y.Z 或 vX.Y.Z-<prerelease> 形式"
    exit 1
  fi
  version="${tag#v}"
  base_version="${version%%-*}"
  if [ "$base_version" != "$cmake_version" ]; then
    echo "::error::标签版本（$base_version）与 CMakeLists.txt project(VERSION $cmake_version) 不一致"
    exit 1
  fi
  prerelease=false
  if [ "$version" != "$base_version" ]; then
    prerelease=true
  fi
else
  tag=""
  version="$cmake_version"
  prerelease=false
  echo "::notice::workflow_dispatch 演练运行：使用 CMakeLists.txt 版本 $version，不创建 Release"
fi

{
  echo "version=$version"
  echo "tag=$tag"
  echo "prerelease=$prerelease"
} >> "$GITHUB_OUTPUT"
