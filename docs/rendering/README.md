# Rendering 模块

> 状态：**Implemented**（Phase 6，软件后端子集）

## 已实现

- Layout Tree → Paint → Display List → 软件光栅化 → PPM 输出
- background / border / text 绘制、alpha 混合、裁剪
- 文本：内嵌公有领域 8x8 位图字体（ASCII；见 ADR 0005）

## 未实现

- FreeType/HarfBuzz 文本整形与 Unicode 回退
- 图像解码、渐变、变换、滤镜、分层合成、GPU 后端

## 架构

```text
Layout Tree → Paint → Display List → Rasterization → Compositor(计划) → Window(计划)
```
