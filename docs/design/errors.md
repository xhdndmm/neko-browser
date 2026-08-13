# Error / Result 模型设计

> 位置：`neko::base`（`src/base/include/neko/base/status.h`）

## 为什么存在

浏览器引擎大量处理预期失败：URL 解析、HTTP 传输、HTML/CSS 解析、IO、安全检查。
需要一种**显式、可测试、无异常开销**的错误传播方式，并区分错误类别，便于上层
决策（重试、降级、展示、终止）。

## 核心类型

### Error

```cpp
class Error {
  ErrorCategory category_;   // 分类：InvalidArgument / Io / Network / Parse / ...
  std::string message_;      // 人类可读描述
};
```

- 用命名工厂构造：`Error::Parse("...")` 比裸构造可读。
- `operator bool()`：非 `kNone` 时为 true（表示存在错误）。

### Result\<T\>

`std::variant<T, Error>` 的值或错误联合：

```cpp
Result<int> ParsePort(std::string_view s);
```

- `[[nodiscard]]`：强制调用方处理结果。
- `.value()` 在错误态抛出 `BadResultAccess`（编程错误时使用；预期失败应检查
  `has_value()` 后走显式分支）。
- `.error()` 访问错误（仅错误态）。
- `.value_or(fallback)` 便捷降级。
- 隐式构造：`return 42;` / `return Error::Io("...");` 自然表达。

### Status（= Result\<void\>）

无值操作的专用形态，`Ok()` / `Err(...)`。

### 自由函数

- `Ok(value)` / `Ok()` → 成功结果
- `Err(error)` → `ErrorResult` 载体，可隐式转换为任意 `Result<T>`

### NEKO_TRY

GNU 语句表达式实现（GCC/Clang）：

```cpp
Result<int> Outer() {
  const int v = NEKO_TRY(ParsePort("8080"));
  return v * 2;
}
```

- 失败时提前返回 `error()`
- MSVC 上不可用（编译期显式报错），改用两步模式

## 使用规则

| 场景 | 用 |
| --- | --- |
| 预期失败（解析、IO、网络、权限） | `Result<T>` / `Status` |
| 编程错误 / 不变量破坏 | `NEKO_ASSERT` |
| 致命且无法恢复 | `NEKO_LOG_FATAL` + abort |

- 禁止静默吞错：`(void)result;` 或忽略返回值
- 禁止用布尔返回值承载错误分类信息

## 线程与所有权

- `Error` / `Result<T>` 是值类型，可自由跨线程传递
- 无隐藏共享状态

## 测试

见 `tests/unit/base/status_test.cpp`（Ok/Err、隐式转换、void、value_or、
BadResultAccess、NEKO_TRY、move-only 值）。

## 演进

若将来切换到 C++23 `std::expected`，只需改 `Result` 实现为别名，公共 API 语义
不变（见 ADR 0002 / 0004）。
