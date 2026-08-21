# neko-browser

> **状态：浏览器 UI 里程碑达成（Phases 1–7，含存储/图像/媒体/PDF）**
> 一个从零开始、真正可编译可运行的**跨平台浏览器引擎与浏览器应用**，主要使用现代 C++20 编写。

本项目不是 WebView 封装、不是截图工具、也不是玩具 HTML 渲染器。它已经能用
**自研引擎**完整跑通 `URL → HTTP → HTML → DOM → CSS → Style → Layout → Paint →
Rasterization` 渲染管线，能抓取、解析、渲染**真实网站**，并附带 Qt6 GUI、
历史/书签/Cookie 持久化、下载器、PNG/JPEG 图像解码、WAV 音频、PDF 文本提取
与 DevTools。

---

## 当前进度

引擎核心纵向切片已可工作（Linux/GCC、Clang 均验证）：

```bash
# 抓取并解析真实网页
./build/debug/bin/neko_browser --url http://example.com/ --dump-dom

# 渲染为图片（PPM）
./build/debug/bin/neko_browser --url http://example.com/ --screenshot out.ppm

# 图形界面（Qt6）
./build/debug/bin/neko_gui

# 无头截图（offscreen）
./build/debug/bin/neko_gui_screenshot http://example.com/ out.png

# 存储与内容解析（headless）
./build/debug/bin/neko_browser --dump-history --profile ~/.neko-browser
./build/debug/bin/neko_browser --dump-bookmarks --profile ~/.neko-browser
./build/debug/bin/neko_browser --show-cookies --profile ~/.neko-browser
./build/debug/bin/neko_browser --download http://example.com/ --download-dir /tmp/dl
./build/debug/bin/neko_browser --extract-pdf doc.pdf
./build/debug/bin/neko_browser --audio-info clip.wav
./build/debug/bin/neko_browser --image-info pic.png --image-out pic.ppm
./build/debug/bin/neko_browser --eval "Math.max(1, 5, 3)"
./build/debug/bin/neko_browser --eval "console.log('hi'); JSON.stringify({a:1})"
```

已实现（均有单元测试，277 个测试全绿，含 ASan）：

- **URL**：解析、相对解析（RFC 3986 5.4.1 样例）、百分号编码、Origin
- **网络**：TCP Socket（POSIX）、HTTP/1.1 GET、重定向、chunked、Content-Length
- **HTML**：WHATWG 风格 tokenizer + 树构建（插入模式）、字符引用、畸形输入容错
- **DOM**：Node/Element/Text/Comment/Document、树操作、querySelector
- **CSS**：tokenizer/parser、选择器匹配与特异性、级联、@media
- **Style**：UA 样式表 + 级联 + 继承 + 计算样式（em/rem/百分比解析）
- **Layout**：盒模型、block/inline 布局、文字换行、relative 定位
- **Paint**：显示列表 + 软件光栅化 + 8x8 位图字体 + PPM 输出
- **合成器**：软件合成器抽象层（ADR 0015，`Surface` + `Compositor` 接口，
  CPU 实现，图层/alpha/脏矩形/滚动 blit；GUI 已接线：页面层 + caret 覆盖层）
  —— GPU 后端尚未实现
- **存储**：Cookie（RFC 6265 子集）、历史、书签 —— 自研行式文件 + 原子写入；
  **IndexedDB**：版本化数据库（open/onupgradeneeded）、对象存储
  （keyPath/autoIncrement）、事务 + add/put/get/delete/clear/count/getAll、
  微任务回调 —— **PARTIAL**（无游标/索引）
- **图像**：自研 PNG 解码器（chunk/CRC/滤波/Adam7/全部颜色类型）+ 自研 GIF 解码器
  （LZW/交错/透明/disposal + **动画**：全帧解码、循环次数、延迟钳制，页面内 `<img>`
  与背景图由 50ms 帧时钟驱动播放）+ libjpeg/libwebp/libavif 封装（JPEG/WebP/**AVIF**）
- **媒体**：自研 WAV 解码（PCM+float，8/16/24/32-bit）；
  **视频**：FFmpeg 解复用/解码（MP4/WebM、H.264/VP9 等，LGPL 动态链接，
  ADR 0014）封装在 `media::MediaSource`/`DecodeVideo` 后，并已接入
  **`<video>` 元素**（`<video src>` 子资源抓取+解码、首帧作为替换内容渲染、
  autoplay/loop 帧时钟播放、JS 子集 play()/pause()/currentTime/duration/
  paused）—— **PARTIAL**（无 controls/音轨/缓冲）
- **PDF**：文本提取 + **页面渲染**（矢量图形/描边/填充/文本、q-Q/cm 变换、xref
  stream 与对象流、/MediaBox 继承）—— **PARTIAL**
- **JavaScript**：QuickJS（quickjs-ng）runtime 封装 —— 核心语言 + console
  + 执行时限/内存上限 + **DOM 绑定与页内脚本执行**（`window === globalThis`、
  事件/CustomEvent、setTimeout 事件循环、fetch/localStorage/indexedDB、matchMedia、
  **`<script type="module">` ES 模块执行**（静态 import 走网络栈、相对解析、
  import.meta.url；无动态 import()/import maps）、innerText 等子集，
  详见[兼容性矩阵](docs/compatibility/compatibility-matrix.md)）
- **GUI（Qt6）**：标签页、地址栏、后退/前进/刷新/新标签/书签/下载、DevTools
  （DOM 树/网络日志/**JS Console REPL**）、历史/书签/下载/设置面板
- **下载器**：Content-Disposition/URL 文件名、原子写入
- **多进程（M1，ADR 0016）**：`neko::ipc`（帧协议 Channel + 跨平台 Subprocess）+
  **Renderer 子进程**（`--renderer-child` 独立地址空间跑完整页面管线，位图 + DOM
  经 IPC 回传，子进程崩溃不带走浏览器）；CLI `--renderer-process` 路由加载——
  **PARTIAL**（GUI 接入/Network/GPU 进程/沙箱未开始）
- **CLI**：`--url` / `--dump-dom` / `--screenshot` / `--dump-history` /
  `--dump-bookmarks` / `--show-cookies` / `--download` / `--extract-pdf` /
  `--audio-info` / `--image-info` 等

> **诚实声明**：**GPU 合成**（Compositor 缝已就位，仅有 CPU 软件实现）、
> **完整多进程架构**均 **尚未实现**（M1 已交付：Renderer 子进程 + IPC，仅
> CLI 接入；无沙箱/Network/GPU 进程，见 ADR 0016）；
> 视频解码已接入 FFmpeg（MP4/H.264、WebM/VP9 实测），`<video>` 元素
> 支持子集（播放/暂停/seek/duration，无 controls/音轨/缓冲）
> （见[兼容性矩阵](docs/compatibility/compatibility-matrix.md)）。
> JavaScript 为 QuickJS runtime + 常用 DOM 绑定子集（无完整 Web IDL、WebSocket/XHR 等；
> ES 模块已支持静态 import，动态 import()/import maps 未实现）；
> IndexedDB 为子集（无游标/索引；值走 JSON 克隆，无 Date/BinaryData）；
> PDF 渲染为子集（矢量路径填充/描边、变换、文本；无图像 XObject/裁剪/pattern/CMap）；
> flexbox 已支持 order/min-max/auto 外边距；
> grid 已支持 minmax()/命名线/命名区域/auto-flow/inline-grid；
> GIF 已支持动画（直接导航到 .gif 仍显示首帧）。

---

## 快速开始

### 依赖

- CMake ≥ 3.24
- 任一编译器：GCC ≥ 12 / Clang ≥ 15 / MSVC ≥ 19.3x（均需 C++20 支持）
- 构建工具：Ninja 或 Make（自动检测）

### 构建与测试

```bash
# 配置 + 构建 + 测试（Debug）
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug

# 或一条命令（构建步骤默认并行）
cmake --workflow --preset debug

# Release（含 LTO）
cmake --workflow --preset release

# ASan + UBSan
cmake --workflow --preset asan

# 覆盖率
cmake --workflow --preset coverage
```

详见 [BUILDING.md](BUILDING.md) 与 [TESTING.md](TESTING.md)。

### 运行 CLI

```bash
./build/debug/bin/neko_browser --version
./build/debug/bin/neko_browser --help
./build/debug/bin/neko_browser --url http://example.com/ --dump-dom
./build/debug/bin/neko_browser --url http://example.com/ --screenshot out.ppm
./build/debug/bin/neko_browser --url ./page.html --screenshot out.ppm
```

### 运行 GUI（Qt6）

```bash
# 需要 Qt6 基础包（Debian/Ubuntu: sudo apt install qt6-base-dev）
./build/debug/bin/neko_gui
# 指定 profile（默认：NEKO_PROFILE 环境变量或平台应用数据目录）
NEKO_PROFILE=~/.neko-browser ./build/debug/bin/neko_gui
```

GUI 提供：标签页、地址栏、后退/前进/刷新/新标签/书签/下载按钮，
DevTools 停靠面板（DOM 树 / 网络日志 / Console），以及历史 / 书签 / 下载 /
设置面板。无显示环境可用：

```bash
QT_QPA_PLATFORM=offscreen ./build/debug/bin/neko_gui_screenshot http://example.com/ out.png
```

## 文档导航

| 文档 | 说明 |
| --- | --- |
| [ARCHITECTURE.md](docs/architecture/architecture.md) | 总体架构、模块边界、依赖方向 |
| [roadmap.md](docs/development/roadmap.md) | 分阶段开发路线图与里程碑 |
| [BUILDING.md](BUILDING.md) | 构建指南 |
| [TESTING.md](TESTING.md) | 测试指南 |
| [CONTRIBUTING.md](CONTRIBUTING.md) | 贡献指南 |
| [SECURITY.md](SECURITY.md) | 安全策略与报告流程 |
| [docs/](docs/README.md) | 全部技术文档索引 |

## 目录结构

```text
cmake/                  CMake 工具模块（警告、sanitizer）
src/
  base/                 日志、Error/Result、字符串、UTF-8、版本
  url/                  URL 解析与 Origin
  network/              Socket + HTTP/1.1
  dom/                  DOM 树与查询
  html/                 HTML tokenizer/parser
  css/                  CSS tokenizer/parser/选择器/值
  style/                计算样式引擎
  layout/               布局引擎
  paint/                显示列表 + 光栅化 + 字体 + PPM
  renderer/             页面管线编排
  storage/              Cookie / 历史 / 书签 持久化
  image/                PNG 自研解码 + JPEG(libjpeg)
  media/                WAV 解码
  pdf/                  PDF 文本提取 + 页面渲染
  javascript/           QuickJS runtime 封装
  browser/              BrowserController + 下载器 + CLI
  ui/                   Qt6 GUI（neko_gui / neko_gui_screenshot）
tests/
  unit/                 GoogleTest 单元测试（按模块）
docs/                   架构、设计、开发、测试文档
tools/                  开发脚本（format / check）
.github/workflows/      CI
```

## 许可证

[Unlicense](LICENSE) —— 公有领域，详见仓库 LICENSE 文件。