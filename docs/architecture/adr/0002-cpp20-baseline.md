# ADR 0002：C++20 标准基线

- 状态：**Accepted**（2026-08）
- 决策者：架构组

## 背景

浏览器引擎对性能、确定性、资源控制要求高，需要 C++ 的零开销抽象；同时要求
GCC/Clang/MSVC 三编译器可移植。

## 决策

- 主语言标准：**C++20**（`-std=c++20`，`CMAKE_CXX_EXTENSIONS=OFF`）。
- 全面使用 RAII、值语义、智能指针、move 语义、const 正确性、强类型、
  标准容器与算法。
- 阶段性采用 `std::format`（GCC 13 / Clang 15 / MSVC 19.3x 均支持）。
- 不使用 C++23 特性（如 `std::expected`），除非有明确收益且三编译器都支持；
  届时先写 ADR。
- 工程内自有 `Result<T>` / `Error` 模型（见 ADR 0004），不依赖 `std::expected`。

## 备选方案

- **C++17**：缺少 `std::format`、三路比较、concepts 等，写起核心基础设施更繁琐。
- **C++23**：编译器支持尚不齐（尤其 MSVC 部分特性），生态未稳定。

## 后果

- 优点：现代、安全、可读的代码；三编译器支持成熟。
- 缺点：仍需显式管理 ownership 与生命周期 —— 这是浏览器引擎的固有复杂度，
  标准版本无法消除。

## 参考

- https://en.cppreference.com/w/cpp/20
