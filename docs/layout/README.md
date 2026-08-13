# Layout 模块

> 状态：**Not Started**（计划 Phase 5）

## 职责

```text
DOM → Style → Layout Tree → Layout → Paint Tree
```

- DOM 与 Layout Tree 概念分离
- 盒模型（content/padding/border/margin）
- block / inline / text layout
- 后续：flexbox、grid、定位、transform

## 约束

- Flexbox/Grid 必须真实实现，禁止伪装成 block layout
