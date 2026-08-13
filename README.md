# neko-browser

> **状态：Phase 0 — 项目引导（Project Bootstrap）**
> 一个从零开始、真正可编译可运行的**跨平台浏览器引擎与浏览器应用**，主要使用现代 C++20 编写。

本项目不是 WebView 封装、不是截图工具、也不是玩具 HTML 渲染器。目标是逐步构建一个具有现代浏览器架构的**真实浏览器引擎**。

---

## 当前进度（Phase 0）

Phase 0 已经完成，仓库现在可以：

```bash
git clone <repo>
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

全部成功。当前包含：

- CMake 工程（C++20、严格警告、GCC/Clang/MSVC 兼容）
- 8 个构建 Preset：`debug` / `release` / `relwithdebinfo` / `asan` / `ubsan` / `tsan` / `coverage`
- 基础库 `neko::base`：日志系统、Error/Result 模型、字符串工具、版本与断言
- CLI 可执行文件 `neko_browser`：`--help` / `--version` / `--headless` / `--url` 等参数解析
- GoogleTest 单元测试（54 个测试，全绿）
- clang-format / clang-tidy 配置
- GitHub Actions CI（Linux GCC / Linux Clang / Windows MSVC / macOS Clang / ASan / Coverage / 格式检查）

> **诚实声明**：浏览器引擎核心（网络、HTML、DOM、CSS、布局、渲染、JavaScript）**尚未实现**。
> `neko_browser` 目前只验证 CLI、日志与构建管线。这是设计使然 —— 参见 [开发路线图](docs/development/roadmap.md)。

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
./build/debug/bin/neko_browser --url https://example.com --headless
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
  base/                 基础库：日志、Error/Result、字符串、版本
  browser/              CLI 入口与浏览器应用
tests/
  unit/                 GoogleTest 单元测试
docs/                   架构、设计、开发、测试文档
tools/                  开发脚本（format / check）
.github/workflows/      CI
```

## 许可证

[Unlicense](LICENSE) —— 公有领域，详见仓库 LICENSE 文件。