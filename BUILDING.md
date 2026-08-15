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
- **系统包**：zlib、libjpeg、libwebp（图像解码）、FreeType（字体光栅化）、
  OpenSSL（HTTPS/TLS）、Qt6 Widgets（GUI，可选；`NEKO_BUILD_UI=OFF` 可跳过）。
  - Debian/Ubuntu：`sudo apt install zlib1g-dev libjpeg-dev libwebp-dev libfreetype-dev libssl-dev qt6-base-dev`
  - macOS（Homebrew）：`brew install jpeg webp freetype qt openssl`
  - Windows（vcpkg）：`vcpkg install zlib libjpeg-turbo libwebp freetype openssl`，配置时传入
    `-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake`；
    Qt6 GUI 在 Windows 上默认不构建（`NEKO_BUILD_UI=OFF`）。
- 参见 [dependency-policy.md](docs/development/dependency-policy.md)。
- 离线或受限网络环境：可预先下载 tarball 并设置
  `CMAKE_FETCHCONTENT_SOURCE_DIR_GOOGLETEST` / `CMAKE_FETCHCONTENT_SOURCE_DIR_QUICKJS`
  指向解压目录。

## 产物位置

所有产物统一输出到构建目录下：

```text
build/<preset>/bin/  可执行文件（neko_browser、测试程序）
build/<preset>/lib/  静态库（libneko_base.a 等）
```

## 常见问题

- **构建时卡在 googletest-populate**：网络无法访问 GitHub。参考上文"依赖获取"。
- **`-Werror` 构建失败**：修复警告本身，不要静默降级警告（见 AGENTS.md §13）。
