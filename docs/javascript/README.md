# JavaScript 模块

> 状态：**Not Started**（计划 Phase 8）

## 职责

```text
JavaScript Engine → {Parser, AST, Bytecode, VM, GC} + Web IDL 绑定层
```

- 早期可接入第三方 JS runtime（QuickJS 等）作为过渡，但**仅作 JS runtime**
- 不得让第三方 JS 引擎替代 DOM/CSS/布局/渲染/导航/安全/存储
- 长期研究自有 lexer/parser/AST/interpreter/bytecode VM/GC/JIT

## 参考

- ECMAScript Specification
- Web IDL Specification
