# CSS 模块

> 状态：**Implemented**（Phase 4，子集）

## 已实现

- tokenizer（ident/at-keyword/hash/数字/尺寸/百分比/字符串/标点/注释）
- parser：样式表（限定规则 + @media 嵌套）、声明块、!important
- 选择器：type/id/class/属性（全部操作符）/伪类(:first-child/:last-child/
  :nth-child/:root) + 组合器（后代/子/相邻/后续兄弟），特异性计算
- 颜色（#hex、rgb()/rgba()、~150 命名色）与类型化值解析
- **CSS 自定义属性**（CSS Custom Properties Level 1）：`:root` 上定义
  `--name`，任意属性用 `var(--name[, fallback])` 引用；自定义属性默认继承；
  var() 无法解析且无 fallback 时该声明在 computed-value 时无效（按 unset
  处理）。限制：不支持 var() 内嵌/fallback 内 var()、同一元素上的自定义
  属性链式引用。
- **逻辑属性**（CSS Logical Properties 1）：inline-size/block-size、
  min/max-inline-size、min/max-block-size、margin/padding-inline(-start/end)、
  margin/padding-block(-start/end)、border-block-start/end、
  border-inline-start/end；margin/padding-inline/block 支持 1–2 值展开；
  place-items 折叠为 align-items。
- flex 属性解析：display:flex/inline-flex、flex-direction/flex-wrap/
  justify-content/align-items/align-content、flex-grow/flex-shrink/flex-basis、
  flex 简写、gap/row-gap/column-gap
- grid 属性解析：display:grid、grid-template-columns/rows
  （px/%/fr/auto/min-content/max-content + repeat()）、grid-column/row
  （含 start/end longhand）、gap（与 flex 共用）

## 未实现

- transform/动画/过渡、媒体查询完整求值（prefers-color-scheme 等按匹配
  处理，仅 print/speech 不匹配）
- 数学函数：calc()/min()/max()/clamp()、vw/vh 单位
- aspect-ratio、border-radius、box-shadow、place-items 的 inline（justify）
  分量、逻辑边框颜色/样式分量
- grid：minmax()、repeat(auto-fit/auto-fill)、命名线/区域
- 完整伪类集（:hover 等按不匹配处理）

## 架构

```text
CSS Source → Tokenizer → Parser → Stylesheet → Selector Matching → Cascade → Computed Style
```
