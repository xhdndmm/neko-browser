# neko-browser

> **状态：首个浏览器里程碑达成（Phases 1–6）**
> 一个从零开始、真正可编译可运行的**跨平台浏览器引擎与浏览器应用**，主要使用现代 C++20 编写。

本项目不是 WebView 封装、不是截图工具、也不是玩具 HTML 渲染器。它已经能用
**自研引擎**完整跑通 `URL → HTTP → HTML → DOM → CSS → Style → Layout → Paint →
Rasterization` 渲染管线，并能抓取、解析、渲染**真实网站**。

---

## 当前进度

引擎核心纵向切片已可工作（Linux/GCC、Clang 均验证）：

```bash
# 抓取并解析真实网页
./build/debug/bin/neko_browser --url http://example.com/ --dump-dom

# 渲染为图片（PPM）
./build/debug/bin/neko_browser --url http://example.com/ --screenshot out.ppm
```

已实现（均有单元测试，158 个测试全绿）：

- **URL**：解析、相对解析（RFC 3986 5.4.1 样例）、百分号编码、Origin
- **网络**：TCP Socket（POSIX）、HTTP/1.1 GET、重定向、chunked、Content-Length
- **HTML**：WHATWG 风格 tokenizer + 树构建（插入模式）、字符引用、畸形输入容错
- **DOM**：Node/Element/Text/Comment/Document、树操作、querySelector
- **CSS**：tokenizer/parser、选择器匹配与特异性、级联、@media
- **Style**：UA 样式表 + 级联 + 继承 + 计算样式（em/rem/百分比解析）
- **Layout**：盒模型、block/inline 布局、文字换行、relative 定位
- **Paint**：显示列表 + 软件光栅化 + 8x8 位图字体 + PPM 输出
- **CLI**：`--url` / `--dump-dom` / `--screenshot` / `--headless` 等

> **诚实声明**：JavaScript、TLS/HTTPS、flexbox/grid、图像解码、GPU 合成、多进程、
> GUI 窗口均 **尚未实现**（见[兼容性矩阵](docs/compatibility/compatibility-matrix.md)）。
> 文本渲染当前使用内嵌公有领域 8x8 位图字体（仅 ASCII，无整形/Unicode 回退）。

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
  browser/              CLI 与浏览器应用
tests/
  unit/                 GoogleTest 单元测试（按模块）
docs/                   架构、设计、开发、测试文档
tools/                  开发脚本（format / check）
.github/workflows/      CI
```

## 许可证

[Unlicense](LICENSE) —— 公有领域，详见仓库 LICENSE 文件。