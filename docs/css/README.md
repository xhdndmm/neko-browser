# CSS 模块

> 状态：**Implemented**（Phase 4，子集）

## 已实现

- tokenizer（ident/at-keyword/hash/数字/尺寸/百分比/字符串/标点/注释）
- parser：样式表（限定规则 + @media 嵌套）、声明块、!important
- 选择器：type/id/class/属性（全部操作符）/伪类(:first-child 等) + 组合器
  （后代/子/相邻/后续兄弟），特异性计算
- 颜色（#hex、rgb()/rgba()、~150 命名色）与类型化值解析
- flex 属性解析：display:flex/inline-flex、flex-direction/flex-wrap/
  justify-content/align-items/align-content、flex-grow/flex-shrink/flex-basis、
  flex 简写、gap/row-gap/column-gap
- grid 属性解析：display:grid、grid-template-columns/rows
  （px/%/fr/auto/min-content/max-content + repeat()）、grid-column/row
  （含 start/end longhand）、gap（与 flex 共用）

## 未实现

- transform/动画/过渡、媒体查询完整求值
- grid：minmax()、repeat(auto-fit/auto-fill)、命名线/区域
- 完整伪类集（:hover 等按不匹配处理）

## 架构

```text
CSS Source → Tokenizer → Parser → Stylesheet → Selector Matching → Cascade → Computed Style
```
