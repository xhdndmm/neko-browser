# Paint / Rendering 设计

> 位置：`neko::paint`（`src/paint/`）

## 为什么存在

布局树 → 可保留的绘制命令（显示列表）→ 软件光栅化 → 图像/表面。显示列表让
绘制与光栅化解耦，为将来增量绘制/合成器留出空间。

## 管线

```text
Layout Tree → Painter → DisplayList → Rasterizer → RGBA 缓冲 → PPM
```

- `DisplayList`：FillRect / BorderRect / DrawText 命令。
- `Rasterizer`：RGBA8888 缓冲、alpha 混合、边界裁剪；文本默认用 FreeType 灰度
  字形（`neko::graphics`，抗锯齿、任意字号、UTF-8 解码、glyph 缓存），字符按
  真实 advance 步进；无字体可用时回退到内嵌 8x8 位图字体（公有领域，见
  `font8x8.h` 头注释）。
- `Painter`：按 背景 → 边框 → 行内文本 → 块级子盒 的顺序生成命令。

## 文本渲染决策（ADR 0005 → ADR 0009）

Phase 6 先用内嵌 8x8 位图字体渲染 ASCII（ADR 0005）；随后迁移到 FreeType
（ADR 0009）：ASCII 获得真实字形与抗锯齿，为中文/全 Unicode 铺路。布局层的
等宽度量（每字符 = font_size）仍是中间态，真实 advance 测量在后续里程碑
统一；`font-family` 匹配与中文字体回退链也在后续里程碑。

## 未实现

- 图像、渐变、变换、滤镜、分层合成、GPU 后端。
- 文本：布局真实 advance 测量、`font-family` 匹配、中文字体回退、HarfBuzz
  整形、粗体/斜体变体匹配。
