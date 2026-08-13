# ADR 0001：使用 CMake 作为构建系统

- 状态：**Accepted**（2026-08）
- 决策者：架构组

## 背景

浏览器引擎是长期、跨平台（Linux/Windows/macOS）、多编译器（GCC/Clang/MSVC）的
大型 C++ 项目，需要一个可维护、可复现、被广泛支持的构建系统。

## 决策

- 使用 **CMake ≥ 3.24**。
- 使用 **CMakePresets.json** 管理构建配置（debug/release/relwithdebinfo/asan/
  ubsan/tsan/coverage），并提供 workflow preset 一键 配置+构建+测试。
- 平台相关逻辑集中在 `cmake/` 下的模块文件（CompilerWarnings、Sanitizers）。

## 备选方案

- **Bazel / Buck2**：构建能力更强，但对 Windows/MSVC 支持与生态成熟度不如 CMake，
  学习成本高。
- **Meson**：简洁，但大型跨平台项目的三方生态与 IDE 集成不如 CMake。
- **手写 Makefile**：不可维护，不跨平台。

## 后果

- 优点：跨平台、跨编译器、IDE 集成好、preset 机制让 CI 与本地一致。
- 缺点：CMake 语法表达能力有限；需要保持 `cmake/` 模块整洁避免臃肿。

## 参考

- https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html
