# JavaScript 模块

> 状态：**Partial**（Phase 8 里程碑 1：runtime 已接入；DOM 绑定未开始）

## 现状（里程碑 1）

- **运行时**：QuickJS（quickjs-ng v0.16.1，MIT），经 FetchContent 固定版本构建。
  封装在 `neko::javascript` 自有接口之后（`ScriptEngine` / `ScriptValue`），
  第三方头文件不泄漏到公共 API（见 ADR 0008）。
- **能力**：ES2025 核心语言、`ScriptEngine::Evaluate/CallGlobal/GetGlobal/
  SetGlobal`、值转换（ToString/ToNumber/ToBoolean/JsonStringify）、
  项目自有 `console` 绑定、执行时限中断、内存上限。
- **集成**：CLI `--eval`；GUI DevTools Console 持久 REPL。
- **测试**：27 个单元测试（算术/函数/对象/console/中断/内存限制/值生命周期/错误分类）。

## 长期架构目标

```text
JavaScript Engine → {Parser, AST, Bytecode, VM, GC} + Web IDL 绑定层
```

- QuickJS 仅作 JS runtime，**不得**替代 DOM/CSS/布局/渲染/导航/安全/存储。
- 长期可研究自有 lexer/parser/AST/interpreter/bytecode VM/GC/JIT
  （当前无计划，保持文档化）。

## 里程碑 2（未开始）

- Web IDL / DOM 绑定（window/document/navigator/location/history/console/
  timer/fetch/storage/events）
- 页面 `<script>` 标签执行
- 事件循环对接（microtask/Promise 与浏览器事件循环）
- 每页独立 ScriptEngine / 隔离域

## 参考

- ECMAScript Specification
- Web IDL Specification
- QuickJS: https://github.com/quickjs-ng/quickjs

