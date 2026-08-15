# 架构决策记录 0013：渲染性能——缓存、并行栅格化与滚动 blit

- 状态：**Accepted**（2026-08）
- 决策者：架构组

## 背景

渲染路径（Layout → Paint → Rasterize）此前每次绘制都全量重做：页面内容
未变时也重新走 Painter 生成绘制指令、重新光栅化整张位图；滚动时整页重新
光栅化；字体缓存跨线程访问无保护（存在 UAF）。中文站点（QQ、Bilibili 等）
页面元素多、字体渲染重，性能问题明显。

目标：**内容未变不重绘，滚动只补绘露出带，绘制并行化，缓存线程安全**。
为未来多进程/合成器架构保留正确边界（渲染管线仍集中在渲染线程，缓存可在
GUI 线程安全读取）。

## 决策

### 1. 显示列表缓存（Page 层）
- `Page` 持有 `mutable std::optional<paint::DisplayList> display_list_` 与
  单调递增版本号 `version_`。
- 任何内容变更（加载、重应用样式、外部样式表、元素图像、布局）都
  `BumpVersion()`；`EnsureDisplayList()` 仅在版本变化时重建 Painter 输出。
- 语义：**布局仍然每次发生**（布局树是当前架构的真实状态），缓存的是
  "绘制指令生成"这一步（Painter 遍历布局树 → DisplayList）。布局计算本身
  的增量失效留待后续（有明确测量需求后再做）。

### 2. 并行带栅格化（Rasterizer 层）
- `Rasterizer::RasterizeParallel(list, pool, min_band_height)` 把页面按
  水平带划分，各带在 `base::ThreadPool` 上并行光栅化。
- 带视图是**同缓冲区的子区间视图**：新增 `band_y0_/band_y1_/band_origin_y_`，
  所有绘制原语经 `BandOffset()` 平移坐标并用 `ClampToBand` 裁剪，保证
  带间结果与串行光栅化**逐像素一致**（有 `ParallelMatchesSerial` 回归测试）。
- 整数定点 `BlendPixel` 替代浮点 alpha 混合（同结果、更快、无浮点噪声）。
- `Resize` 复用已分配缓冲（尺寸不变不重分配）；`ClearBand` 只清可见带。

### 3. 滚动 blit 视口缓存（UI 层）
- `WebView` 持有 `raster_cache_`：内容/尺寸/布局版本未变时，滚动仅
  `ShiftRows(delta)` 内存搬移上一帧像素，再只光栅化新露出带
  （`RasterizeInto` + `ClearBand`），最后 blit 到 QImage。
- 命中条件：宽高相同、布局版本相同、仅滚动偏移变化。

### 4. 字体/样式缓存线程安全
- `GlyphCache`：进程级 LRU 加互斥锁，`GetOwned` 在锁内拷贝像素数据；
  `Insert` 修正条目自身 `bitmap.data` 指向自身 storage（**修复 UAF**——
  此前缓存了指向调用方临时缓冲的指针）。
- `FontFace`：FreeType 全部操作（HasGlyph/Advance/RenderGlyph/Ascent/
  Descent）加 `ft_mutex`；`RenderGlyph` 返回自持拷贝而非裸指针。
- `FontRegistry` 选择器缓存与 `FontSelector::TextWidth` 记忆化均加互斥锁
  （TextWidth 缓存上限 4096，满则清空）。

### 5. 共享线程池
- `BrowserController` 持有唯一 `base::ThreadPool`；子资源抓取、图像解码与
  并行栅格化复用同一池，避免每任务建池的开销。

## 备选方案

- **整页重光栅化**（原状）：实现最简单，但滚动/重复绘制 O(整页) 且无法
  并行化，否决。
- **脏矩形 + 增量光栅化**：更精细，但需要布局/绘制级失效追踪，当前无测量
  需求支撑，留作后续优化（避免过度设计，AGENTS §45）。
- **合成器/GPU 层**：Phase 12 目标，当前软件渲染架构预留接口即可。

## 后果

- 页面重绘/滚动显著加速（blit O(带宽) 而非 O(整页)）；大页面渲染可按核
  数并行。
- 渲染管线仍为单线程所有者 + 线程安全缓存：为未来多进程渲染进程模型
  保留正确边界。
- 新增测试：缓冲复用、滚动 blit 上下等价、并行=串行、可见带/清带、
  整数混合、布局版本失效、字体缓存并发。
