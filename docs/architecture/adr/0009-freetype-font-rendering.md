# 架构决策记录 0009：文本渲染迁移到 FreeType

- 状态：**Accepted**（2026-08）
- 决策者：架构组

## 背景

ADR 0005 用公有领域 8x8 位图字体渲染 ASCII 32–126，作为 Phase 6 管线验证的
零依赖起点。它的限制（仅 ASCII、无 hinting/抗锯齿、等宽步进）在真实页面
（如 kaom.net，大量中文）上表现为**非 ASCII 字符完全空白**，且布局层等宽
假设与真实字形度量脱节。现在需要真实的字体光栅化。

## 决策

- 使用 **FreeType** 作为字体光栅化引擎，通过系统包引入（`find_package`），
  与 zlib / libjpeg / Qt6 的引入方式一致；CI 三平台显式安装
  （Linux `libfreetype-dev`、Windows vcpkg `freetype`、macOS brew `freetype`）。
- FreeType API **只允许出现在新模块 `neko::graphics`** 内（`src/graphics/`）：
  `FontLibrary`（初始化/字体文件池）、`FontFace`（字形度量与光栅化）、
  `GlyphCache`（LRU 字形缓存）、系统字体发现（内置候选路径表，不引入
  fontconfig）。核心代码不得直接依赖 FreeType 类型（依赖政策 §3）。
- `Rasterizer::DrawText` 切换为 FreeType 灰度字形（抗锯齿、任意字号），
  UTF-8 解码后按码点光栅化，字符按真实 advance 步进。
- 现有 8x8 位图字体**保留为 fallback**：字体库初始化失败或找不到任何系统
  字体时回退，页面不空白。
- 里程碑：M1 光栅化切换（本 ADR 范围，ASCII 先受益）→ M2 布局测量贯通
  （真实 advance 进 layout/命中测试）→ M3 `font-family` 匹配 + 中文回退链
  → M4（后续）HarfBuzz 整形、粗斜体变体。

## 备选方案

- **stb_truetype（单头文件）**：零构建依赖，但无 hinting、CJK 质量差，
  且后续做整形仍需 FreeType，属重复投入。
- **自绘 CJK 点阵**：数千字模工作量，质量远差。
- **Qt QFont 渲染**：破坏 renderer 平台独立原则（headless 也依赖 GUI）。
- **FreeType 走 FetchContent**：可复现性最好，但 FreeType 源码构建慢、
  且系统已普遍自带；与 zlib/libjpeg/Qt 的"系统包"惯例不一致。

## 许可证

- FreeType 双许可：**FreeType License（FTL）** 或 GPLv2，二选一。
  选择 **FTL**：BSD 风格宽松许可（商用/闭源/修改/再分发均可，仅要求保留
  版权与许可声明），与项目 Unlicense 兼容（FreeType 组件保留 FTL 声明）。
  官方注明 FTL 与 GPLv3 兼容、与 GPLv2 不兼容——对 Unlicense 项目无影响。
- 加载的**系统字体文件**（DejaVu / Noto / 微软雅黑等）仅运行时引用、
  不随项目重新分发，各自许可（OFL / 专有）不构成项目负担。

## 后果

- 优点：ASCII 文本立即获得真实字形与抗锯齿；任意字号；为 M3 中文显示与
  后续整形铺路；8x8 fallback 保证无字体环境不空白。
- 缺点：新增一个系统包依赖（三平台 CI 各加一行安装）；系统字体文件差异
  导致跨平台渲染不完全一致（渲染测试不做逐字节对比，见 AGENTS.md §33；
  单元测试对无字体环境用 skip 策略）；M1–M2 之间布局仍按等宽假设排版，
  字距为中间态（M2 统一真实度量）。

## 参考

- https://freetype.org/license.html
- https://freetype.org/
- docs/development/dependency-policy.md
- ADR 0005（bitmap font rendering）
