# 架构决策记录 0008：JavaScript 运行时接入 QuickJS（quickjs-ng）

- 状态：**Accepted**（2026-08）
- 决策者：架构组 + 用户

## 背景

Phase 8 需要 JavaScript 执行能力。候选方案：

1. **自研 JS 引擎**（lexer/parser/AST/bytecode/VM/GC）—— 工作量巨大（数年量级），
   会严重拖延后续 Web API / 安全 / 多进程里程碑。
2. **QuickJS（quickjs-ng v0.16.1）** —— 轻量、可嵌入、C 语言单进程、
   ES2025 接近完整、启动快、内存占用小、MIT 许可；quickjs-ng 是积极维护的
   fork，提供原生 CMake 支持（FetchContent 友好）。
3. **V8 / JavaScriptCore / SpiderMonkey** —— 功能最强但构建链极其庞大、
   内存与启动开销大、嵌入成本高，不适合本项目的自研路线。
4. **Duktape / JerryScript** —— 偏嵌入式 MCU 场景，ES 支持较弱。

## 决策

- JavaScript runtime 采用 **QuickJS（quickjs-ng v0.16.1）**，经 FetchContent
  URL+SHA256 固定版本构建（与 GoogleTest 同模式）。
- 封装为 `neko::javascript` 模块（`ScriptEngine` / `ScriptValue`），
  **第三方头文件绝不泄漏到公共 API**；QuickJS 仅作 runtime，
  **不得**替代 DOM/CSS/布局/渲染/导航/安全/存储。
- 安全边界：
  - `QJS_BUILD_LIBC=OFF` —— 不编译 QuickJS 的 `std`/`os` 模块
    （文件/进程/网络访问全部不可用），仅提供项目自有的 `console` 绑定。
  - 默认内存上限 128 MiB（`JS_SetMemoryLimit`），可配置。
  - 默认执行时限 10 秒（中断处理器 + `steady_clock`，死循环可中止）。
- 集成点（里程碑 1）：
  - CLI `--eval <script>`（headless 求值）。
  - GUI DevTools Console 的持久 REPL（worker 线程求值，不阻塞 UI）。
- 语法错误映射为 `Error::Parse`，运行时/超时/内存错误映射为 `Error::Javascript`
  （base 新增错误类别）。

## 后果

- 优点：ES2025 核心语言能力立即可用；构建可复现；安全边界清晰。
- 缺点：JS 引擎非自研（长期路线仍保留自研 VM 研究，见
  docs/javascript/README.md）；quickjs-ng 需在配置时下载（与 GTest 相同）。
- 后续：Web IDL / DOM 绑定（里程碑 2）、页面 `<script>` 执行、事件循环
  （Promise/microtask 与浏览器事件循环的对接）。
