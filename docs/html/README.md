# HTML 模块

> 状态：**Not Started**（计划 Phase 3）

## 职责

```text
HTML Source → Tokenizer → Token Stream → Parser → DOM
```

- 真实 tokenizer/parser，禁止正则实现
- 字符引用、畸形 HTML、注释、容错
- 参考：WHATWG HTML Standard

## 约束

- 畸形 HTML 是正常输入，不是异常
- 深度嵌套 / 超大输入必须有资源限制
