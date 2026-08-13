# ADR 0003：GoogleTest 经 FetchContent 固定版本

- 状态：**Accepted**（2026-08）
- 决策者：架构组

## 背景

Phase 0 需要单元测试框架。测试框架属于"基础设施"类依赖，允许使用成熟三方库。

## 决策

- 使用 **GoogleTest v1.15.2**（固定 tag + SHA256 校验）。
- 通过 **FetchContent** 在配置期拉取，URL 固定：
  `https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz`
- 不随仓库 vendoring 三方源码。
- GTest 头文件按 **system include** 处理，使项目严格警告只作用于项目代码。

## 备选方案

- **Catch2 / doctest**：也成熟；但 GoogleTest 的 `gtest_discover_tests` 与
  ctest 集成最顺，团队熟悉度最高。
- **系统包（libgtest-dev）**：版本不受控、不同 CI 镜像不一致，不可复现。
- **自研测试框架**：纯属重复造轮子，无收益。

## 后果

- 优点：构建可复现（固定版本 + 校验和）、CI 与本地一致、与 ctest 集成好。
- 缺点：配置期需要网络拉取；离线环境需预下载（见 BUILDING.md）。

## 参考

- https://google.github.io/googletest/
