# Paint / Rendering 设计

> 位置：`neko::paint`（`src/paint/`）

## 为什么存在

布局树 → 可保留的绘制命令（显示列表）→ 软件光栅化 → 图像/表面。显示列表让
绘制与光栅化解耦，为将来增量绘制/合成器留出空间。

## 管线

```text
Layout Tree → Painter → DisplayList → Rasterizer → RGBA 缓冲 → PPM
```

- `DisplayList`：FillRect / BorderRect / DrawText / DrawImage 命令（DrawText 携带
  font-family/weight/italic 供字形选择；DrawImage 携带 object-fit）。
- `Rasterizer`：RGBA8888 缓冲、alpha 混合、边界裁剪；文本默认用 FreeType 灰度
  字形（`neko::graphics` 的 `FontRegistry`，按 `font-family` 选字体栈、逐字符
  回退、抗锯齿、任意字号、UTF-8 解码、glyph 缓存）；`DrawImage` 按 object-fit
  计算具体对象尺寸后最近邻缩放 blit（含滚动与裁剪），无字体可用时回退到内嵌
  8x8 位图字体（公有领域，见 `font8x8.h` 头注释）。
- `Painter`：按 背景 → 边框 → 行内原子盒（`Line.boxes`：`<img>` 走 DrawImage，
  `display:inline-block` 递归 `PaintBox(block_box)` 绘制其内部块内容）→ 行内文本
  → 块级子盒 的顺序生成命令。原生控件外观（`appearance`，见 CSS-UI-4 §7.2 +
  WHATWG rendering §15.5.4）：`<button>`（`appearance:auto`）或
  `appearance:button` 的盒默认用 buttonface 背景 + outset 边框（上/左亮、下/右
  暗，宽度沿用布局边框）绘制；作者的 background-color/border-color 声明优先于
  原生外观（§7.2.3 允许 UA 忽略，但引擎与浏览器一致保留作者样式，如导航
  dropdown 按钮 `background:#2a3c54`）；`appearance:none` 走普通背景/边框路径。

## 文本渲染决策（ADR 0005 → ADR 0009）

Phase 6 先用内嵌 8x8 位图字体渲染 ASCII（ADR 0005）；随后迁移到 FreeType
（ADR 0009）：真实字形与抗锯齿，布局按真实 advance 测量，`font-family` 解析
匹配（具体名 + 通用族），字体栈自动附加 CJK 回退 → 中文可显示；粗体/斜体按
`font-weight`/`font-style` 匹配相邻字体文件。剩余：HarfBuzz 整形、完整字体
目录扫描。

## 未实现

- 图像：双线性/高质量缩放（当前最近邻）、GIF/WebP/AVIF 解码、渐变、变换、
  滤镜、分层合成、GPU 后端。
- 文本：HarfBuzz 整形、`text-align` 对齐。
