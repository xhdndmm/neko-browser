# Rendering 模块

> 状态：**Not Started**（计划 Phase 6）

## 职责

```text
Layout Tree → Paint → Display List → Rasterization → Compositor → Window
```

- 图形抽象层：`{Software, OpenGL, Vulkan, Metal, Direct3D}`
- background / border / text / images / clipping / scrolling
- 后续：layers、compositing、GPU、animations、transforms、filters

## 约束

- 渲染器不得直接绑定平台 API
- 核心引擎不因图形后端差异而改变语义
