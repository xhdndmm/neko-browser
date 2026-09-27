#!/usr/bin/env bash
# CI（ci.yml）macOS 构建依赖。
#
# 只装 qtbase（GUI 只用 Qt6 Widgets，测试用 Qt6 Test）。不要装聚合包 `qt`——
# 它会拖入 qtwebengine/qtvirtualkeyboard/qtsvg/... 模块，而发布打包时
# macdeployqt 会部署这些模块的插件却解析不到其 framework（见
# tools/package_runtime_macos.sh 与 ADR 0019）。
set -euo pipefail

: "${GITHUB_ENV:?GITHUB_ENV 未设置：本脚本在 GitHub Actions 中运行}"

brew install jpeg webp qtbase freetype openssl ffmpeg libavif
echo "CMAKE_PREFIX_PATH=$(brew --prefix);$(brew --prefix qtbase)" >> "$GITHUB_ENV"
