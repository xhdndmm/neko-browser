# Sanitizer 使用

## Preset

| Preset | 组合 | 说明 |
| --- | --- | --- |
| `asan` | `address;undefined` | 内存错误 + 未定义行为 |
| `ubsan` | `undefined` | 仅 UB |
| `tsan` | `thread` | 数据竞争 |

```bash
cmake --workflow --preset asan
cmake --workflow --preset ubsan
cmake --workflow --preset tsan
```

## 运行选项

```bash
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 ctest --preset asan
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --preset asan
```

## 平台说明

- 当前 sanitizer preset 面向 **GCC/Clang（Linux/macOS）**。
- MSVC 的 `/fsanitize=address` 在需要时补充（届时更新
  `cmake/Sanitizers.cmake` 与本文档）。

## 已知环境注意事项

1. **TSan 与高熵 ASLR**：Ubuntu 24.04 等新内核默认 `vm.mmap_rnd_bits=32`，
   GCC libtsan 会报 `FATAL: ThreadSanitizer: unexpected memory mapping`。
   这是工具链/内核兼容问题，**不是代码缺陷**。规避方式：
   - `sudo sysctl vm.mmap_rnd_bits=28`（系统级）
   - 或单次运行：`setarch $(uname -m) -R ctest --preset tsan`
   已用后一种方式验证：本项目 TSan 下 495/495 测试通过，无 neko 代码数据竞争。
2. **GCC 系统头误报**：-O2 + TSan 时 GCC 可能在 libstdc++ 头（如 `<streambuf>`）
   报 `-Wnull-dereference` 误报。sanitizer preset 默认不开启
   `NEKO_WARNINGS_AS_ERRORS`，请勿为 sanitizer 构建强开 Werror 后去"修"系统头。
3. **Qt 内部竞争抑制（`tools/tsan.supp`）**：GUI 冒烟测试在真实 Qt 事件循环
   下运行，Qt 自身内部（QArrayData 隐式共享、QThreadPool、QWidget 事件分发/
   析构）会被 TSan 报告数据竞争。经核实：这些报告的读写两侧栈帧**全部**在
   `libQt6*` 内，与 neko 代码无关。`tools/tsan.supp` 仅按模块名抑制
   `libQt6Core/Gui/Widgets` 的竞争，**不含任何 neko 源文件**——项目代码的
   竞争仍会照常报告并必须修复（如 2026-08 修复的 `Tab::origin` 未持锁写入
   竞争，见 ADR 0011 关联提交）。该抑制已通过 `tsan` 测试预设的
   `TSAN_OPTIONS` 环境变量接入，无需手动指定。

## 铁律

- 内存错误、数据竞争、UB 是**真实缺陷**，必须修复。
- 禁止关闭 sanitizer、禁用检查、删除测试来掩盖失败。
- 新模块落地时，CI 的 asan 任务必须保持通过。

## 实现说明

- `NEKO_SANITIZERS` 缓存变量是 CMake 列表（分号分隔），在
  `cmake/Sanitizers.cmake` 中转换为编译器所需逗号分隔的
  `-fsanitize=address,undefined`。
- 覆盖率使用 `--coverage`（gcov），见 TESTING.md。
