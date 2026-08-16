# 架构决策记录 0015：软件合成器抽象层（Compositor 缝）

- 状态：**Accepted**（2026-08）
- 决策者：架构组

## 背景

渲染架构（AGENTS.md §23 / architecture.md）要求
`Layout Tree → Paint → Display List → Rasterization → Compositor → Surface`，
并明确"长期支持 OpenGL/Vulkan/Metal/Direct3D，使用图形抽象层"。
此前渲染链的终点是 `paint::Rasterizer` 的像素缓冲，由 `WebView` 直接
blit 到窗口：滚动 blit（`ShiftRows`）、分带重栅格化、caret 绘制都散落在
UI 层，没有"合成"这一层的归属。任何未来的 GPU 后端都无处安放，而把
GPU 代码直接塞进 `WebView` 会破坏 UI↔引擎的边界。

## 决策

- 新增 **`neko::compositor`** 模块，定义两条边界：
  1. **`Surface`**：自有的 RGBA8888 像素缓冲 + 拷贝/滚动原语
     （`CopyFrom/CopyRect/CopyBand/CopyPixels(Band)` 直拷、
     `BlendOver` 按 straight-alpha "over" 混合、`ShiftRows` 滚动 blit），
     缓冲在 `Resize` 间复用（同 Rasterizer）。
  2. **`Compositor` 接口**：拥有输出 `Surface` 与有序图层列表
     （`Layer{surface, x, y, opacity, visible}`）；`Composite(clear)` 全量
     合成（清屏 → 图层 0 直拷 → 覆盖层按序混合）、`CompositeRect` 脏矩形
     重合成（从图层 0 还原背景 + 重混相交覆盖层）、`ScrollOutput(delta)`
     滚动输出并报告暴露带。覆盖层像素随内容一起滚动（"粘"在页面上）。
- **`SoftwareCompositor`** 是第一个实现：CPU 逐像素合成，混合数学与
  `paint::Rasterizer::BlendPixel` 完全一致（整数定点、相同舍入），保证
  合成输出与光栅化输出逐像素可比。
- **依赖方向**：`compositor` 只依赖 `css`（颜色）+ `base`，**不依赖**
  `paint`/`renderer`——GPU 实现可以复用同一接口而不引入软件光栅化器。
  UI（`WebView`）通过 `std::unique_ptr<Compositor>` 使用它。
- **GUI 接线**：`WebView` 的呈现链改为
  `Rasterizer（页面光栅缓存）→ 图层 0（直拷页面像素）→ [图层 1 = caret
  覆盖层] → Composite → 输出表面 → QImage blit`。
  - 全量重绘：`RasterizeFull → CopyPixels(图层 0) → UpdateCaretLayer →
    Composite(白)`。
  - 滚动：`图层 0.ShiftRows + ScrollOutput（输出与覆盖层一起滚动）→
    仅重栅格化暴露带 → CopyPixelsBand 进图层 0 与输出`。图层 0 始终是
    可见页面的精确镜像，脏矩形重合成不会还原出过期行。
  - caret 闪烁/移动：`CompositeRect(旧矩形 ∪ 新矩形)` 增量重合成。
- 覆盖层目前只有 caret（1px 竖线，blink 由 GUI 定时器驱动）；未来
  `<video>` 独立层、滚动条覆盖层、GPU 纹理层都挂同一接口。
- 诚实边界：**GPU 合成尚未实现**；`SoftwareCompositor` 是合成缝的第一个
  真实实现，GUI 已实际使用（不是孤立的库代码）。

## 备选方案

- **直接给 Rasterizer 加"图层"概念**：把合成塞进光栅化器会模糊两条
  管线的职责（光栅化=显示列表→像素；合成=像素→像素），且 GPU 后端不
  需要光栅化器，耦合反而成为迁移包袱。
- **WebView 继续直连 Rasterizer，只留接口文档**：没有真实消费方，
  抽象层形同虚设（违反 AGENTS.md "不做伪实现"）；本次接线让滚动 blit、
  增量重合成都成为合成器的真实职责。
- **完整 Dirty-Rect 追踪 / 分层光栅化（每层独立 DisplayList）**：
  当前页面仍是"整页一张位图"，没有把页面拆成多层的真实需求
  （无独立滚动的层、无合成加速的需求）；等 `<video>` 覆盖层或 GPU
  后端出现时再扩展，避免过早设计。

## 后果

- 优点：合成职责有归属、可测试（17 个单元测试覆盖拷贝裁剪/混合/滚动/
  层级/脏矩形）；滚动快路径保持（滚动仍是带级 blit，无全视口重算）；
  GPU 后端只需实现同一接口；UI 不再直接操作光栅化缓冲。
- 缺点：多一份视口大小的输出缓冲（约 1100×800×4 ≈ 3.5 MB）与每次重绘
  一次全视口拷贝（memcpy 级开销，远低于光栅化本身）；caret 进入合成
  输出后，blink 需要脏矩形重合成来擦除旧位置（已实现并有测试）。
