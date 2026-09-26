# 技术文档索引

neko-browser 的文档体系。文档是项目的一等公民 —— 改动架构、公共 API、模块职责
时必须同步更新对应文档。

## 架构

- [总体架构](architecture/architecture.md) —— 模块划分、依赖方向、数据流
- [线程、进程与异步 I/O 模型](architecture/threading-model.md) —— 线程归属、
  同步机制、进程隔离与 I/O 现状（新增后台线程前必须登记于此）
- [架构决策记录（ADR）](architecture/adr/)
  - [0001：使用 CMake 构建系统](architecture/adr/0001-cmake-build-system.md)
  - [0002：C++20 标准基线](architecture/adr/0002-cpp20-baseline.md)
  - [0003：GoogleTest 经 FetchContent 固定版本](architecture/adr/0003-googletest-fetchcontent.md)
  - [0004：自研日志与 Error/Result 模型](architecture/adr/0004-own-logging-and-result.md)
  - [0005：位图字体文本渲染](architecture/adr/0005-bitmap-font-rendering.md)
  - [0006：GUI 使用 Qt6](architecture/adr/0006-qt6-gui.md)
  - [0007：PNG 自研解码 + JPEG 封装 libjpeg](architecture/adr/0007-png-decoder.md)
  - [0008：JS runtime 接入 QuickJS](architecture/adr/0008-quickjs-runtime.md)
  - [0009：文本渲染迁移到 FreeType](architecture/adr/0009-freetype-font-rendering.md)
  - [0010：HTTPS/TLS 采用 OpenSSL](architecture/adr/0010-tls-openssl.md)
  - [0011：security 子系统起步（Origin / SOP）](architecture/adr/0011-security-origin.md)
  - [0012：字符编码采用 WHATWG Encoding](architecture/adr/0012-character-encoding.md)
  - [0013：渲染性能——缓存与并行栅格化](architecture/adr/0013-renderer-caching-parallelism.md)
  - [0014：视频解码采用 FFmpeg](architecture/adr/0014-video-ffmpeg.md)
  - [0015：软件合成器抽象层](architecture/adr/0015-software-compositor.md)
  - [0016：多进程架构](architecture/adr/0016-multiprocess-architecture.md)
  - [0017：GPU 合成框架](architecture/adr/0017-gpu-compositor-framework.md)
  - [0018：内置 DNS 解析器](architecture/adr/0018-built-in-dns-resolver.md)
  - [0019：发布产物运行时打包（静态链接 + 运行库捆绑）](architecture/adr/0019-release-runtime-packaging.md)

## 开发

- [开发路线图](development/roadmap.md) —— 分阶段计划与里程碑
- [依赖策略](development/dependency-policy.md)
- [编码风格](development/coding-style.md)
- [Sanitizer 使用](development/sanitizers.md)

## 测试与安全

- [测试策略](testing/strategy.md)
- [安全模型](security/security-model.md)
- [兼容性矩阵](compatibility/compatibility-matrix.md)

## 模块文档

| 模块 | 状态 | 文档 |
| --- | --- | --- |
| Network | Implemented (Phase 2 核心 + HTTPS + 压缩) | [networking](networking/README.md) |
| HTML | Implemented (子集) | [html](html/README.md) |
| CSS | Implemented (子集) | [css](css/README.md) |
| Layout | Implemented (子集，含 Flexbox M1–M5) | [layout](layout/README.md) |
| Rendering | Implemented (子集) | [rendering](rendering/README.md) |
| Storage | Implemented (子集，含 LocalStorage) | 见 [总体架构](architecture/architecture.md) |
| Image / Media / PDF | Partial（Image 含 PNG/JPEG/GIF） | 见 [总体架构](architecture/architecture.md) |
| GUI (Qt6) | Partial | 见 [总体架构](architecture/architecture.md) |
| JavaScript | Partial (runtime) | [javascript](javascript/README.md) |

## 发布

- [发布说明](releases/README.md)
