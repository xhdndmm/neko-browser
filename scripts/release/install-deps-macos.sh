#!/usr/bin/env bash
# release.yml：macOS 生产构建依赖。
#
# Qt 只装 qtbase：GUI 只用 Widgets（测试用 Test）。不要装聚合包 `qt`——
# 它会拖入 qtwebengine（QtPdf）、qtvirtualkeyboard、qtsvg、qtdeclarative，
# 并把它们的插件符号链接进共享插件目录；而 macdeployqt 会*无条件*部署
# iconengines / platforminputcontexts / imageformats 里的插件，对应的
# framework 却分散在各自的 keg 里解析不到 → 打包出现 “Cannot resolve
# rpath” 与依赖校验失败（2026-09 rc2 macOS 发布失败的根因之一）。
#
# 注意：Homebrew 产出的库以**构建系统自身**为部署目标（bottle 按系统代次、源码
# 构建按宿主/SDK 版本，且 Homebrew 会清除 MACOSX_DEPLOYMENT_TARGET），所以发布包
# 的实际 macOS 下限 = runner 系统版本（当前 macos-15 → 15.0，见 release.yml 的
# min_macos 与 docs/releases/README.md）；升级依赖版本不改变这一点，只有自建依赖
# 才能重新指定部署目标。
set -euo pipefail

: "${GITHUB_ENV:?GITHUB_ENV 未设置：本脚本在 GitHub Actions 中运行}"

# 诊断：依赖的 minos 跟随构建工具链的默认部署目标，打包时若因「minos 高于
# MIN_MACOS」失败，这行日志能直接指出当时的 Xcode（见 release.yml 与
# docs/releases/README.md 的 macOS 下限说明）。
xcodebuild -version 2>/dev/null | head -n 1 || true

brew install jpeg webp qtbase freetype openssl ffmpeg libavif
# openssl 是 keg-only，不会符号链接进 prefix；qtbase 也显式加入
# CMAKE_PREFIX_PATH，保证 find_package(Qt6) 不依赖 keg 链接方式
# （Intel runner 的 prefix 为 /usr/local）。
{
  echo "CMAKE_PREFIX_PATH=$(brew --prefix);$(brew --prefix qtbase)"
  echo "OPENSSL_ROOT_DIR=$(brew --prefix openssl)"
  echo "PKG_CONFIG_PATH=$(brew --prefix)/lib/pkgconfig"
} >> "$GITHUB_ENV"
