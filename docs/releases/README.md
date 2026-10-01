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
test-windows-arm64   在原生 ARM64 runner 上运行 windows-arm64 的 ctest
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
| Windows | arm64 | 构建 `windows-2025`（x64 宿主交叉编译），测试 `windows-11-arm` | 含 Qt6 GUI（自带 Qt 运行库） |
| macOS | x86_64 | `macos-26-intel` | 含 Qt6 GUI |
| macOS | arm64 | `macos-26` | 含 Qt6 GUI |

Windows 构建全部使用 x64 runner：x86_64 为原生构建，ARM64 用 MSVC 交叉编译到
ARM64（Qt 也用官方 ARM64 交叉编译包，宿主工具来自同版本 x64 包）；ARM64 的
测试在原生 ARM64 runner（`windows-11-arm`）上运行——构建任务把自包含测试载荷
打成 artifact，测试任务按相同工作区布局还原后跑 ctest。
Linux/macOS 使用 runner 原生架构与系统包管理器（apt / Homebrew）。
打包前会校验产物架构（Windows 解析 PE Machine，Linux/macOS 用 `file`），
架构与矩阵不符即失败——避免在交叉编译配置下悄悄产出 x64 产物。

Windows 链接方式（见 [ADR 0019](../architecture/adr/0019-release-runtime-packaging.md)）：
除 **Qt**（官方仅提供动态库）与 **FFmpeg**（LGPL 重链接义务，见 ADR 0014）
外全部依赖静态链接——vcpkg 使用 `*-windows-static-md` 三元组；FFmpeg 从
动态三元组单独安装，DLL 与 Qt、MSVC 运行库一起随包分发。

### 最低系统版本与基线校验

产物声明的操作系统下限与校验方式（详细政策见 [BUILDING.md](../../BUILDING.md)
「最低操作系统版本」）：

- **macOS**：`CMAKE_OSX_DEPLOYMENT_TARGET=13.3`（高于 Qt 6.8 自身的 13.0 基线：
  libc++ 的 `<format>` 需要 macOS 13.3 才提供的浮点 `std::to_chars`，低于 13.3
  会在配置阶段直接失败；可在配置时提高）。
  打包脚本对包内每个 Mach-O 校验 `LC_BUILD_VERSION minos` ≤ 声明值
  （`MIN_MACOS` 可覆盖声明，默认 13.3）。Homebrew 依赖为 runner 自身系统构建，
  若某个依赖超出声明值，打包直接失败并列出文件——不得静默发布与文档不符的产物。
- **Windows**：`_WIN32_WINNT=_WINVER=0x0A00`（Windows 10 1809）在 CMake 层统一
  定义；零安装校验（ADR 0019）额外保证导入表中不存在包外 DLL。
- **Linux**：产物不捆绑 glibc，因此基线 = 构建镜像的 glibc（当前 `ubuntu-26.04`）。
  `scripts/release/package-unix.sh` 自动用 `getconf GNU_LIBC_VERSION` 取值并通过
  `tools/check_glibc_baseline.sh` 校验：包内任何 ELF 的最高 `GLIBC_x.y` 符号需求
  不得超过该值（防止预编译的第三方二进制把基线拖高）。需要声明更低支持面时
  显式传 `GLIBC_BASELINE=<x.y>`，失败输出会列出具体超标文件；**真正降低基线
  需要换用更老的构建环境**，校验的作用是让基线无法悄悄变化。

### 零安装打包（ADR 0019）

- **Windows**：静态链接（见上）+ 捆绑 Qt（`windeployqt` / ARM64 手工部署）、
  FFmpeg DLL 与 MSVC 运行库；打包后校验**包内每个 PE 文件**（exe 与 dll）的
  导入表——只允许包内文件、System32 或 Windows API set（`api-ms-win-*`，
  由加载器解析、不是磁盘文件），即完整传递闭包；且被静态链接的依赖不得
  再以 DLL 形式出现。
- **Linux**：`tools/package_runtime_linux.sh` 把全部非系统库（Qt、FFmpeg、
  OpenSSL、...）复制进 `lib/`、Qt 插件进 `plugins/`，RPATH 改写为 `$ORIGIN`
  相对路径；平台插件除 `platforms/` 与 `imageformats/` 外还包含
  **Wayland 客户端插件族**（shell integration / decoration /
  graphics-integration-client —— 缺了它们 wayland 平台插件加载失败，
  显式要求 Wayland 时 GUI 起不来，见 ADR 0019）；**不**捆绑 glibc（NSS/DNS
  需要）与 GPU/驱动栈（需要匹配内核驱动）。产物捆绑的 `libcrypto` 来自构建
  runner，因此信任库在**运行主机**上发现（ADR 0010 修订）。
- **macOS**：`tools/package_runtime_macos.sh` 把 GUI 组装为
  `neko_browser_gui.app` 并交给 `macdeployqt`（Qt framework、插件、非 Qt dylib
  一并部署；Qt 只装 Homebrew `qtbase`，见 ADR 0019）；部署后脚本会再跑一遍
  依赖归一化（删除落不到包内的 LC_RPATH、把已部署依赖的引用改写为
  `@executable_path/../Frameworks/...`——`macdeployqt` 对经 LC_RPATH 命中的
  依赖两者都不做）；ad-hoc 签名由脚本在归一化后统一完成
  （先签嵌套 Mach-O、再签主可执行文件、最后封 `.app`，再
  `codesign --verify --deep`；顺序为什么是强制的见 ADR 0019）。
  CLI 的依赖捆绑进 `lib/`，引用改写为 `@executable_path/../lib/...`。
  打包后按 dyld 语义校验每个依赖都能解析到包内文件；系统框架仍来自目标机。
- 打包后立即用**产物本身**跑冒烟测试（CLI `--dump-dom`；GUI 以
  `QT_QPA_PLATFORM=offscreen` 启动），且清空 `LD_LIBRARY_PATH` /
  `DYLD_LIBRARY_PATH`，避免借用到构建机已装的库。
  注意：runner 与构建机同为 Debian 系，**测试不出** CA 布局差异类故障
  （2026-09 rc2：产物的 `libcrypto` 指向 runner 的 `/usr/lib/ssl`，在
  Arch/Fedora 上信任库为空）；发布前在非 Debian 系主机上跑一次
  `bin/neko_browser --url https://example.com/ --dump-dom` 是最直接的
  端到端验证。

### 「生产版本」的定义

- CMake preset `release`：`CMAKE_BUILD_TYPE=Release` + `NEKO_ENABLE_LTO=ON`
- `NEKO_WARNINGS_AS_ERRORS=ON`（与 CI 一致，见 AGENTS.md §13）
- 每个平台在打包前运行完整 `ctest`；测试不通过则不会产出 Release
  （Windows ARM64 的测试由 `test-windows-arm64` 任务在原生 ARM64 runner 上
  运行，见「构建矩阵」）
- 打包后通过零安装校验（Windows 导入表检查；Linux/macOS 依赖闭包校验）
  与产物冒烟测试

### 产物

```text
neko-browser-<version>-linux-x86_64.tar.gz
├── bin/neko_browser              CLI
├── bin/neko_browser_gui          Qt6 GUI（可直接运行，无需安装 Qt）
├── lib/                          全部非系统运行库（RPATH 已改写）
├── plugins/                      Qt 平台/图像格式/Wayland 插件（qt.conf 指向此处）
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

- **Windows ARM64 测试在独立 runner 上运行**：构建仍是 `windows-2025`（x64 宿主
  交叉编译），测试载荷上传后在 `windows-11-arm`（原生 ARM64）上运行 ctest；
  两个 runner 的工作区盘符不同（x64 镜像 `D:\a\<repo>\<repo>`，ARM64 镜像
  `C:\a\<repo>\<repo>`——ARM64 镜像的第二块盘没有挂载，见
  actions/runner-images#14088），还原脚本先把构建机的盘符别名到本 runner 的
  盘符根，ctest 生成文件与测试二进制（`NEKO_TEST_PAGES_DIR` / `NEKO_BROWSER_BIN`）
  里写死的绝对路径才成立（载荷与还原细节见
  `scripts/release/stage-tests-windows-arm64.ps1` / `test-windows-arm64.ps1`）。
  ARM64 的 Qt 运行库按固定清单手工部署（Qt 交叉编译包不含 windeployqt）：
  3 个 Qt DLL + 平台/样式/图像格式插件。
- **Linux glibc 基线**：产物在 `ubuntu-26.04` 上构建，需要目标机的 glibc 不低于
  构建环境；更旧的发行版不受支持（glibc 与显卡驱动始终来自目标机，见 ADR 0019）。
- **捆绑的 FFmpeg 来自发行版/Homebrew 构建**：其中可能包含发行版启用的 GPL
  组件；后续计划为发布构建自有 LGPL 运行时（最小特性集）。
- **未签名 / 未公证**：macOS 首次运行可能需要
  `xattr -d com.apple.quarantine <binary>`；签名/公证见后续工作。
- **证书信任库来自目标机**：产物不携带 CA 包（HTTPS 用目标机 CA 库，见
  ADR 0010 修订）。目标机没有 `ca-certificates` 时 HTTPS 会失败关闭并提示
  安装或设置 `SSL_CERT_FILE`；macOS 尚未接 Keychain，Windows 系统 ROOT 库
  尚未在运行期验证。
- **无独立调试符号包**。

### 后续工作（Phase 12+）

- 代码签名（Windows）与公证（macOS）
- 调试符号包 / symbol server
- 自有 LGPL FFmpeg 运行时（替代发行版/Homebrew 构建）
- `project(VERSION)` 与标签的同步自动化

## 发布历史

| 版本 | 日期 | 内容 |
| --- | --- | --- |
| — | 尚未发布 | 开发中（引擎纵向切片 + Qt6 GUI + 存储/图像/媒体/PDF + JS runtime + 渲染器会话；完整测试套件由 CI 运行，含 ASan/UBSan） |
