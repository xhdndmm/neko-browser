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
- `Rasterizer`：RGBA8888 缓冲、alpha 混合、边界裁剪；文本用内嵌 8x8 位图字体
  （公有领域，来源见 `font8x8.h` 头注释），字形按 font_size/8 缩放，字符步进 =
  font_size。
- `Painter`：按 背景 → 边框 → 行内文本 → 块级子盒 的顺序生成命令。

## 文本渲染决策（ADR 0005）

Phase 6 使用内嵌 8x8 位图字体渲染 ASCII，避免在核心管线落地前引入 FreeType/
HarfBuzz 依赖与系统字体查找。Unicode、整形、字体回退是后续里程碑。

## 未实现

- 图像、渐变、变换、滤镜、分层合成、GPU 后端。
