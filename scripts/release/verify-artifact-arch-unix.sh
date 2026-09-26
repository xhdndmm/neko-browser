#!/usr/bin/env bash
# release.yml：校验 Linux / macOS 产物架构与发布矩阵一致。
#
# 双架构是发布的核心诉求：产物架构与矩阵不一致时必须失败，而不是在交叉
# 编译配置下悄悄产出 x86_64 二进制（它在 x86_64 runner 上同样能运行，
# 不会被发现）。
#
# 用法：scripts/release/verify-artifact-arch-unix.sh <x86_64|arm64>
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

arch="${1:?usage: scripts/release/verify-artifact-arch-unix.sh <x86_64|arm64>}"
case "$arch" in
  x86_64) pattern='x86[-_]64' ;;
  arm64)  pattern='aarch64|arm64' ;;
  *) echo "::error::未知架构：$arch"; exit 1 ;;
esac

found=0
for binary in build/release/bin/neko_browser build/release/bin/neko_browser_gui; do
  [ -f "$binary" ] || continue
  description=$(file -b "$binary")
  echo "$binary: $description"
  if ! grep -Eq "$pattern" <<<"$description"; then
    echo "::error::$binary 的架构不是 $arch"
    exit 1
  fi
  found=$((found + 1))
done

if [ "$found" -eq 0 ]; then
  echo "::error::build/release/bin 下没有可执行文件"
  exit 1
fi
