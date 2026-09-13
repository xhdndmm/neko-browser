# 发布说明

> 状态：尚未正式发布（0.1.0 开发里程碑）。

## 版本策略

- 采用 Semantic Versioning（`0.x.y` 起步）
- 达到真正稳定前不宣称 `1.0.0`
- 标签格式：`vX.Y.Z`；预发布使用 `vX.Y.Z-<prerelease>`（例如 `v0.1.0-rc1`）
- 标签版本必须与 `CMakeLists.txt` 的 `project(... VERSION X.Y.Z)` 一致，
  否则发布工作流在 `prepare` 阶段直接失败，不产出任何产物

## 自动发布流程

工作流：`.github/workflows/release.yml`

```text
push tag vX.Y.Z
    ↓
prepare   解析版本、校验「标签 ↔ CMakeLists 版本」一致
    ↓
build × 6 Release + LTO、跑完整 ctest、校验产物架构、打包
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
| Linux | x86_64 | `ubuntu-24.04` | 含 Qt6 GUI |
| Linux | arm64 | `ubuntu-24.04-arm` | 含 Qt6 GUI |
| Windows | x86_64 | `windows-2025` | 含 Qt6 GUI（自带 Qt 运行库） |
| Windows | arm64 | `windows-2025`（x64 宿主交叉编译） | 含 Qt6 GUI（自带 Qt 运行库） |
| macOS | x86_64 | `macos-15-intel` | 含 Qt6 GUI |
| macOS | arm64 | `macos-15` | 含 Qt6 GUI |

Windows 全部使用 x64 runner：x86_64 为原生构建，ARM64 用 MSVC 交叉编译到
ARM64（Qt 也用官方 ARM64 交叉编译包，宿主工具来自同版本 x64 包）。
Linux/macOS 使用 runner 原生架构与系统包管理器（apt / Homebrew）。
打包前会校验产物架构（Windows 解析 PE Machine，Linux/macOS 用 `file`），
架构与矩阵不符即失败——避免在交叉编译配置下悄悄产出 x64 产物。

### 「生产版本」的定义

- CMake preset `release`：`CMAKE_BUILD_TYPE=Release` + `NEKO_ENABLE_LTO=ON`
- `NEKO_WARNINGS_AS_ERRORS=ON`（与 CI 一致，见 AGENTS.md §13）
- 每个平台在打包前运行完整 `ctest`；测试不通过则不会产出 Release
  （例外：Windows ARM64 为交叉编译产物，无法在 x64 runner 上执行，见「已知限制」）

### 产物

```text
neko-browser-<version>-{linux,windows,macos}-{x86_64,arm64}.{tar.gz,zip}
├── bin/neko_browser[.exe]        CLI
├── bin/neko_browser_gui[.exe]    Qt6 GUI（Windows 产物自带 Qt 运行库与平台插件）
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
- **动态链接**：Linux/macOS 的 GUI 需要目标机器提供 Qt6 运行库；全部产物需要
  zlib / libjpeg / libwebp / FreeType / OpenSSL / FFmpeg / libavif
  （见 [BUILDING.md](../../BUILDING.md)「依赖获取」）。
- **未签名 / 未公证**：macOS 首次运行可能需要
  `xattr -d com.apple.quarantine <binary>`。
- macOS 产物未打成独立 `.app`（依赖 Homebrew 的 Qt 运行库路径）。
- **无独立调试符号包**。

### 后续工作（Phase 12+）

- 代码签名（Windows）与公证（macOS）
- 调试符号包 / symbol server
- 自带运行库的独立发行包（`.app` / AppImage / MSIX 等）
- `project(VERSION)` 与标签的同步自动化

## 发布历史

| 版本 | 日期 | 内容 |
| --- | --- | --- |
| — | 尚未发布 | 开发中（Phases 0–8：引擎纵向切片 + Qt6 GUI + 存储/图像/媒体/PDF + JS runtime，277 测试全绿） |
