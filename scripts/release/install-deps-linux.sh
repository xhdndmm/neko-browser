#!/usr/bin/env bash
# release.yml：Linux 生产构建依赖。
#
# 与 scripts/ci/install-deps-linux.sh 的差异：patchelf（打包阶段改写 RPATH）
# 与 qt6-wayland（Wayland 平台插件族；tools/package_runtime_linux.sh 必须把
# 它们和 platforms/libqwayland*.so 一起捆绑，缺一则 GUI 在 Wayland 会话起不来）。
set -euo pipefail

sudo apt-get update
sudo apt-get install -y \
  zlib1g-dev libjpeg-dev libwebp-dev libfreetype-dev libssl-dev qt6-base-dev qt6-wayland \
  libavcodec-dev libavformat-dev libavutil-dev libavdevice-dev libswscale-dev libavif-dev \
  patchelf
