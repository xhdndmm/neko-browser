# 发布说明

> 状态：尚未正式发布（0.1.0 开发里程碑）。

## 版本策略

- 采用 Semantic Versioning（`0.x.y` 起步）
- 达到真正稳定前不宣称 `1.0.0`
- 标签格式：`vX.Y.Z`；预发布使用 `vX.Y.Z-<prerelease>`（例如 `v0.1.0-rc1`）
- 标签版本必须与 `CMakeLists.txt` 的 `project(... VERSION X.Y.Z)` 一致，
  否则发布工作流在 `prepare` 阶段直接失败，不产出任何产物

## 自动发布流程

工作流：`.github/workflows/release.yml`；各步骤脚本：`scripts/release/`
（Windows 为 `.ps1`，Linux/macOS 为 `.sh`，约定见
[../development/ci-scripts.md](../development/ci-scripts.md)）。

```text
push tag vX.Y.Z
    ↓
prepare   解析版本、校验「标签 ↔ CMakeLists 版本」一致
    ↓
build × 6 Release + LTO、跑完整 ctest、校验产物架构、零安装打包（ADR 0019）
+ 产物冒烟测试
    ↓
release   合并产物、生成 SHA256SUMS、创建（或更新）GitHub Release
```

### 触发方式

| 事件 | 行为 |
| --- | --- |
| push tag `vX.Y.Z` | 构建全部产物并创建 Release（带 `-<prerelease>` 的标签自动标记为 prerelease） |
| `workflow_dispatch` | 相同的完整生产构建与测试，仅上传 workflow artifact，**不**创建 Release（打标签前演练） |

### 构建矩阵

| 平台 | 架构 | Runner | GUI |
| --- | --- | --- | --- |
| Linux | x86_64 | `ubuntu-26.04` | 含 Qt6 GUI |
| Linux | arm64 | `ubuntu-26.04-arm` | 含 Qt6 GUI |
| Windows | x86_64 | `windows-2025` | 含 Qt6 GUI（自带 Qt 运行库） |
| Windows | arm64 | `windows-2025`（x64 宿主交叉编译） | 含 Qt6 GUI（自带 Qt 运行库） |
| macOS | x86_64 | `macos-26-intel` | 含 Qt6 GUI |
| macOS | arm64 | `macos-26` | 含 Qt6 GUI |

Windows 全部使用 x64 runner：x86_64 为原生构建，ARM64 用 MSVC 交叉编译到
ARM64（Qt 也用官方 ARM64 交叉编译包，宿主工具来自同版本 x64 包）。
Linux/macOS 使用 runner 原生架构与系统包管理器（apt / Homebrew）。
打包前会校验产物架构（Windows 解析 PE Machine，Linux/macOS 用 `file`），
架构与矩阵不符即失败——避免在交叉编译配置下悄悄产出 x64 产物。

Windows 链接方式（见 [ADR 0019](../architecture/adr/0019-release-runtime-packaging.md)）：
除 **Qt**（官方仅提供动态库）与 **FFmpeg**（LGPL 重链接义务，见 ADR 0014）
外全部依赖静态链接——vcpkg 使用 `*-windows-static-md` 三元组；FFmpeg 从
动态三元组单独安装，DLL 与 Qt、MSVC 运行库一起随包分发。

### 零安装打包（ADR 0019）

- **Windows**：静态链接（见上）+ 捆绑 Qt（`windeployqt` / ARM64 手工部署）、
  FFmpeg DLL 与 MSVC 运行库；打包后校验**包内每个 PE 文件**（exe 与 dll）的
  导入表——只允许包内文件、System32 或 Windows API set（`api-ms-win-*`，
  由加载器解析、不是磁盘文件），即完整传递闭包；且被静态链接的依赖不得
  再以 DLL 形式出现。
- **Linux**：`tools/package_runtime_linux.sh` 把全部非系统库（Qt、FFmpeg、
  OpenSSL、...）复制进 `lib/`、Qt 插件进 `plugins/`，RPATH 改写为 `$ORIGIN`
  相对路径；**不**捆绑 glibc（NSS/DNS 需要）与 GPU/驱动栈（需要匹配内核驱动）。
- **macOS**：`tools/package_runtime_macos.sh` 把 GUI 组装为
  `neko_browser_gui.app` 并交给 `macdeployqt`（Qt framework、插件、非 Qt dylib
  一并部署；Qt 只装 Homebrew `qtbase`，见 ADR 0019）；ad-hoc 签名由脚本在
  部署完成后统一完成（从内到外 + 封 `.app` + `codesign --verify --deep`）。
  CLI 的依赖捆绑进 `lib/`，引用改写为 `@executable_path/../lib/...`。
  打包后按 dyld 语义校验每个依赖都能解析到包内文件；系统框架仍来自目标机。
- 打包后立即用**产物本身**跑冒烟测试（CLI `--dump-dom`；GUI 以
  `QT_QPA_PLATFORM=offscreen` 启动），且清空 `LD_LIBRARY_PATH` /
  `DYLD_LIBRARY_PATH`，避免借用到构建机已装的库。

### 「生产版本」的定义

- CMake preset `release`：`CMAKE_BUILD_TYPE=Release` + `NEKO_ENABLE_LTO=ON`
- `NEKO_WARNINGS_AS_ERRORS=ON`（与 CI 一致，见 AGENTS.md §13）
- 每个平台在打包前运行完整 `ctest`；测试不通过则不会产出 Release
  （例外：Windows ARM64 为交叉编译产物，无法在 x64 runner 上执行，见「已知限制」）
- 打包后通过零安装校验（Windows 导入表检查；Linux/macOS 依赖闭包校验）
  与产物冒烟测试

### 产物

```text
neko-browser-<version>-linux-x86_64.tar.gz
├── bin/neko_browser              CLI
├── bin/neko_browser_gui          Qt6 GUI（可直接运行，无需安装 Qt）
├── lib/                          全部非系统运行库（RPATH 已改写）
├── plugins/                      Qt 平台/图像格式插件（qt.conf 指向此处）
├── LICENSE
└── README.md

neko-browser-<version>-macos-{x86_64,arm64}.tar.gz
├── bin/neko_browser              CLI（依赖在 lib/）
├── lib/                          CLI 的非系统运行库
├── neko_browser_gui.app          Qt6 GUI（macdeployqt 部署，ad-hoc 签名）
├── LICENSE
└── README.md

neko-browser-<version>-windows-{x86_64,arm64}.zip
├── bin/neko_browser.exe          CLI（静态链接，仅需 Windows 系统 DLL）
├── bin/neko_browser_gui.exe      Qt6 GUI（Qt/FFmpeg/CRT 运行库随身）
├── bin/*.dll                     Qt、FFmpeg、MSVC 运行库
├── LICENSE
└── README.md
```

Release 页面同时附带 `SHA256SUMS`（`sha256sum --check SHA256SUMS` 校验）。
重新运行工作流会覆盖同名产物（`gh release upload --clobber`），不会重复出错。

### 发布前检查清单

- CI 全绿
- sanitizer 通过
- 文档更新（含本文件与兼容性矩阵）
- 标签版本与 `CMakeLists.txt` 一致

### 已知限制

- **Windows ARM64 未运行测试**：产物由 x64 宿主交叉编译，无法在 x64 runner 上执行；
  同一源码的测试完整运行于 `windows-x86_64` 任务。ARM64 的 Qt 运行库按固定清单
  手工部署（Qt 交叉编译包不含 windeployqt）：3 个 Qt DLL + 平台/样式/图像格式插件。
- **Linux glibc 基线**：产物在 Ubuntu 24.04 上构建，需要目标机的 glibc 不低于
  构建环境；更旧的发行版不受支持（glibc 与显卡驱动始终来自目标机，见 ADR 0019）。
- **捆绑的 FFmpeg 来自发行版/Homebrew 构建**：其中可能包含发行版启用的 GPL
  组件；后续计划为发布构建自有 LGPL 运行时（最小特性集）。
- **未签名 / 未公证**：macOS 首次运行可能需要
  `xattr -d com.apple.quarantine <binary>`；签名/公证见后续工作。
- **无独立调试符号包**。

### 后续工作（Phase 12+）

- 代码签名（Windows）与公证（macOS）
- 调试符号包 / symbol server
- 自有 LGPL FFmpeg 运行时（替代发行版/Homebrew 构建）
- `project(VERSION)` 与标签的同步自动化

## 发布历史

| 版本 | 日期 | 内容 |
| --- | --- | --- |
| — | 尚未发布 | 开发中（Phases 0–8：引擎纵向切片 + Qt6 GUI + 存储/图像/媒体/PDF + JS runtime，277 测试全绿） |
