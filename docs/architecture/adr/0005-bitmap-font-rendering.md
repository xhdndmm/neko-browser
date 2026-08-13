# 架构决策记录 0005：Phase 6 文本渲染使用内嵌位图字体

- 状态：**Accepted**（2026-08）
- 决策者：架构组

## 背景

渲染里程碑（Phase 6）需要文本绘制。完整方案（FreeType + HarfBuzz）会带来：
- 两个 C 库依赖（构建复杂度、跨平台差异）
- 运行时系统字体查找（CI/容器里不可复现）
- 整形、回退、度量的大量正确性工作

而 Phase 6 的核心目标是验证"布局 → 绘制 → 光栅化 → 输出"整条管线。

## 决策

- 使用**公有领域 8x8 位图字体**（font8x8_basic，来源与许可见
  `src/paint/include/neko/paint/font8x8.h` 头注释），渲染 ASCII 32–126。
- 字符步进 = font_size（方块字体），字形按比例缩放（最近邻）。
- 后续里程碑再引入 FreeType/HarfBuzz 抽象层（`docs/rendering/README.md`）。

## 后果

- 优点：零依赖、可复现、管线正确性可先行验证。
- 缺点：仅 ASCII、无整形/回退/复杂度量 —— 已在兼容性矩阵标注为 Partial。
