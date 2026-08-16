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
| QuickJS (quickjs-ng) | v0.16.1 | JavaScript runtime（封装在 neko::javascript 后） | FetchContent（固定 SHA256） | MIT |
| Qt6 Widgets | 系统包 | GUI 基础设施（窗口/事件/控件，见 ADR 0006） | find_package | LGPL |
| FreeType | 系统包 | 字体光栅化（封装在 neko::graphics 后，见 ADR 0009） | find_package | FTL（双许可选 FTL） |
| OpenSSL | 系统包 | TLS/HTTPS（封装在 neko::network::TlsSocket 后，见 ADR 0010） | find_package | Apache-2.0 |
| FFmpeg | 6.1（系统包） | 视频解复用/解码/像素转换（封装在 neko::media 后，见 ADR 0014） | find_package（pkg-config） | LGPL-2.1-or-later |

> **libwebp 说明**：WebP 是当前 web 内容（尤其 Bing 等站点壁纸）的主要图片格式。
> 自研 VP8/VP8L 解码器成本高且非本项目核心，因此封装 libwebp（BSD-3-Clause，
> Google 维护，Linux/Windows/macOS 全平台，安全更新活跃）。封装在
> `neko::image` 之后，接口与 PNG/JPEG/GIF 解码器一致。

> **FFmpeg 说明**：视频解复用/解码（MP4/WebM、H.264/VP8/VP9 等）自研不现实
> 且非本项目核心，故封装 FFmpeg（见 ADR 0014）。**LGPL 约束**：仅动态链接
> 发行版构建（默认配置不含 GPL 组件），禁止链接 `libx264` 等 GPL 编解码器；
> 如未来需静态链接，须随发行提供重链接目标文件（LGPL §4）。FFmpeg 头文件
> 不越过 `src/media/src/video.cpp`。

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
