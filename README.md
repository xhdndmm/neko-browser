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
```

已实现（均有单元测试，247 个测试全绿，含 ASan）：

- **URL**：解析、相对解析（RFC 3986 5.4.1 样例）、百分号编码、Origin
- **网络**：TCP Socket（POSIX）、HTTP/1.1 GET、重定向、chunked、Content-Length
- **HTML**：WHATWG 风格 tokenizer + 树构建（插入模式）、字符引用、畸形输入容错
- **DOM**：Node/Element/Text/Comment/Document、树操作、querySelector
- **CSS**：tokenizer/parser、选择器匹配与特异性、级联、@media
- **Style**：UA 样式表 + 级联 + 继承 + 计算样式（em/rem/百分比解析）
- **Layout**：盒模型、block/inline 布局、文字换行、relative 定位
- **Paint**：显示列表 + 软件光栅化 + 8x8 位图字体 + PPM 输出
- **存储**：Cookie（RFC 6265 子集）、历史、书签 —— 自研行式文件 + 原子写入
- **图像**：自研 PNG 解码器（chunk/CRC/滤波/Adam7/全部颜色类型）+ libjpeg 封装
- **媒体**：自研 WAV 解码（PCM+float，8/16/24/32-bit）
- **PDF**：文本提取（xref/FlateDecode/文本操作符）—— **PARTIAL**
- **GUI（Qt6）**：标签页、地址栏、后退/前进/刷新/新标签/书签/下载、DevTools
  （DOM 树/网络日志/Console）、历史/书签/下载/设置面板
- **下载器**：Content-Disposition/URL 文件名、原子写入
- **CLI**：`--url` / `--dump-dom` / `--screenshot` / `--dump-history` /
  `--dump-bookmarks` / `--show-cookies` / `--download` / `--extract-pdf` /
  `--audio-info` / `--image-info` 等

> **诚实声明**：JavaScript、TLS/HTTPS、flexbox/grid、**视频解码**、GIF/WebP/
> GPU 合成、多进程、LocalStorage/IndexedDB 均 **尚未实现**
> （见[兼容性矩阵](docs/compatibility/compatibility-matrix.md)）。
> PDF 仅文本提取（无渲染）；文本渲染当前使用内嵌公有领域 8x8 位图字体
> （仅 ASCII，无整形/Unicode 回退）。

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
cmake --build --preset debug
ctest --preset debug

# 或一条命令
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
  pdf/                  PDF 文本提取
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