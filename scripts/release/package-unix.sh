#!/usr/bin/env bash
# release.yml：Linux / macOS 零安装打包 + 冒烟测试（ADR 0019）。
#
# Linux 把全部非系统库捆绑进 lib/ 并改写 RPATH；macOS 的 GUI 打成 .app 由
# macdeployqt 部署（framework / Qt 插件 / 非 Qt dylib 一并处理），CLI 的依赖
# 捆绑进 lib/。随后用打包产物直接跑冒烟测试，并清空 LD_LIBRARY_PATH /
# DYLD_LIBRARY_PATH，防止借用到构建机已装的库。
#
# 用法：scripts/release/package-unix.sh <linux|macos> <version>
#
# 环境：PACKAGE —— 产物目录名（workflow 的 env 提供）
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

platform="${1:?usage: scripts/release/package-unix.sh <linux|macos> <version>}"
version="${2:?usage: scripts/release/package-unix.sh <linux|macos> <version>}"
: "${PACKAGE:?PACKAGE 未设置：产物目录名由 workflow 的 env 提供}"

rm -rf "$PACKAGE"
mkdir -p "$PACKAGE/bin"

cp build/release/bin/neko_browser "$PACKAGE/bin/"
if [ -f build/release/bin/neko_browser_gui ]; then
  cp build/release/bin/neko_browser_gui "$PACKAGE/bin/"
fi
cp LICENSE README.md "$PACKAGE/"

if [ "$platform" = "linux" ]; then
  bash tools/package_runtime_linux.sh "$PACKAGE"
  gui_bin="$PACKAGE/bin/neko_browser_gui"
else
  bash tools/package_runtime_macos.sh "$PACKAGE" "$version"
  gui_bin="$PACKAGE/neko_browser_gui.app/Contents/MacOS/neko_browser_gui"
fi

# 冒烟：CLI 解析本地页面并 dump DOM。
printf '<html><body><p>packaged smoke</p></body></html>' > /tmp/neko-smoke.html
env -u LD_LIBRARY_PATH -u DYLD_LIBRARY_PATH NEKO_PROFILE="$PWD/smoke-profile" \
  "$PACKAGE/bin/neko_browser" --url /tmp/neko-smoke.html --dump-dom > /tmp/neko-smoke-out.txt
grep -q 'packaged smoke' /tmp/neko-smoke-out.txt

# 冒烟：GUI 在 offscreen 平台上至少存活 8 秒（无“平台插件加载失败”）。
if [ -x "$gui_bin" ]; then
  env -u LD_LIBRARY_PATH -u DYLD_LIBRARY_PATH NEKO_PROFILE="$PWD/smoke-profile" \
    QT_QPA_PLATFORM=offscreen "$gui_bin" > /tmp/neko-gui-smoke.log 2>&1 &
  gui_pid=$!
  sleep 8
  if ! kill -0 "$gui_pid" 2>/dev/null; then
    echo "::error::GUI 冒烟测试提前退出"
    cat /tmp/neko-gui-smoke.log
    exit 1
  fi
  kill "$gui_pid"
  wait "$gui_pid" 2>/dev/null || true
  if grep -qiE 'failed to load platform|could not load the Qt platform|Plugin loader' /tmp/neko-gui-smoke.log; then
    echo "::error::GUI 未能在 offscreen 平台启动"
    cat /tmp/neko-gui-smoke.log
    exit 1
  fi
fi

tar -czf "$PACKAGE.tar.gz" "$PACKAGE"
sha256sum "$PACKAGE.tar.gz" > "$PACKAGE.tar.gz.sha256"
cat "$PACKAGE.tar.gz.sha256"
