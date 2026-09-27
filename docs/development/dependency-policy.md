# 依赖策略

## 原则

1. **优先自研核心**：浏览器引擎核心（网络协议解析、HTML/CSS parser、DOM、
   布局、渲染、安全模型）必须由本项目实现，不得交给第三方库。
2. **基础设施可用三方库**：测试框架、TLS、字体、图形、压缩等通用基础设施
   允许使用成熟三方库。
3. **必须封装**：任何三方库都必须封装在项目自己的接口之后，核心代码不得直接
   依赖三方 API 类型。
4. **不因省事引入**：能用 ~100 行项目代码写清的琐碎能力，不要引入依赖。
5. **不重造安全关键轮子**：TLS/加密必须用成熟实现（OpenSSL/mbedTLS/BoringSSL）。

## 每个依赖必须说明

| 项目 | 说明 |
| --- | --- |
| 为什么需要 | 解决什么具体问题 |
| 替代方案 | 自研或其他库的成本对比 |
| 许可证 | 与项目（Unlicense）是否兼容 |
| 维护状况 | 活跃度、安全公告响应 |
| 平台支持 | Linux/Windows/macOS |
| 安全风险 | 历史 CVE、攻击面 |
| 版本策略 | 固定版本 + 校验和 |

## 当前依赖清单

| 依赖 | 版本 | 用途 | 引入方式 | 许可证 |
| --- | --- | --- | --- | --- |
| GoogleTest | v1.15.2 | 单元测试框架 | FetchContent（固定 SHA256） | BSD-3-Clause |
| zlib | 系统包 | PNG IDAT / PDF FlateDecode / HTTP gzip-deflate 解压 | find_package | zlib |
| libjpeg | 系统包 | JPEG 解码（封装在 neko::image 后） | find_package | BSD-like |
| libwebp | 系统包 | WebP 解码（封装在 neko::image 后） | find_package | BSD-3-Clause |
| libavif | 系统包 | AVIF 解码（封装在 neko::image 后；**必须带 AV1 解码器**） | find_package | BSD-2-Clause |
| QuickJS (quickjs-ng) | v0.16.1 | JavaScript runtime（封装在 neko::javascript 后） | FetchContent（固定 SHA256） | MIT |
| Qt6 Widgets | 系统包 | GUI 基础设施（窗口/事件/控件，见 ADR 0006） | find_package | LGPL |
| FreeType | 系统包 | 字体光栅化（封装在 neko::graphics 后，见 ADR 0009） | find_package | FTL（双许可选 FTL） |
| OpenSSL | 系统包 | TLS/HTTPS（封装在 neko::network::TlsSocket 后，见 ADR 0010） | find_package | Apache-2.0 |
| FFmpeg | 6.1（系统包） | 视频解复用/解码/像素转换（封装在 neko::media 后，见 ADR 0014） | find_package（pkg-config，缺失时回退到头文件/库搜索） | LGPL-2.1-or-later |

> **libwebp 说明**：WebP 是当前 web 内容（尤其 Bing 等站点壁纸）的主要图片格式。
> 自研 VP8/VP8L 解码器成本高且非本项目核心，因此封装 libwebp（BSD-3-Clause，
> Google 维护，Linux/Windows/macOS 全平台，安全更新活跃）。封装在
> `neko::image` 之后，接口与 PNG/JPEG/GIF 解码器一致。

> **libavif 说明**：AVIF 容器解析由 libavif 提供，AV1 位流解码由它链接的
> 解码器（dav1d / aom）完成。**libavif 自身不带解码器**：发行版包
> （Debian `libavif-dev`、Homebrew `libavif`）默认启用，但 vcpkg 的
> `libavif` 端口没有 default-features，必须显式选择，否则解码在运行期返回
> `AVIF_RESULT_NO_CODEC_AVAILABLE`（历史上表现为不透明的
> “avif: decode failed”，2026-09 Windows 发布的 `AvifTest` 失败即此原因）：
>
> ```powershell
> vcpkg install 'libavif[dav1d]'   # 或 libavif[aom]
> ```
>
> 解码器缺失是**环境配置**问题，不改动测试期望：`AvifTest` 用真实 AV1
> 无损夹具覆盖解码，是这类配置错误的守门测试。解码失败信息里带
> `avifResultToString()` 的结果文本，便于一眼区分“坏文件”与“缺解码器”。

> **FFmpeg 说明**：视频解复用/解码（MP4/WebM、H.264/VP8/VP9 等）自研不现实
> 且非本项目核心，故封装 FFmpeg（见 ADR 0014）。**LGPL 约束**：仅动态链接
> 发行版构建（默认配置不含 GPL 组件），禁止链接 `libx264` 等 GPL 编解码器；
> 如未来需静态链接，须随发行提供重链接目标文件（LGPL §4）。FFmpeg 头文件
> 不越过 `src/media/src/video.cpp`。
>
> 发现方式：`cmake/FindFFmpeg.cmake` 先试 pkg-config（Linux/macOS），失败则
> 回退到 `find_path`/`find_library`（Windows/vcpkg 没有 pkg-config 程序）。
> 两条路径都暴露同一个导入目标 `FFmpeg::FFmpeg`（`PkgConfig::FFMPEG` 不再是
> 公共接口），`src/media/CMakeLists.txt` 只链接前者。

## 发布产物的链接方式（ADR 0019）

发布产物以「下载即用」为目标，链接策略按平台取舍：

- **Windows**：zlib / libjpeg-turbo / libwebp / FreeType / OpenSSL / libavif
  用 vcpkg 静态三元组（`*-windows-static-md`）静态链接；
  **FFmpeg 保持动态**（LGPL 重链接义务 + 发行构建的 GPL 组件风险），
  DLL 与官方动态 Qt（`windeployqt` 部署）、MSVC 运行库随包分发。
- **Linux / macOS**：第三方库保持动态并**随包捆绑**（改写 rpath /
  install name）；glibc 与系统框架必须来自目标机（macOS 上无法静态，
  Linux 上静态 glibc 会破坏 NSS/DNS）。
- 捆绑的 FFmpeg 运行库来自发行版/Homebrew 构建，可能包含其启用的 GPL
  组件；后续工作是为发布构建自有 LGPL 运行时。捆绑范围与校验脚本见
  [ADR 0019](../architecture/adr/0019-release-runtime-packaging.md) 与
  `tools/package_runtime_{linux,macos}.sh`。

## 三方头文件与警告

项目的严格警告集（`-Wall -Wextra -Wpedantic -Wconversion -Wold-style-cast …`，
CI 中配合 `-Werror`）只适用于项目自身代码，绝不作用于三方头文件。

CMake 会把导入目标（`JPEG::JPEG`、`OpenSSL::Crypto`、`FFmpeg::FFmpeg` 等）
的包含目录标记为 SYSTEM，但对**编译器默认搜索目录**会丢掉该标记
（`CMAKE_<LANG>_IMPLICIT_INCLUDE_DIRECTORIES`）。macOS 上 Homebrew 前缀
`/usr/local/include`（Intel）与 `/opt/homebrew/include`（Apple Silicon）即属于此类：
三方头文件会经由编译器默认搜索路径被找到，某些工具链将其归为“用户目录”，
于是 libjpeg 的 `jpeg_create_decompress` 宏、OpenSSL 的 `safestack.h` 等会在
`-Werror` 下报错（macOS CI 实际出现过该失败）。

处理方式：顶层 `CMakeLists.txt` 汇总 `NEKO_THIRD_PARTY_INCLUDE_DIRS`，
`neko_filter_third_party_include_dirs()`（`cmake/CompilerWarnings.cmake`）筛出
`NEKO_THIRD_PARTY_SYSTEM_INCLUDE_DIRS`，`apply_compiler_warnings()` 在 Apple 平台
为每个目标显式补回 `-isystem <依赖目录>`；其他平台上编译器本就将其默认目录视为
系统目录，因此该处理为空操作。实测（clang 18）：`CPATH` 目录会被 CMake 判为“隐式
目录”而丢掉 SYSTEM 标记，同时被 clang 当作普通用户目录搜索（宏展开的
`-Wold-style-cast` 因此落到我们的翻译单元），此时补回的 `-isystem` 确实能压住它。

**例外（绝不补回）**：提供 C 标准库头文件的目录（含 `stddef.h` 或 `stdint.h`）。
用户 `-isystem` 目录排在**所有**搜索路径之前（实测 clang 18：`-isystem /usr/include`
把 `/usr/include` 提到 `<c++/v1>` 之前），重新标记这类目录会让 libc++ 的 C 兼容头
（`<c++/v1/stddef.h>`、`<c++/v1/stdint.h>`）被遮蔽，`<cstddef>` / `<cstdint>` 以

```text
<cstddef> tried including <stddef.h> but didn't find libc++'s <stddef.h> header.
```

直接失败——macOS CI 就是这样挂掉的。该失败要求 `-isystem` 名单里存在一个提供
`stddef.h`/`stdint.h` 的目录，而 macOS 上唯一可能是 SDK 自带的 C 头目录
（`<sysroot>/usr/include`）：某个 `find_package()` 在 keg-only 依赖（例如 Homebrew 的
openssl，不在 `/opt/homebrew/include` 里）或 SDK 自身满足它时会返回该目录。这类目录
本就由编译器以系统方式搜索，跳过它没有任何损失。配置阶段会打印每一个被剔除的目录，
因此具体是哪个依赖可以直接从日志确认；规则由 ctest 用例
`build_system.third_party_include_filter`
（`tests/cmake/third_party_include_filter_test.cmake`）锁定。

注意：用 `#pragma clang diagnostic ignored` 包裹 `#include` 只能屏蔽头文件自身的代码，
**无法**屏蔽落到我们翻译单元调用点的宏展开（已用 clang 实验验证），因此不得以
pragma 替代该机制。

新增三方依赖时：目录发现集中在顶层 `CMakeLists.txt`，请同步把新的包含目录
加入 `NEKO_THIRD_PARTY_INCLUDE_DIRS`。

## 未来候选依赖（引入时逐个评估）

- ICU（Unicode）
- HarfBuzz（文本整形）
- brotli（HTTP 压缩）
- SQLite（存储）
- fmt / spdlog（仅当性能分析证明需要时，见 ADR 0004）

## 引入流程

1. 说明理由（上表 7 项）
2. 写 ADR
3. 封装在项目接口后
4. 固定版本 + 校验和
5. 更新本文档
