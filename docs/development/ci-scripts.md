# CI / 发布脚本（`scripts/`）

`.github/workflows/` 里的三个 workflow 只保留：

- 触发条件、矩阵（平台 × 架构 × 预设）与 `needs` 依赖；
- 需要 GitHub 上下文的步骤：checkout、artifact 上传/下载、创建 Release；
- 无控制流的单命令调用（`cmake --build --preset ...`、`ctest --preset ...`、
  单条 `cmake --preset ...` 配置等）。

其余**依赖安装、环境准备、打包、校验**逻辑全部在 `scripts/` 下：

```text
scripts/
├── ci/                ci.yml（构建矩阵 + sanitizer + coverage + 格式门禁）
├── static-analysis/   static-analysis.yml（clang-tidy，非阻塞）
└── release/           release.yml（三平台 × 双架构发布）
```

## 约定

| 约定 | 说明 |
| --- | --- |
| 语言 | Linux/macOS 用 `.sh`（`#!/usr/bin/env bash` + `set -euo pipefail`）；Windows 用 `.ps1`（`$ErrorActionPreference = 'Stop'`，原生命令失败显式检查 `$LASTEXITCODE`） |
| 参数 | 矩阵相关取值通过命令行参数传入（如 `-Arch x86_64`、`bash scripts/... linux`），脚本里不出现 `${{ }}` |
| 环境 | 依赖 Actions 注入的变量（`GITHUB_ENV`、`GITHUB_OUTPUT`、`GITHUB_PATH`、`PACKAGE`、`QT_PREFIX` 等）在脚本头部注释登记 |
| 失败语义 | 校验/打包脚本失败即非零退出；诊断脚本（`diagnose-link-inputs-windows.ps1`）只打印、不失败 |
| 注释 | 解释“为什么”的背景（历史事故、平台差异、ADR 链接）随脚本走，workflow 中只留一句指引 |
| 路径 | 脚本自行解析仓库根（`ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"`），在任意工作目录运行结果一致 |

## 本地运行与校验

改动 `scripts/` 或 workflow 后，提交前运行：

```bash
for f in scripts/*/*.sh; do bash -n "$f"; done     # 语法
shellcheck scripts/*/*.sh                           # 静态检查
actionlint                                          # workflow YAML（go install github.com/rhysd/actionlint/cmd/actionlint@latest）
```

不依赖平台工具链、可以本机直接运行的脚本：

```bash
env GITHUB_REF_TYPE=tag GITHUB_REF_NAME=v0.1.0 GITHUB_OUTPUT=/tmp/out \
  bash scripts/release/resolve-version.sh
bash scripts/release/generate-sha256sums.sh   # 需要 dist/ 下已有 *.sha256
VERSION=0.1.0 TAG=v0.1.0 bash scripts/release/write-release-notes.sh
```

`.ps1` 脚本可在任意平台做语法解析（不执行）：

```powershell
$errors = $null
[System.Management.Automation.Language.Parser]::ParseFile($path, [ref]$null, [ref]$errors)
```

## 与 `tools/` 的分工

- `tools/`：开发者本机使用（`check.sh`、`format.sh`、`package_runtime_linux.sh`、
  `package_runtime_macos.sh`）。
- `scripts/`：CI / 发布专用，被 workflow 直接调用。

两者都要求“人类可以直接运行”：显式参数、明确失败、头部注释写清用法与环境。
