# WebIDL 生成绑定

> 状态：**第一阶段（属性反射 + 方法注册）**，2026-10。
> 相关：[ADR 0023](../architecture/adr/0023-webidl-generated-bindings.md)。

## 为什么

`src/javascript/src/binding/` 是手写的 QuickJS 胶水。每个 Web 平台成员都要写：
解包 `JSValue` → 类型检查 → 类型转换 → 调引擎 → 包回 `JSValue` → 标脏。
一个布尔反射属性约 25 行，全仓 500+ 个访问器、200+ 个方法，合计 2.1 万行。
手写的核心问题不是"累"，而是**同一套 WebIDL 语义被复制了几百遍**，每份都可能
在强制转换、异常、脏标记上略有不同，且无法机器校验。

## 管线

```
src/javascript/webidl/*.webidl
        │  tools/webidl/gen.py（词法 + 语法 + 校验 + 代码生成）
        │  生成后调用 clang-format（保证格式门禁通过）
        ▼
src/javascript/src/binding/generated/*_generated.{h,cpp}
        │  被手写的 Define<Interface>Prototype() 调用
        ▼
QuickJS 原型对象
```

生成文件**入库**（与 `tools/gen_encoding_tables.py` 生成的编码表同策略），
构建时不需要 Python。`ctest -R webidl` 会跑 `gen.py --check`，保证入库文件
与 `.webidl` 一致。

```bash
python3 tools/webidl/gen.py           # 重新生成
python3 tools/webidl/gen.py --check   # 校验（CI）
```

## 支持的子集

见 `tools/webidl/gen.py` 顶部的注释。当前：

| 语法 | 含义 |
| --- | --- |
| `[Reflect=attr] attribute boolean x` | 生成 getter/setter：`HasAttribute` / `JS_ToBool` + 置空属性或移除 + `MarkAttrDirty` |
| `[Reflect=attr] attribute DOMString x` | 生成 getter/setter：`GetAttribute` / `ArgString` + 空串移除 + `MarkAttrDirty` |
| `[CFunction=Symbol] void m(args)` | 只生成注册条目（`JS_CFUNC_DEF`），函数体手写 |
| `[Getter=S] [Setter=S] attribute T x` | 注册已有手写访问器，不生成函数体 |
| `[Getter=S] readonly attribute T x` | 同上，仅 getter |

生成器对**不支持的类型/组合直接报错**，不会产出半正确代码。尚未支持：
`long`/`unsigned`/枚举/可空类型/重载/`[PutForwards]`/`[CEReactions]` 时序语义、
多接口继承的自动挂载。

## 新增一个生成成员

1. 在 `src/javascript/webidl/<interface>.webidl` 里加一行；
2. `python3 tools/webidl/gen.py`；
3. 编译 + 测试。

例如把 `HTMLElement.title` 交给生成器：

```webidl
[Reflect=title] attribute DOMString title;
```

生成结果（节选，`generated/html_element_generated.cpp`）：

```cpp
JSValue GeneratedHTMLElementGetTitle(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::optional<std::string_view> value = element->GetAttribute("title");
  return value.has_value() ? JS_NewStringLen(ctx, value->data(), value->size())
                           : JS_NewStringLen(ctx, "", 0);
}
```

## 边界：什么不该进生成器

生成器只负责**能从 IDL 完整推导**的成员。引擎特有语义留在手写代码里，直到
生成器能表达它为止：

- `dir`：只接受 `ltr`/`rtl`/`auto`，非法值读回 `""`；
- `tabIndex`：缺省值取决于元素种类（链接/控件 0，其余 -1）；
- `className`：`SVGAnimatedString` 在 SVG 上不是字符串；
- `value`/`checked`：走 `TextControlState`，不是内容属性反射。

生成器与手写代码**共存**，不追求一次性迁移。判断标准：写进 `.webidl` 的那一行
必须能唯一、完整地描述行为。

## 线程

生成代码与手写绑定同线程归属：在 `DomBinder` 的线程上调用，不加锁。
