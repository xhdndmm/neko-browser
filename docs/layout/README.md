# Layout 模块

> 状态：**Implemented**（Phase 5，子集）

## 已实现

- 独立 Layout Tree（与 DOM 分离），绝对视口坐标
- 盒模型（margin/border/padding，百分比按包含块宽度解析）
- block layout（垂直堆叠、宽度填充/显式/百分比、内容高度/显式高度）
- inline layout（词级换行 → 行盒 → 文本游程，inline 元素样式作用于文本）
- display:none 跳过、position:relative 偏移

## 未实现

- flexbox/grid、absolute/fixed/sticky、浮动、表格布局算法、margin 折叠

## 架构

```text
DOM → Style → Layout Tree → Layout → Paint Tree
```
