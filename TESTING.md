# TESTING

neko-browser 将测试视为实现的一部分。任何重要行为都必须有验证策略。

## 运行测试

```bash
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug
```

只运行某个测试二进制：

```bash
./build/debug/bin/neko_base_tests            # base 模块测试
./build/debug/bin/neko_base_tests --gtest_filter='StatusTest.*'
./build/debug/bin/neko_browser_tests         # CLI 选项测试
```

## 测试分层

| 层级 | 目录 | 说明 |
| --- | --- | --- |
| 单元测试 | `tests/unit/` | 快速、无外部 I/O（除临时文件），覆盖各模块 |
| 集成测试 | `tests/integration/` | Phase 2+，引擎级流程 |
| 网络测试 | `tests/network/` | Phase 2+，本地 HTTP 服务器 |
| HTML/CSS/布局/渲染 | `tests/{html,css,layout,rendering}/` | Phase 3–6+ |
| 模糊测试 | `tests/fuzz/` | Phase 1+，parser 安全 |
| Web Platform Tests | `tests/web-platform/` | Phase 13+ |

## 添加新测试

1. 在 `tests/unit/<module>/` 下新建 `xxx_test.cpp`。
2. 在 `tests/unit/CMakeLists.txt` 中把它加入对应测试二进制，或新建二进制。
3. 写有意义的断言 —— 覆盖率不是唯一指标，禁止为了覆盖率写无意义测试。

### 时间相关测试

不要用 `sleep_for` 去赌一个时间窗口：CI 机器负载不同，sleep 可能比预期多睡几十到上百毫秒，
让帧调度/动画断言随机失败（见 AGENTS.md §44，且不得靠放宽断言掩盖）。

- 需要推进动画、视频帧的测试用 `renderer::Page::SetAnimationClockForTesting()` 注入一个可手动
  推进的单调时钟（毫秒），不要真实等待；这样测试同时更快、跨平台稳定。
- 只能依赖真实时间的场景（Qt 冒烟测试、子进程事件循环）改用带超时的轮询
  （`WaitFor` / 有界重试），等状态到达再断言，而不是 `sleep` 之后只断言一次。
- 字体度量依赖系统字体栈，跨平台断言要留出合理容差，并优先比较引擎内部一致的两条路径
  （例如 SVG 文本整形与 layout 文本测量走同一个 advance 来源）。

## Sanitizer 测试

```bash
cmake --workflow --preset asan     # ASan + UBSan
cmake --workflow --preset tsan     # TSan
```

发现内存错误 / 数据竞争 / UB 时必须修复，而不是关闭 sanitizer。
参见 [sanitizers.md](docs/development/sanitizers.md)。

## 覆盖率

```bash
cmake --workflow --preset coverage
cd build/coverage
lcov --capture --directory . --output-file coverage.info --ignore-errors mismatch
lcov --remove coverage.info -o filtered.info '/usr/*' '*/_deps/*' '*/tests/*' '*/build/*'
lcov --list filtered.info
```

覆盖率报告在 CI 中以 artifact 形式上传。

## 测试失败处理

当测试失败，先确定失败类别，再修复真正原因：

```text
测试代码 Bug
实现 Bug
基础设施 Bug
平台差异
规格理解错误
```

禁止通过删除测试、修改期望来掩盖问题（见 AGENTS.md §44、§61）。
