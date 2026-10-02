# Rendering 模块

> 状态：**Implemented**（Phase 6，软件后端子集）

## 已实现

- Layout Tree → Paint → Display List → 软件光栅化 → PPM 输出
- background / border / text 绘制、alpha 混合、裁剪
- 文本：FreeType 灰度字形（`neko::graphics`，抗锯齿、UTF-8、glyph 缓存；
  见 ADR 0009），无字体时回退内嵌 8x8 位图字体（ASCII；ADR 0005）
- 布局文本宽度按真实 advance 测量（词宽/空格/命中测试），与绘制一致
- 换行：CJK 逐字断行（行盒）与 **CJK min-content 逐字测量（2026-10）**——
  最小内容宽按“每个 CJK 字符一个不可断单元”测量，原按空格分词使整段中文成为
  一个不可断词（flex `min-width:auto` 无法收缩，标题溢出卡片）；无字体回退
  按码点计数
- `visibility`（2026-10，CSS 2.2 §11.2）：hidden 盒保留布局但不绘制自身
  装饰/内容（含文本 run、行内替换盒），后代 `visibility:visible` 可恢复；
  命中测试未跳过隐藏元素（见兼容性矩阵）
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
    带视图与原语坐标平移 + 裁剪保证与串行逐像素一致。**2026-10 已接入全部
    三条曾串行的调用路径**：GUI 帧生产（`ProduceFrame` → `pool_`）、renderer
    子进程截图、CLI 分带截图（`Page::RasterizeInto` 新增可选池参数）；调用
    线程亲自光栅化第 0 带、只向池提交 `带数-1` 个任务，保证任务数严格小于
    worker 数（调用方可能持 DOM 锁，而池线程可能阻塞在该锁上，见
    threading-model §3.3 的死锁约束）
  - 文本与绘制命令按可见带提前跳过（`RowRangeVisible`）：带视图会遍历整条
    显示列表，若不跳过，每个带都会重做字体选择与字形缓存查找，而字形缓存
    是「全局锁 + 自持像素拷贝」，多线程下互相踩踏会把并行变成串行。实测
    （Debug、78k 行 ×1500 div 文本页、CLI 截图）：未跳过时墙钟 9.3s / 总
    CPU 42s，跳过后 0.95s / 2.3s，输出与串行逐字节一致（1.92s / 1.87s）
  - 缓冲复用（`Resize` 不重分配）、整数定点 alpha 混合、分带清屏/可见带裁剪
  - 字体/字形缓存线程安全（互斥锁 + 自持像素拷贝，修复 UAF）
- **软件合成器（ADR 0015）**：`neko::compositor` 定义 `Surface`（RGBA8888
  缓冲 + 拷贝/混合/滚动原语）与 `Compositor` 接口（输出表面 + 有序图层：
  全量 `Composite`、脏矩形 `CompositeRect`、`ScrollOutput` 滚动 blit 并报告
  暴露带）；`SoftwareCompositor` 是 CPU 实现（混合数学与 Rasterizer 一致），
  滚动为带级 blit（图层 0 与输出同步移动，无全视口重算）。**GPU 后端
  （ADR 0017，2026-10）**：Linux EGL/OpenGL 3.3 core，运行时加载、headless
  （surfaceless/GBM/默认显示），整数 shader 与软件合成**逐字节一致**（设备
  回读断言，无 GPU 自动跳过）。**注意**：ADR 0020 之后 GUI 走 worker 不可变帧
  直绘（caret 由 Qt 覆盖绘制），不再经过合成器；合成器作为渲染管线缝保留，
  重接线随图层化渲染推进

## 未实现

- HarfBuzz 文本整形
- 精确字体基线（`<img>` 的 baseline 对齐近似为文本底边，未含 descender）
- ~~图片增量加载/懒加载~~（2026-10 已实现：认领式晚到抓取 + `img` load/error
  事件 + 滚动感知 IntersectionObserver，见兼容性矩阵“图片子资源生命周期”
  与“IntersectionObserver”）；alt 文本渲染、srcset/sizes 选择仍未实现
- ~~块级/浮动/Flex 子项替换元素像素绘制~~（2026-10 已实现：`BuildBlock`
  （含表格单元格/grid 项）/`BuildFloat`/`BuildFlexItem` 挂载解码像素 +
  replaced 尺寸；此前 `display:block` 封面画成灰块）与 ~~抓取完成重绘~~
  （晚到 pass 置脏帧泵，否则封面到位后界面不刷新）；**晚到附着不再重复
  布局**（2026-10）：尺寸确定的图片只就地更新布局盒/行内盒的图像指针，
  不重建布局树（逐张懒加载封面时，每张一次全量重排把滚动拖死；固有
  尺寸盒仍重建）
- `text-align` 对齐、连字符断行
- 完整系统字体目录扫描（当前内置候选路径表；具体名按文件名匹配）
- `<video>` 播放的音频轨道、controls 与缓冲（视频帧动画已接入，见渲染器
  `Page::AdvanceAnimations`；直链 MP4/WebM 解码为**有界前缀**——预算命中截断
  并置 `VideoClip::truncated`）；**MSE（`MediaSource` + `blob:`）NOT
  IMPLEMENTED**，bilibili 等站点播放器走此路径，画面为空
- GPU 窗口呈现（swapchain）与 Vulkan/Metal/D3D11 后端（Linux EGL/OpenGL
  已实现，见 ADR 0017）；GPU 光栅化
- 布局增量失效（当前布局每次全量重算；显示列表/光栅化已增量）

## 外部资源时序

- 外部样式表在脚本执行前抓取、解析并应用，使脚本读取到完整的作者样式级联。
- `@font-face` 字体在页面发布后由资源线程抓取；首屏可先使用可用的回退字体，字体注册成功后重应用样式并重建布局与显示列表。
- 单 worker 的资源线程池以串行方式抓取字体，避免在 worker 内提交任务后等待自身导致死锁；多字体且线程池至少有两个 worker 时并行抓取。

## 架构

```text
当前 GUI 路径：Layout Tree → Paint → Display List → Rasterization → RemoteFrame → QImage（Qt 直绘）
合成器缝（保留，未接 GUI）：Rasterization → Compositor（Software/GPU）→ Surface → Window blit
                 ↓
             neko::graphics (FreeType 封装)
```
