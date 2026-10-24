# 0023 — WebIDL 生成绑定（QuickJS）

- 状态：**已接受，第一阶段已实现**
- 日期：2026-10-24
- 相关：[线程模型](../threading-model.md)、[JavaScript 文档](../../javascript/webidl-bindings.md)

## 背景

DOM 绑定层（`src/javascript/src/binding/`）是 JavaScript 对象与 C++ 引擎对象
之间唯一的桥。项目使用 QuickJS，它只提供 C API，不提供任何接口描述语言
（IDL）机制。因此每个 Web 平台成员都是手写的：

- 解包 `JSValue` → 取出 C++ 指针；
- 按 WebIDL 的类型强制转换规则转换参数；
- 调用引擎；
- 把结果包回 `JSValue`，失败时抛 JavaScript 异常；
- 标记 DOM/样式脏，让浏览器层重跑样式与布局。

到 2026-10，这一层已增长到 **21,274 行（23 个 .cpp）**，注册了 500+ 个访问器
和 200+ 个方法，是仓库第二大模块，且仍以每个 API 数十行的速度线性增长。

手写带来的具体问题：

1. **语义漂移**：同样的 `attribute DOMString` 反射在不同文件里对空串、`null`、
   `undefined` 的处理可能不同；WebIDL 的转换算法没有被集中实现。
2. **规范无法对账**：没有 `.webidl` 就无法把"规范要求的接口表面"与"实际注册的
   表面"做机器比对。
3. **算法被写进绑定**：例如 `Range` 的树操作写在 `range_binding.cpp`，因为那里
   方便，导致 DOM 核心贫血。
4. **批量重构不可行**：想统一给所有属性写入加脏标记策略，需要改数百处。

## 决定

引入**项目自有的 WebIDL 子集编译器**：`tools/webidl/gen.py`。它读取
`src/javascript/webidl/*.webidl`，生成 `*_generated.{h,cpp}`，由手写的
`Define<Interface>Prototype()` 调用。

原则：

- **只生成能从 IDL 完整推导的成员**。引擎特有语义（`dir` 的值域、`tabIndex`
  的默认值、`value` 的 `TextControlState`）继续手写，直到生成器能表达。
- **生成器宁可不支持，也不猜**。遇到未实现的类型/扩展属性直接报错。
- **生成文件入库**，与 `tools/gen_encoding_tables.py` 等既有生成器同策略；
  构建不依赖 Python，`ctest -R webidl` 校验入库文件与源一致。
- **生成代码走同一条格式门禁**：生成器内部调用 clang-format。
- **不引入重型依赖**：不引入 Python 包，只依赖标准库 + clang-format。

第一阶段范围：`[Reflect]` 的 `boolean`/`DOMString` 属性、`[CFunction]` 方法注册、
`[Getter]/[Setter]` 已有访问器注册。迁移 `HTMLElement` 的 `title`、`lang`、
`hidden`、`inert` 与 `focus()`。

## 备选方案

| 方案 | 说明 | 结论 |
| --- | --- | --- |
| 维持手写 | 零迁移成本 | 线性成本与语义漂移持续累积；到 3 万行后不可维护 |
| 表驱动注册（仅去掉注册样板） | 改动小 | 只解决 10% 的重复；函数体仍是手写 |
| 只生成属性反射（不走 IDL） | 1–2 天 | 收益大但无法扩展到方法/类型系统；与 A 不冲突，是 A 的子集 |
| **WebIDL 编译器（本 ADR）** | 数周完成全量子集 | 与浏览器同构；增量交付，第一阶段即可替换真实成员 |

## 后果

正面：

- 新增反射属性从"两个函数 + 注册"变成 `.webidl` 一行；
- 类型转换、异常、脏标记只有一份实现；
- 入库文件可被 CI 校验，`.webidl` 成为接口的单一事实来源；
- 为后续"接口表面与规范比对"提供数据基础。

负面 / 已知限制：

- 生成器本身是新的维护面（约 500 行 Python + 测试）；
- 子集之外仍需手写，短期内两套机制并存，需要纪律（见文档的边界一节）；
- 生成代码依赖 clang-format 才能通过格式门禁；
- `[CEReactions]` 的排队语义、重载、可空类型等尚未建模。

## 后续

1. 支持 `long`/`unsigned long`/枚举/可空类型与默认值；
2. 生成原型链与 `[Exposed]` 可见性，减少 `Define<Interface>Prototype` 的手写表；
3. 把 `Node`/`Document`/`Range` 的纯反射成员逐步迁移；
4. 增加"`.webidl` 与实际注册表 diff"的自检，作为规范覆盖率的度量。
