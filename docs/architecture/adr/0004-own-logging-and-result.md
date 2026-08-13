# ADR 0004：自研日志系统与 Error/Result 模型

- 状态：**Accepted**（2026-08）
- 决策者：架构组

## 背景

浏览器引擎需要全项目统一的日志与错误处理模型。第三方库（spdlog、fmt、
tl::expected）都成熟，但项目规则要求：*"不要为了省事引入大量依赖；不要为了
避免写琐碎项目代码而添加依赖"*。日志与错误模型是**核心基础设施**，属于项目
应当自有的部分，且需要与项目语义（ErrorCategory 分类、浏览器上下文）深度绑定。

## 决策

- **日志**：自研轻量日志系统（`neko::base`）：
  - 级别：TRACE/DEBUG/INFO/WARN/ERROR/FATAL
  - sink 抽象：ConsoleLogSink、FileLogSink，可扩展
  - 进程级单例 Logger，线程安全（互斥 + 原子级别）
  - 宏：`NEKO_LOG_*` / `NEKO_LOGF`（基于 `std::format`）
- **错误模型**：自研 `Error` / `Result<T>` / `Status`（`Result<void>`）：
  - 显式错误分类（InvalidArgument/Io/Network/Parse/Security/...）
  - `Result<T>` 为 value-or-error 变体，`[[nodiscard]]` 强制处理
  - 预期失败用 Result（解析、IO、网络），不用异常
  - `NEKO_TRY` 宏（GCC/Clang 语句表达式；MSVC 上明确不可用）

## 备选方案

- **spdlog / fmt**：成熟；但引入两个外部依赖只为约 300 行日志代码，且把核心
  基础设施绑在三方 API 上。将来如需结构化/高性能日志，可评估再引入并加 ADR。
- **std::expected（C++23）**：见 ADR 0002，编译器支持未齐；自研 Result 现在
  就能提供一致语义，将来可平滑迁移。

## 后果

- 优点：零额外依赖、语义与项目绑定、可控、可测试（单元测试覆盖日志路由、
  级别过滤、文件输出；Result 语义全覆盖）。
- 缺点：自研代码需要维护与测试（已由 tests/unit/base 覆盖）。
- 后续：若性能分析证明需要异步/批量日志，再评估 spdlog 并更新本 ADR。

## 参考

- AGENTS.md §35（依赖政策）、§37（错误处理）
