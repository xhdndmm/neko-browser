#!/usr/bin/env bash
# CI（ci.yml）Linux 构建依赖。
#
# 用法：scripts/ci/install-deps-linux.sh [额外包...]
#   例如 coverage 任务：scripts/ci/install-deps-linux.sh lcov
#
# 版本以 Ubuntu runner 的发行版为准；发布构建的依赖清单在
# scripts/release/install-deps-linux.sh（额外包含打包用的 patchelf）。
set -euo pipefail

packages=(
  zlib1g-dev libjpeg-dev libwebp-dev libfreetype-dev libssl-dev qt6-base-dev
  libavcodec-dev libavformat-dev libavutil-dev libavdevice-dev libswscale-dev libavif-dev
)
packages+=("$@")

sudo apt-get update
sudo apt-get install -y "${packages[@]}"
