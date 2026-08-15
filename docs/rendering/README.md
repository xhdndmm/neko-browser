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
- 粗体/斜体变体匹配（font-weight/font-style → 相邻字体文件，如
  LiberationSans-Bold/-Italic，缺失时回退常规字形）
- 页面内 `<img>`：子资源抓取 → `neko::image` 解码注入 → 行内原子盒（与文字
  同行，replaced 尺寸：固有/显式宽高/比例保持、width/height 属性）→ DrawImage
  按 object-fit（fill/contain/cover/none/scale-down）绘制，vertical-align
  对齐（baseline/middle/top/bottom）
- `display:inline-block`：行内级原子盒，内部为块格式化上下文（块子元素垂直堆叠），
  宽度显式或 shrink-to-fit（CSS2.1 10.3.9），background/border/padding、行高参与
  行盒，嵌套于任意行内元素中亦可
- **性能（ADR 0013）**：
  - 显示列表缓存：`Page` 按版本号增量重建 Painter 输出（内容未变不重生成绘制指令）
  - 并行带栅格化：`RasterizeParallel` 把页面按水平带在共享线程池并行光栅化，
    带视图与原语坐标平移 + 裁剪保证与串行逐像素一致
  - 缓冲复用（`Resize` 不重分配）、整数定点 alpha 混合、分带清屏/可见带裁剪
  - 滚动 blit：`WebView` 视口光栅缓存，滚动时 `ShiftRows` 内存搬移 + 仅补绘露出带
  - 字体/字形缓存线程安全（互斥锁 + 自持像素拷贝，修复 UAF）

## 未实现

- HarfBuzz 文本整形
- 精确字体基线（`<img>` 的 baseline 对齐近似为文本底边，未含 descender）
- 图片增量加载/懒加载、alt 文本渲染、srcset
- `text-align` 对齐、连字符断行、CJK 逐字断行
- 完整系统字体目录扫描（当前内置候选路径表；具体名按文件名匹配）
- GIF 动画、WebP/AVIF、渐变、变换、滤镜、分层合成、GPU 后端
- 布局增量失效（当前布局每次全量重算；显示列表/光栅化已增量）

## 架构

```text
Layout Tree → Paint → Display List → Rasterization → Compositor(计划) → Window(计划)
                 ↓
             neko::graphics (FreeType 封装)
```
