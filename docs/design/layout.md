# Layout 设计

> 位置：`neko::layout`（`src/layout/`）

## 为什么存在

DOM 与布局树分离：布局树持有几何信息（盒模型、行盒、文本游程），DOM 不存几何。
这样布局可以独立于 DOM 变更而失效/重建。

## 模型

- `LayoutBox`：border box 位置 (x, y) 与尺寸 (w, h)，**绝对坐标**（视口空间）；
  margin/border/padding 分别存储；`content_*()` 访问器。
- 块级子盒垂直堆叠；内联内容（文本节点 + inline 元素）按词换行生成 `Line`
  与 `TextRun`（含字号与颜色）。
- 宽度：显式 px/% 或填满包含块内容宽；高度：内容驱动或显式值。
- `position: relative` 用 left/top 偏移；absolute/fixed 按 static 处理（未实现）。
- 表格（`display: table`）走独立算法：先收集行/单元格（展平 thead/tbody/tfoot、
  支持匿名直接 `<tr>`），用 colspan/rowspan 建占用网格；列宽由显式单元格宽固定、
  auto 列按 max-content 测量分配剩余宽度；行高由单元格内容高决定；单元格先按
  (0,0) 局部布局，再 `TranslateBox` 平移到网格槽位。
- span 解析对齐 WHATWG tables.html 的表格模型：colspan/rowspan 按"非负整数"解析
  （跳过前导空白、尾随文本忽略、非数字开头视为无效）；colspan 截断上限 1000、
  rowspan 截断上限 65534；`rowspan="0"` 表示跨到**所在行组**末尾——收集行时保留
  行组边界（显式 thead/tbody/tfoot 行组 + 连续匿名 `<tr>` 的隐式行组），`rowspan=0`
  按 `group.end - cell.row` 解析，不再跨过行组边界。

## 坐标约定

全部坐标是**视口绝对坐标**（根为 (0,0)），便于 paint 直接使用；这也是测试断言
的基础。

## 未实现

- flexbox / grid、absolute/fixed/sticky、浮动、垂直 margin 折叠
  （margin 折叠未实现：子盒 margin 不穿透父盒）。
- 表格：border-collapse/border-spacing、`vertical-align`、`<caption>` 定位、
  auto 表格宽度的 shrink-to-fit（当前填满包含块）、跨行单元格的完整行高分配。
