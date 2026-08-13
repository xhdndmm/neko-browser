# CSS 模块

> 状态：**Not Started**（计划 Phase 4）

## 职责

```text
CSS Source → Tokenizer → Parser → Stylesheet → Selector Matching → Cascade → Computed Style
```

- 选择器、声明、属性、值、单位、颜色、长度、百分比
- 特异性、级联、继承、内联样式、UA 样式表
- 后续：flexbox/grid/transforms/animations/media queries

## 约束

- 禁止把 CSS 解析折叠成字符串拆分/单个巨型函数
- 参考：CSS 官方规范（各模块）
