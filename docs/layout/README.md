# Layout 模块

> 状态：**Implemented**（Phase 5，子集）

## 已实现

- 独立 Layout Tree（与 DOM 分离），绝对视口坐标
- 盒模型（margin/border/padding，百分比按包含块宽度解析）
- block layout（垂直堆叠、宽度填充/显式/百分比、内容高度/显式高度）
- inline layout（词级换行 → 行盒 → 文本游程，inline 元素样式作用于文本）
- inline-block（`display:inline-block`）：行内级原子盒，内部为块格式化上下文
  （块子元素垂直堆叠），宽度显式或 shrink-to-fit（CSS2.1 10.3.9），background/
  border/padding、vertical-align/行高参与行盒，嵌套于任意行内元素中亦可
- 文本宽度：注入 `neko::graphics` 字体时按 FreeType 真实 advance 测量（词宽、
  空格宽、表格 max-content 测量、命中测试），无字体时回退"每字符 = font_size"
  等宽模型
- display:none 跳过、position:relative 偏移、position:absolute 定位（从流移除、
  相对最近 positioning 祖先 padding box、top/left/right/bottom、shrink-to-fit 与
  left+right 约束方程）
- float（`float:left/right`）：行外锚定包含块一侧，后续行盒按浮动盒占用的垂直
  区间收缩可用宽度以环绕；宽 shrink-to-fit 或显式、显式高支持
- table layout（table/tr/td/th 网格、colspan/rowspan、显式列宽 px/%、auto 列按
  max-content 比例分配剩余宽度、行高按内容）
- span 解析按 WHATWG tables.html：非负整数解析（尾随文本忽略）、colspan>1000 截断
  到 1000、rowspan>65534 截断到 65534、rowspan=0 表示跨到**所在行组**末尾（保留
  thead/tbody/tfoot 与连续匿名 `<tr>` 的隐式行组边界）

## 未实现

- flexbox/grid、fixed/sticky、margin 折叠、z-index、百分比 offset
  （fixed 暂按 absolute 处理）
- float：`clear`、多个浮动盒相交的 BFC 排布
- 表格：border-collapse/border-spacing、`vertical-align`、`<caption>` 定位、
  显式 `height`/`rowspan` 的完全行高分配（overflow 只加到最后一个跨行行）、
  auto 表格宽度按 shrink-to-fit（当前按 100% 填满包含块）

## 架构

```text
DOM → Style → Layout Tree → Layout → Paint Tree
```
