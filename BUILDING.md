# BUILDING

本指南说明如何在 Linux、Windows、macOS 上构建 neko-browser。

## 环境要求

| 组件 | 最低版本 | 说明 |
| --- | --- | --- |
| CMake | 3.24 | 建议 3.28+ |
| GCC | 12 | C++20 完整支持 |
| Clang | 15 | C++20 完整支持 |
| MSVC | VS 2022 17.x | `/std:c++20` |
| Ninja 或 Make | 任一 | 自动检测 |

## 一键流程

```bash
cmake --preset debug      # 配置（生成到 build/debug/）
cmake --build --preset debug --parallel   # 多线程编译（默认使用全部核心）
ctest --preset debug
```

或者使用 workflow preset 一步完成（构建步骤同样并行）：

```bash
cmake --workflow --preset debug
```

## Preset 一览

| Preset | 构建类型 | 说明 |
| --- | --- | --- |
| `debug` | Debug | 快速、无优化、断言全开 |
| `release` | Release | 优化 + LTO |
| `relwithdebinfo` | RelWithDebInfo | 优化 + 调试信息 |
| `asan` | RelWithDebInfo | AddressSanitizer + UBSan |
| `ubsan` | RelWithDebInfo | 仅 UBSan |
| `tsan` | RelWithDebInfo | ThreadSanitizer |
| `coverage` | Debug | gcov 覆盖率插桩 |

## 多线程编译

所有构建 preset 默认启用并行编译：`jobs` 设为 `0`（等价于命令行 `--parallel`，
交给原生构建工具决定并行度）。Ninja 与 Makefiles 生成器会利用全部 CPU 核心，
MSBuild（Windows）使用 `/m`；`cmake --workflow --preset ...` 的构建步骤同样并行。

需要限制并发数时，追加 `-j <N>` 或 `--parallel <N>`（命令行优先于 preset）：

```bash
cmake --build --preset debug --parallel 4   # 最多 4 个并发编译任务
```

也可以设置环境变量 `CMAKE_BUILD_PARALLEL_LEVEL=<N>` 作为默认并行度；
设为空字符串等价于“使用原生构建工具默认并行度”。

## 常用选项

```bash
cmake --preset debug -DNEKO_WARNINGS_AS_ERRORS=ON   # 警告即错误
cmake --preset debug -DNEKO_BUILD_TESTS=OFF         # 跳过测试
```

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `NEKO_BUILD_TESTS` | ON | 构建单元测试（拉取 GoogleTest） |
| `NEKO_WARNINGS_AS_ERRORS` | OFF | 警告提升为错误 |
| `NEKO_ENABLE_LTO` | OFF | 链接期优化 |
| `NEKO_ENABLE_COVERAGE` | OFF | 覆盖率插桩（`--coverage`） |
| `NEKO_SANITIZERS` | 空 | sanitizer 列表，如 `address;undefined` |

## 指定编译器

```bash
# Clang
cmake --preset debug -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang

# 或设置环境变量
CC=clang CXX=clang++ cmake --preset debug
```

## 依赖获取

- **GoogleTest**（单元测试）：FetchContent 从 GitHub 拉取，固定版本与 SHA256。
- **QuickJS / quickjs-ng**（JavaScript runtime）：FetchContent 从 GitHub 拉取
  `v0.16.1` tarball，固定 SHA256；配置时自动下载。
- **系统包**：zlib、libjpeg、libwebp、libavif（图像解码）、FreeType（字体光栅化）、
  OpenSSL（HTTPS/TLS）、FFmpeg（视频解码，`neko::media` 使用，必需）、
  Qt6 Widgets（GUI，可选；`NEKO_BUILD_UI=OFF` 可跳过）。
  - Debian/Ubuntu：`sudo apt install zlib1g-dev libjpeg-dev libwebp-dev libavif-dev libfreetype-dev libssl-dev libavcodec-dev libavformat-dev libavutil-dev libswscale-dev qt6-base-dev`
  - macOS（Homebrew）：`brew install jpeg webp libavif freetype qt openssl ffmpeg`
  - Windows（vcpkg）：`vcpkg install zlib libjpeg-turbo 'libavif[dav1d]' libwebp freetype openssl ffmpeg`，配置时传入
    `-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake`。
    libavif 自身不含 AV1 解码器且 vcpkg 端口没有默认特性，**必须显式选择
    `dav1d`（或 `aom`）**，否则 AVIF 解码在运行期失败（见
    [dependency-policy.md](docs/development/dependency-policy.md)）。
    vcpkg 不提供 pkg-config 程序，`FindFFmpeg.cmake` 会自动回退到头文件/库搜索，
    无需额外配置。
    Qt6 用官方 MSVC 预编译包：
    `pip install aqtinstall`，
    `aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 --archives qtbase`，
    并把 `<安装目录>/6.8.3/msvc2022_64` 加入 `CMAKE_PREFIX_PATH`。
    交叉编译 ARM64 时额外用 `-A ARM64 -DVCPKG_TARGET_TRIPLET=arm64-windows`、
    Qt 的 `win64_msvc2022_arm64_cross_compiled` 包与 `-DQT_HOST_PATH=<x64 Qt>`。
    不需要 GUI 时加 `-DNEKO_BUILD_UI=OFF`。
- 参见 [dependency-policy.md](docs/development/dependency-policy.md)。
- 离线或受限网络环境：可预先下载 tarball 并设置
  `CMAKE_FETCHCONTENT_SOURCE_DIR_GOOGLETEST` / `CMAKE_FETCHCONTENT_SOURCE_DIR_QUICKJS`
  指向解压目录。

## 发布版（零安装产物）的链接方式

正式发布产物不要求用户安装运行时依赖（见
[ADR 0019](docs/architecture/adr/0019-release-runtime-packaging.md)）。本地复现该形态：

**Windows（除 Qt / FFmpeg 外全部静态链接）**

```powershell
vcpkg install zlib libjpeg-turbo libwebp freetype openssl 'libavif[dav1d]' --triplet x64-windows-static-md
vcpkg install ffmpeg --triplet x64-windows
cmake --preset release -DNEKO_WARNINGS_AS_ERRORS=ON `
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md `
  -DFFMPEG_ROOT=<vcpkg>/installed/x64-windows
```

- `*-windows-static-md` = 静态库 + 动态 CRT（与官方 Qt 的 /MD 一致）；
- **FFmpeg 保持动态链接**（LGPL 重链接义务，见 ADR 0014）：DLL 需随包分发，
  `FFMPEG_ROOT` 让 `FindFFmpeg.cmake` 去动态三元组的安装前缀里找头文件与导入库；
- Qt 仍为官方动态库，发布时用 `windeployqt` 随包（ARM64 交叉包没有部署工具，
  按固定清单手工部署），MSVC 运行库从 VS 的 `Redist` 目录拷贝。

**Linux / macOS（捆绑非系统运行库）**

```bash
cmake --workflow --preset release
bash tools/package_runtime_linux.sh <staging-dir>             # Linux
bash tools/package_runtime_macos.sh <staging-dir> <version>   # macOS（GUI 打成 .app）
```

脚本会把可执行文件（macOS 含 GUI 的 .app）所需的非系统库复制进包内，并改写
rpath / install name；glibc 与系统框架始终来自目标机。本地开发构建
（`debug` / `release` preset）不经过这些脚本，仍使用系统包。

## 产物位置

所有产物统一输出到构建目录下：

```text
build/<preset>/bin/  可执行文件（neko_browser、测试程序）
build/<preset>/lib/  静态库（libneko_base.a 等）
```

Windows 的 Visual Studio 生成器是多配置生成器：CMake 会在输出目录后再追加一层配置名，
因此产物在 `build/<preset>/bin/Release/`（Debug 构建则在 `bin/Debug/`）。构建 preset
已显式指定 `configuration`，所以 `cmake --build --preset release` 在 Windows 上确实
构建 Release 配置；Linux/macOS 的单配置生成器忽略该字段。

## 常见问题

- **构建时卡在 googletest-populate**：网络无法访问 GitHub。参考上文"依赖获取"。
- **`-Werror` 构建失败**：修复警告本身，不要静默降级警告（见 AGENTS.md §13）。
