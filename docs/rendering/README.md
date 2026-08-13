# Rendering 模块

> 状态：**Implemented**（Phase 6，软件后端子集）

## 已实现

- Layout Tree → Paint → Display List → 软件光栅化 → PPM 输出
- background / border / text 绘制、alpha 混合、裁剪
- 文本：FreeType 灰度字形（`neko::graphics`，抗锯齿、UTF-8、glyph 缓存；
  见 ADR 0009），无字体时回退内嵌 8x8 位图字体（ASCII；ADR 0005）
- 布局文本宽度按真实 advance 测量（词宽/空格/命中测试），与绘制一致
- `font-family` 解析与匹配（具体名 + sans-serif/serif/monospace 通用族）、
  逐字符字体回退，栈末尾自动附加 CJK 字体 → 中文可显示

## 未实现

- HarfBuzz 文本整形、粗体/斜体变体匹配
- `text-align` 对齐、连字符断行、CJK 逐字断行
- 完整系统字体目录扫描（当前内置候选路径表；具体名按文件名匹配）
- 图像解码、渐变、变换、滤镜、分层合成、GPU 后端

## 架构

```text
Layout Tree → Paint → Display List → Rasterization → Compositor(计划) → Window(计划)
                 ↓
             neko::graphics (FreeType 封装)
```
