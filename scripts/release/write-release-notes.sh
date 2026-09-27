#!/usr/bin/env bash
# release.yml：生成 Release 说明（RELEASE-NOTES.md）。
#
# 环境：
#   VERSION  版本号（不含 v 前缀）
#   TAG      标签名（vX.Y.Z 或 vX.Y.Z-<prerelease>）
#   GITHUB_SERVER_URL / GITHUB_REPOSITORY  Actions 提供，用于拼 compare 链接
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

: "${VERSION:?VERSION 未设置}"
: "${TAG:?TAG 未设置}"

cat > RELEASE-NOTES.md <<EOF
## neko-browser ${VERSION}

由标签 \`${TAG}\` 触发 \`release\` workflow 构建：**Release + LTO**，
构建前运行完整 CTest 套件（Windows ARM64 为 x64 宿主交叉编译产物，
见「已知限制」）。

| 平台 | 架构 | 产物 | GUI |
| --- | --- | --- | --- |
| Linux | x86_64 | \`neko-browser-${VERSION}-linux-x86_64.tar.gz\` | 含 \`neko_browser_gui\` |
| Linux | arm64 | \`neko-browser-${VERSION}-linux-arm64.tar.gz\` | 含 \`neko_browser_gui\` |
| Windows | x86_64 | \`neko-browser-${VERSION}-windows-x86_64.zip\` | 含 \`neko_browser_gui\` + Qt 运行库 |
| Windows | arm64 | \`neko-browser-${VERSION}-windows-arm64.zip\` | 含 \`neko_browser_gui\` + Qt 运行库 |
| macOS | x86_64 | \`neko-browser-${VERSION}-macos-x86_64.tar.gz\` | 含 \`neko_browser_gui.app\` |
| macOS | arm64 | \`neko-browser-${VERSION}-macos-arm64.tar.gz\` | 含 \`neko_browser_gui.app\` |

### 运行前提

- 全部产物为**零安装**形态（ADR 0019）：所需运行库已随包提供。
  Windows 除 Qt（官方仅提供动态库）与 FFmpeg（LGPL 要求保持动态链接）外
  全部静态链接，并自带 Qt、FFmpeg 与 MSVC 运行库；Linux/macOS 把
  Qt、FFmpeg、OpenSSL 等第三方库捆绑在包内。
- **Linux**：需要目标机提供 glibc 与显卡驱动（glibc 基线为构建环境
  Ubuntu 24.04）；解包后直接运行 \`bin/neko_browser_gui\` 或 \`bin/neko_browser\`。
- **macOS**：GUI 为 \`neko_browser_gui.app\`（双击或 \`open\`），CLI 为
  \`bin/neko_browser\`；二进制未公证，首次运行可能需要
  \`xattr -d com.apple.quarantine\`。
- **Windows**：解压后直接运行 \`bin\neko_browser.exe\` /
  \`bin\neko_browser_gui.exe\`，无需安装 Qt 或 VC++ 运行库。
- \`SHA256SUMS\` 列出全部产物的 SHA-256；\`sha256sum --check SHA256SUMS\` 可校验。

### 已知限制

- Windows ARM64 为 x64 宿主交叉编译产物，未在 ARM64 机器上执行测试；
  同一源码的测试完整运行于 windows x86_64 任务。
- Linux 产物的 glibc 基线为构建环境（Ubuntu 24.04），更旧的发行版不受支持。
- Linux/macOS 捆绑的 FFmpeg 运行库来自发行版/Homebrew 构建（可能包含
  发行版启用的 GPL 组件）；后续计划为发布构建自有 LGPL 运行时（见 ADR 0019）。
- 二进制未签名、未公证。
- 未提供独立调试符号包。
EOF

previous_tag=$(git describe --tags --abbrev=0 "${TAG}^" 2>/dev/null || true)
if [ -n "$previous_tag" ]; then
  printf '\n**完整变更**：%s/%s/compare/%s...%s\n' \
    "$GITHUB_SERVER_URL" "$GITHUB_REPOSITORY" "$previous_tag" "$TAG" >> RELEASE-NOTES.md
fi

cat RELEASE-NOTES.md
