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

## 坐标约定

全部坐标是**视口绝对坐标**（根为 (0,0)），便于 paint 直接使用；这也是测试断言
的基础。

## 未实现

- flexbox / grid、absolute/fixed/sticky、浮动、表格布局算法、垂直 margin 折叠
  （margin 折叠未实现：子盒 margin 不穿透父盒）。
