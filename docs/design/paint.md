# Paint / Rendering 设计

> 位置：`neko::paint`（`src/paint/`）

## 为什么存在

布局树 → 可保留的绘制命令（显示列表）→ 软件光栅化 → 图像/表面。显示列表让
绘制与光栅化解耦，为将来增量绘制/合成器留出空间。

## 管线

```text
Layout Tree → Painter → DisplayList → Rasterizer → RGBA 缓冲 → PPM
```

- `DisplayList`：FillRect / BorderRect / DrawText 命令（DrawText 携带 font-family
  供字形选择）。
- `Rasterizer`：RGBA8888 缓冲、alpha 混合、边界裁剪；文本默认用 FreeType 灰度
  字形（`neko::graphics` 的 `FontRegistry`，按 `font-family` 选字体栈、逐字符
  回退、抗锯齿、任意字号、UTF-8 解码、glyph 缓存），无字体可用时回退到内嵌
  8x8 位图字体（公有领域，见 `font8x8.h` 头注释）。
- `Painter`：按 背景 → 边框 → 行内文本 → 块级子盒 的顺序生成命令。

## 文本渲染决策（ADR 0005 → ADR 0009）

Phase 6 先用内嵌 8x8 位图字体渲染 ASCII（ADR 0005）；随后迁移到 FreeType
（ADR 0009）：真实字形与抗锯齿，布局按真实 advance 测量，`font-family` 解析
匹配（具体名 + 通用族），字体栈自动附加 CJK 回退 → 中文可显示；粗体/斜体按
`font-weight`/`font-style` 匹配相邻字体文件。剩余：HarfBuzz 整形、完整字体
目录扫描。

## 未实现

- 图像、渐变、变换、滤镜、分层合成、GPU 后端。
- 文本：HarfBuzz 整形、`text-align` 对齐。
