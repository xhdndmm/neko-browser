#!/usr/bin/env bash
# release.yml：Linux 生产构建依赖。
#
# 与 scripts/ci/install-deps-linux.sh 的差异只有 patchelf：打包阶段要用它
# 改写 RPATH（ADR 0019）。
set -euo pipefail

sudo apt-get update
sudo apt-get install -y \
  zlib1g-dev libjpeg-dev libwebp-dev libfreetype-dev libssl-dev qt6-base-dev \
  libavcodec-dev libavformat-dev libavutil-dev libavdevice-dev libswscale-dev libavif-dev \
  patchelf
