#!/usr/bin/env bash
# release.yml：macOS 生产构建依赖。
#
# Qt 只装 qtbase：GUI 只用 Widgets（测试用 Test）。不要装聚合包 `qt`——
# 它会拖入 qtwebengine（QtPdf）、qtvirtualkeyboard、qtsvg、qtdeclarative，
# 并把它们的插件符号链接进共享插件目录；而 macdeployqt 会*无条件*部署
# iconengines / platforminputcontexts / imageformats 里的插件，对应的
# framework 却分散在各自的 keg 里解析不到 → 打包出现 “Cannot resolve
# rpath” 与依赖校验失败（2026-09 rc2 macOS 发布失败的根因之一）。
set -euo pipefail

: "${GITHUB_ENV:?GITHUB_ENV 未设置：本脚本在 GitHub Actions 中运行}"

brew install jpeg webp qtbase freetype openssl ffmpeg libavif
# openssl 是 keg-only，不会符号链接进 prefix；qtbase 也显式加入
# CMAKE_PREFIX_PATH，保证 find_package(Qt6) 不依赖 keg 链接方式
# （Intel runner 的 prefix 为 /usr/local）。
{
  echo "CMAKE_PREFIX_PATH=$(brew --prefix);$(brew --prefix qtbase)"
  echo "OPENSSL_ROOT_DIR=$(brew --prefix openssl)"
  echo "PKG_CONFIG_PATH=$(brew --prefix)/lib/pkgconfig"
} >> "$GITHUB_ENV"
