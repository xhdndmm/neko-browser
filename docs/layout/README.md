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
- **flexbox（M1–M6）**：display:flex/inline-flex；flex-direction
  （row/column+reverse）、flex-wrap（含 wrap-reverse）、flex-grow/shrink/basis
  （含 flex 简写）、justify-content（6 值）、align-items
  （stretch/flex-start/flex-end/center/baseline）、align-content（确定 cross
  尺寸时）、row/column gap、**order**（稳定排序）、**align-self**（逐项覆盖
  align-items，含阻止 stretch）、**min/max-width/height**（主轴/交叉轴夹取，
  min 优先于 max）、**auto 外边距**（主轴吸收自由空间并覆盖 justify-content；
  交叉轴吸收行内自由空间并覆盖 align-self）；嵌套 flex 可用；内联 flex 为
  行内原子盒
- **grid（M1）**：display:grid；grid-template-columns/rows
  （px/%/fr/auto/min-content/max-content + repeat()）、row-major 自动放置
  （grid-auto-flow: row）、grid-column/row 行与 span 放置（含简写与 longhand）、
  column/row gap；超出显式模板的隐式轨道按 auto 尺寸（auto 列按该列起始项目的
  max-content、auto 行按该行项目内容高；fr 列/行分享容器剩余空间）

## 未实现

- flexbox：百分比高度精确解析、flex 容器自身 min/max、max-width 截断后的
  剩余自由空间再分配、`flex-basis: content`
- grid：inline-grid、命名区域、`dense` 打包、`minmax()`、`grid-auto-flow`
  非 row、fr 行在容器高度不确定时的精确解析、网格项目内的绝对定位精确包含块
- fixed/sticky、margin 折叠、z-index、百分比 offset
  （fixed 暂按 absolute 处理）
- float：`clear`、多个浮动盒相交的 BFC 排布
- 表格：border-collapse/border-spacing、`vertical-align`、`<caption>` 定位、
  显式 `height`/`rowspan` 的完全行高分配（overflow 只加到最后一个跨行行）、
  auto 表格宽度按 shrink-to-fit（当前按 100% 填满包含块）

## 架构

```text
DOM → Style → Layout Tree → Layout → Paint Tree
```
