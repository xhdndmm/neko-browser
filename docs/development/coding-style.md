# 编码风格

> 自动格式化以 `.clang-format` 为准；静态检查以 `.clang-tidy` 为准。
> 本文档说明风格背后的**理由**与规则。

## 基础

- C++20，不用 C++ 扩展（`CMAKE_CXX_EXTENSIONS=OFF`）。
- 命名空间 `neko::<module>`，头文件位于 `src/<module>/include/neko/<module>/`。
- 文件头使用 `#pragma once`。
- 缩进 2 空格，列宽 100（由 clang-format 强制执行）。

## 命名

| 类别 | 约定 | 示例 |
| --- | --- | --- |
| 类型 / 类 / 枚举 | `PascalCase` | `class HtmlParser` |
| 枚举值 | `kCamelCase`（前缀 k） | `LogLevel::kInfo` |
| 函数 / 方法 | `CamelCase` | `ParseLogLevel()` |
| 变量 / 参数 | `snake_case` | `result` / `file_path` |
| 成员变量 | 尾随下划线 | `message_` |
| 宏 / 常量 | `UPPER_SNAKE` | `NEKO_LOG_INFO` |
| 命名空间 | 小写 | `neko::base` |

## 所有权与生命周期

- 默认 `std::unique_ptr`；非拥有引用用裸指针/引用；`std::shared_ptr` 仅当
  所有权确实共享。
- 禁止隐式所有权转移；跨模块传递大对象用 move。
- 引用环：DOM/布局等树形结构严禁反向拥有，父持子、子存非拥有父指针。

## 错误处理

- 预期失败用 `Result<T>` / `Status`，**不用异常**（解析、IO、网络、安全）。
- 编程错误（不变量被破坏）用 `NEKO_ASSERT`（Release 编译掉）或显式检查。
- 禁止静默吞错：要么传播、要么记录日志 + 明确的降级行为。
- `NEKO_TRY(...)`（GCC/Clang）可提前返回错误；MSVC 用显式两步模式。

## 并发

- 不引入隐式锁；共享可变状态必须配文档化的同步机制。
- 优先消息/任务传递，而非裸共享内存 + 锁。

## 注释

- 注释解释 **WHY**，不解释 **WHAT**。
- 公共 API 必须有简要注释说明职责与错误语义。
- 复杂不变量、线程模型、生命周期在代码中注明。

## TODO

- 允许，但必须：有原因、有 issue 引用、不掩盖当前正确性。
- 禁止 `// TODO implement everything` 类垃圾。

## 伪实现禁令

- 禁止用 `return true;` / 硬编码 / 固定字符串 / 空函数充当实现。
- 未完成功能必须标注 `NOT IMPLEMENTED` / `PARTIALLY IMPLEMENTED`。

## 静态检查

- CI 强制 clang-format 检查与 `-Werror`。
- clang-tidy 实验性运行（见 .github/workflows/static-analysis.yml）。
