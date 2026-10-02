# Layout 模块

> 状态：**Implemented**（Phase 5，子集）

## 已实现

- 独立 Layout Tree（与 DOM 分离），绝对视口坐标
- 盒模型（margin/border/padding，百分比按包含块宽度解析）
- block layout（垂直堆叠、宽度填充/显式/百分比、内容高度/显式高度）
- **子元素 DOM 顺序 / 匿名块**（2026-10）：连续行内子节点构成匿名块并与
  块级兄弟**交错**排布（CSS 2.1 §9.2.1.1）；修复了“全部行内内容先行、
  全部块级子元素随后”把块间文本/media 提升到容器顶部的问题（`<p>x</p>
  text<p>y</p>`、块间 `<video>` 等）；块间行盒正确抑制相邻兄弟外边距折叠
- **替换元素绘制（块级/浮动/Flex 子项）**（2026-10）：`img`/`video`/`canvas`/`svg`
  在 `BuildBlock`（含表格单元格 / grid 项）、`BuildFloat`、`BuildFlexItem`
  中按 CSS 尺寸 > width/height 属性 > 固有尺寸（单轴给定时保持比例）定尺寸，
  并把解码像素挂到盒上供绘制（此前只留尺寸盒，封面画成灰块）
- **inline `<svg>` 按替换元素布局**（2026-10）：`<svg width=18 height=18>` 图标
  （bilibili 顶栏、Codeberg Octicons）此前按空 inline 盒量到 0×0——行内收集、
  flex 项、绝对定位、固有宽度测量（`MeasureContent`）全部走替换元素路径后，
  图标占位正确、相邻文本不再收缩换行（回归 4 例）；**SVG 子树（path 等）不参与
  布局也不栅格化**（图标为空白占位；SVG 作为独立图片解码是另一条路径）
- **固有宽度测量两处修正**（2026-10）：① 替换元素在**没有任何 CSS width** 时
  可用 width 属性（presentational hint），有作者 width（即使用百分比）时忽略；
  ② 绝对/固定定位子树不参与 min/max-content（CSS Sizing 1 §5）——缺失时
  绝对定位的轮播图片会把 `1fr` 轨道最小值撑大到图片宽
- **Flex 项自动最小值钳制**（Flexbox §4.5，2026-10）：行方向 flex 项
  `min-width:auto` 的内容最小尺寸取 min(content, specified)——`width:100%`
  对确定容器可解析时用它封顶，`min-width:auto` 的项不再以子树 min-content
  为底拒绝收缩（bilibili 轮播 100% 链，回归见 `BilibiliCardRepro`）
- **auto 高度列的 main 尺寸与 reverse 翻转**（2026-10）：容器高 auto 时
  没有可分配的 free space，`row-reverse`/`column-reverse` 的翻转基准改为
  **行自身 main 范围**（原用“容器主轴回退值 = 可用宽度”，column-reverse 的
  子项整体被翻到容器下方数百像素——Codeberg 头部的标题/logo 全空白）；
  列项在容器高 auto 时：确定 height 属性定高，否则可增长项回退实测内容高
  （`flex:2 1 0` 的 basis 0 曾把项压成 0 高，内容溢出不占位；回归 3 例）
- **晚到附着不重复布局**（2026-10）：尺寸已确定的替换元素（CSS 宽高或
  width/height 属性双轴齐备）收到解码帧时只就地更新布局盒/行内盒的图像指针，
  不重建布局树（真实页面逐张懒加载封面时，每张一次全量重排把滚动拖死）；
  固有尺寸盒（缺一个轴）仍重建布局
- inline layout（词级换行 → 行盒 → 文本游程，inline 元素样式作用于文本）
- **text-align**（CSS Text 3 §5）：`left`（默认）/`center`/`right` 在行内把内容
  移到可用宽度内的对应位置；`justify` 已解析但未实现（词间不分散）；UA 样式
  `caption { text-align: center }` 使表格标题居中
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
- **表格宽度**：`width:auto` 按 **shrink-to-fit**（CSS2.1 17.5.2：
  max(min-content, min(max-content, available))），表格不撑满包含块、列间不产生
  过大空隙；显式 `width` 仍按指定值
- **`<caption>` 布局**：`display:table-caption` 的 caption 作为表格首个子框渲染在
  行上方、宽度与表格一致；其自然宽度计入表格 max-content（caption 比列宽时表格
  相应加宽）
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
  行内原子盒。**2026-10（bilibili 驱动）**：替换项无 CSS 主轴尺寸时按替换
  尺寸作基、交叉尺寸按已解析宽度 × 固有比例推导；替换项自动最小尺寸按 0
  （允许图片缩进容器）；确定交叉尺寸容器的 stretch 精确拉伸（不再取 max）；
  flex 项百分比高度在容器内容高确定时精确解析（与 absolute 填充一起构成
  真实站点的 100% 高度链）；遗留 `display:-webkit-box` 映射为块级 flex 容器
  （-webkit-box-orient/-flex/-pack/-align/-ordinal-group → flex 属性）
- **grid（M1）**：display:grid；grid-template-columns/rows
  （px/%/fr/auto/min-content/max-content + repeat()）、row-major 自动放置
  （grid-auto-flow: row）、grid-column/row 行与 span 放置（含简写与 longhand）、
  column/row gap；超出显式模板的隐式轨道按 auto 尺寸（auto 列按该列起始项目的
  max-content、auto 行按该行项目内容高；fr 列/行分享容器剩余空间）
- **列表（list-item）**：`li { display: list-item }`；marker（圆点/序号）作为
  首行文本 run 绘制在内容左侧的 gutter 内，`ul`→disc、嵌套 `ul`→circle→square、
  `ol`→decimal、支持 lower/upper-alpha/roman；marker 按 li 在父列表中的位置编号
- **描述列表（dl/dt/dd）**：`<dl>` 有块级上下边距，`<dt>` 术语与 `<dd>` 值按
  源码顺序垂直排布，`<dd>` 值缩进（UA `margin-inline-start: 40px`）；支持一个
  dt 对应多个 dd、多个 dt 共享值，以及 div 包装的组
- block 容器内 inline 内容先布局、block 级子元素（含嵌套列表/表格）随后在文本
  行之下垂直排布

## 未实现

- flexbox：flex 容器自身 min/max、max-width 截断后的剩余自由空间再分配、
  `flex-basis: content`、`-webkit-line-clamp` 独立生效（当前需配合确定高度，
  由 overflow 裁剪实现两行标题）
- grid：inline-grid、命名区域、`dense` 打包、`minmax()`、`grid-auto-flow`
  非 row、fr 行在容器高度不确定时的精确解析、网格项目内的绝对定位精确包含块
- fixed/sticky、margin 折叠、z-index、百分比 offset
  （fixed 暂按 absolute 处理）
- float：`clear`、多个浮动盒相交的 BFC 排布
- 表格：border-collapse/border-spacing、`vertical-align`、caption 的
  `caption-side: bottom`/定位样式、显式 `height`/`rowspan` 的完全行高分配
  （overflow 只加到最后一个跨行行）
- 列表：`list-style-position: inside`、`::marker` 伪元素、`list-style` 简写、
  alpha/roman 的完整字母/罗马编号（当前重复取模）

## 架构

```text
DOM → Style → Layout Tree → Layout → Paint Tree
```
