# 架构决策记录 0021：表单控件的渲染架构（UA 声明 / 布局分类 / 绘制分流）

- 状态：**Accepted**（2026-09）
- 决策者：架构组
- 关联：WHATWG HTML Rendering §15.3.1、§15.3.10–§15.3.12、§15.5（Widgets，含
  §15.5.1–§15.5.17）；CSS-UI-4 §7.2；ADR 0013（渲染缓存/并行；paint 依赖 layout）；
  `docs/compatibility/compatibility-matrix.md`（表单控件行）；`tests/pages/forms.html`；
  AGENTS.md §2.1/§45/§46/§62。

## 背景

表单控件是引擎里第一类「外观由 UA 规定、却又要能被作者 CSS 覆盖」的元素。旧实现
把问题压平成一个函数：`layout::BuildFormControl()`（`src/layout/src/layout.cpp`）无条件
把所有 `<input>` 当成「白底 + 1px 边框 + padding 4/2 + **硬编码 170px 宽** + 单行文本 run」
的盒子，**完全不看 `type`**。于是 `<input type=checkbox>`、`radio`、`range`、`color`、
`file`、`submit`、`reset` 全部退化成同一个 170px 白色文本框；UA 表里 `input`/`select`
是 `display:inline`，`[hidden]` 呈现提示没有任何实现，`<textarea>` 的 `white-space:pre-wrap`
也没声明。

正确做法必须同时满足规范的三类要求，而它们分属管线不同阶段：

| 规范要求 | 出处 | 自然归属阶段 |
| --- | --- | --- |
| 计算样式（`display`/`box-sizing`/`text-align`/`white-space`/`appearance`） | §15.3.10 | 样式层（UA 样式表） |
| 固有几何（`size`/`rows`/`cols` → 像素宽高、`flow-root` 内显示类型） | §15.5.6/§15.5.16/§15.5.17 | 布局层 |
| 原生外观（对勾、圆点、滑块、色块、文件按钮、下拉箭头） | §15.5.1/§15.5.8–§15.5.16 | 绘制层 |

目标：**用最小的正确切分让这三类要求各自落在能真正实现它的阶段**，而不是把控件
外观硬编码进任何单一阶段（AGENTS §2.1 禁止假实现、§45 避免过度设计）。

## 决策

### 1. 三层分工：UA 样式表声明 + 布局层原子盒 + 绘制层 widget 分流

```text
dom::Element（HTML 语义：tag + type 属性）
        │
        ▼
style：UA 样式表声明（§15.3.10）—— 只产出 CSS 计算值
        display:inline-block / box-sizing:border-box / text-align:center /
        white-space:pre-wrap / appearance:auto …（getComputedStyle 返回规范值）
        │
        ▼
layout：FormControlKind 分类 + 固有几何（§15.5）—— 只产出几何
        ClassifyFormControl(element) → 文本类按 size(缺省 20) 字符宽、
        textarea 按 rows×cols、checkbox/radio 13×13、button/select 各自规则；
        产出「原子行内盒 + 可选文本 run」，LayoutBox 携带 form_control 等字段
        │
        ▼
paint：widget 外观分流（§15.5.x native appearance）—— 只产出绘制指令
        读 LayoutBox::form_control，画对勾/圆点/滑块/色块/按钮/箭头
```

**为什么不能压进一个阶段：**

- 只放**样式**层：`<input type=text>` 的详细宽度是 `(size-1)×avg + max`（§15.5.6 的
  converting a character width to pixels），依赖**主字体度量**——样式层没有字体选择器，
  强行实现会让 `getComputedStyle` 与真实几何互相说谎（AGENTS §2.1）。
- 只放**布局**层（现状的延伸）：对勾、滑块、下拉箭头是**光栅化阶段**的东西，布局产出
  几何而非像素；把像素画法塞进布局会让布局盒同时承担两个阶段的职责，且无法被
  显示列表缓存/合成器复用（ADR 0013/0015）。
- 只放**绘制**层：`size`→宽度的推导需要字体度量与盒模型，绘制层拿不到；而且
  `getComputedStyle(input).display` 仍会返回 `inline`，规范要求 `inline-block`。
- 三层各司其职后：**作者 CSS 覆盖自然生效**（作者 `background`/`border` 写在样式层，
  绘制层的原生外观只在 `appearance` 允许时叠加，与浏览器一致）。

### 2. `FormControlKind` 放在 **layout** 模块

新头文件 `src/layout/include/neko/layout/form_control.h`，定义：

```cpp
enum class FormControlKind { kNone, kHidden, kText, kButton, kCheckbox, kRadio,
                             kRange, kColor, kFile, kSelect, kTextArea };
FormControlKind ClassifyFormControl(const dom::Element&);
bool HasFlowRootInnerDisplay(const dom::Element&);
```

**理由（谁最早需要 + 依赖方向 + 是否成环）：**

- **谁最早需要**：`layout::BuildFormControl()` 必须先知道控件是哪一类，才能选几何规则
  （§15.5.6/§15.5.16/§15.5.17 各不同）。分类是布局的第一个消费者。
- **依赖方向**：现有依赖为 `paint` 依赖 `layout`、`layout` 依赖 `style` 与 `dom`、`style` 依赖 `dom`
  （`neko_layout` PUBLIC 链接 `neko::style neko::dom neko::css neko::graphics`；
  `neko_paint` PUBLIC 链接 `neko::layout`）。`ClassifyFormControl` 只读 `dom::Element` 的 tag
  与 `type` 属性，天然的层级在 dom 之上、paint 之下。
- **不放 dom**：分类是**渲染/控件概念**（HTML Rendering 章按元素「状态」定义），
  DOM 层应保持与呈现无关（AGENTS §62/§63：依赖朝向低层抽象，DOM 不应依赖渲染语义）。
  放进 dom 会把「`type=checkbox` 是个复选框控件」这种呈现判断固化成 DOM 语义。
- **不放 style**：style 层负责的是 **CSS 属性值**，不是 HTML 元素状态分类；把它放进
  style 会让 layout 为了一个**非 CSS** 关切而依赖 style 的新头文件，并在两模块间
  来回引入头文件。`HasFlowRootInnerDisplay` 也是格式化上下文概念，本就属布局。
- **不成环**：layout 已经 include dom/style/css/graphics；新增一个 layout 本地头文件
  不引入任何反向边。paint 经 `LayoutBox`（已 include layout）读取分类，无需新增依赖。

> 若后续发现分类需要 CSS 值（如 `field-sizing`）参与，可考虑将它抽到独立的
> `formcontrol` 低层模块；当前无此需求，按 AGENTS §45 不提前抽象。

### 3. `appearance` 的 devolved / native 状态：已知简化

`style::Appearance` 当前只有三个取值（`src/style/include/neko/style/computed_style.h`）：

```cpp
enum class Appearance { kNone, kAuto, kButton };
```

- `kNone`：不使用原生外观，作者 CSS 完全生效。
- `kButton`：任何元素强制按钮外观。
- `kAuto`：**目前在绘制层硬映射到 `tag == "button"`**（`paint::HasNativeButtonAppearance`）；
  `input`/`select`/`textarea` 的 `auto` 不产生原生外观。

规范 §15.5.1 定义了 widgets 的 **devolvable / non-devolvable** 分类，以及
**devolved / native / primitive** 三种外观状态，并让 CSS-UI-4 的 `appearance` 在一个
状态机里切换它们。**本引擎未建模该状态机**，也无 `checkbox`/`radio`/`textfield`/
`menulist-button` 等兼容关键字（这些声明被忽略，计算值停在初始值 `none`）。

这是**有意的简化**：在绘制层尚未实现任何控件外观之前，建立完整状态机是空转的
抽象（AGENTS §45）。矩阵的「`appearance` / `<button>`」行与「表单控件」行已如实标注
为 Partial 并逐条列出缺口。完整状态机待 Wave 2 绘制层就绪、且出现真实需要时再引入。

### 4. 不做 shadow DOM / 内部 shadow tree

规范 §15.5.16 要求 drop-down `select` 拥有**内部 shadow tree**（slot + “drop-down button”），
§15.5.5 对 `details` 有同样要求（默认 summary、`::details-content`）。

**本引擎不实现它。** 理由：引擎当前没有 shadow DOM（无样式作用域、无事件重定向、
无 slot 分配）。仅为控件外观建立一套影子树，需要引入整套机制，与「让控件看起来对」
的收益不成比例（AGENTS §45）。

**未来若要做，边界划在这里：** 控件内部内容建模为**布局/绘制层由 UA 生成的内部盒树**
（挂在控件 `LayoutBox` 之下、由绘制分流消费），对应 `::file-selector-button` / `::marker` /
`::details-content` 等伪元素——**不经 DOM、JS 不可观测**。真正的 Shadow DOM 是独立的
DOM 语义特性，届时另立 ADR，不把它与控件外观耦在一起。

### 5. 本 ADR 覆盖范围之外（Wave 2+）

- **绘制层 widget 外观**：对勾、圆点、滑块轨道、颜色色块、文件按钮、下拉箭头。
- **交互层**：`checked` 点击/Space 切换、`:checked` 伪类刷新、Tab 焦点遍历、
  hover/active/disabled 伪类与样式。
- **内容垂直居中**：规范用 `align-content`（§15.3.10/§15.5.3），引擎未实现该属性。
- **`details`/`summary` 的 ▸ 三角与折叠**：被选择器/`list-style-type` 能力缺口阻塞（见下）。
- **`field-sizing`、`::file-selector-button`、IME、剪贴板、就地编辑、`maxlength`。**
- **`input[type=password]` 的掩码**：value 仍按明文文本 run 渲染。
- **控件字符宽度无缓存**：§15.5.6 的 avg/max 字符宽度每次布局重新逐字测量
  （ASCII 0x20–0x7E），控件密集的页面是性能热点，需按 (family, weight, italic,
  size) 缓存。

`[hidden]` 呈现提示（§15.3.1）当初列为本决策范围之外，**已在 Wave 1 内实现**：
呈现提示层已能作用于任意元素，并按 CSS Cascade 5 §6.1 输给任何作者
`display` 声明（`style_test.cpp` 有作者覆盖的回归测试）。`hidden=until-found`
与 `<embed>` 例外仍未实现。

### 6. 长期技术债：选择器模型能力缺口如何影响规范条文落地

本引擎选择器模型（`src/css/include/neko/css/selector.h`）的能力缺口，使若干规范条文
**无法照抄进 UA 表**，只能改写或绕行：

| 缺口 | 影响的规范条文 | 落地方式 |
| --- | --- | --- |
| 无 `:is()` / `:where()` | §15.3.10 的 `input:is([type=reset i], …)` | **展开**为逗号分隔列表；`:is()` 取参数最高特异性，而每条展开后的复合选择器都是 `(0,1,1)`（一个属性选择器 + 一个类型选择器），特异性一致，可安全展开 |
| `:not()` 仅支持简单子集（type/#id/.class/[attr]，无列表、无组合器） | §15.3.10 的 `input:not([type=file i], …)` | 多条 `:not([type=x])` 需要**合取**展开或改走代码路径 |
| 无 `:first-of-type` / `:nth-of-type` | §15.5.5 的 `summary:first-of-type` | 无法表达 ⇒ details 三角**被阻塞** |
| 无 `[attr=i]` 大小写不敏感标志 | §15.3.1 的 `input[type=hidden i]`、§15.5 各处 `[type=… i]` | UA 表写不了；**改走代码路径**——`ClassifyFormControl()` 自行做 ASCII 大小写不敏感比较 |
| `list-style-type` 仅 disc/circle/square/decimal，无 `disclosure-*`，无 `::marker` | §15.5.5 | details 三角**被阻塞** |

由此产生一条**长期技术债**：规范大量使用 `[attr=i]` 与函数型伪类表达 HTML
enumerated 属性的**大小写不敏感**语义。更规范的做法是**在属性选择器取值比较的那一层
对 enumerated 属性做 ASCII 小写归一**（对作者 CSS 与 `querySelector` 一并生效），而不是
逐条在 C++ 里绕行。此项涉及 `src/css`，超出本决策范围，作为 CROSS-LANE 请求上报。
在此之前，控件 type 的大小写不敏感只由 `ClassifyFormControl()` 的代码路径保证。

## 补充决策（Wave 1 内追加）：层叠补上 origin 维度

本决策把控件默认值写进 UA 表，而规范条文大量使用 `:is([type=… i], …)`，展开后是
`(0,1,1)` 的属性选择器。当时层叠比较是 `importance > specificity > order`，**没有
origin**，于是 UA 的 `input[type=submit] { text-align: center }` 会压过作者
`input { text-align: right }`（`(0,0,1)`）—— 而 CSS Cascade 5 §6.1 规定作者普通声明
永远高于 UA 普通声明。

因此层叠比较改为先比 **origin/importance 秩**再比特异性：UA 普通 `0` < 作者普通 `1`
< 作者 `!important` `2` < UA `!important` `3`（引擎无 user origin、动画与过渡，
故只需四个秩）。这是一个**独立于表单控件的既有缺陷**（`ul ul ul { list-style-type:
 square }` 长期有同样形状），本次因为新增高特异性 UA 规则而必须修复。

`style_test.cpp` 的 `AuthorDeclarationBeatsHigherSpecificityUaRule` 钉住规范行为。

## 备选方案

- **A：把外观一并塞进 `BuildFormControl()`（旧实现的自然延伸）。** 否决：布局产出几何，
  把对勾/滑块画进来会让布局同时干光栅化的活，且 `getComputedStyle` 仍返回 `inline`，
  违反 §2.1/§46。
- **B：把分类与外观都放进一个大的绘制层函数。** 否决：绘制层拿不到字体度量，`size`
  →宽度无法推导；且样式层无法被作者覆盖。
- **C：为控件实现 shadow DOM。** 否决：为一个外观特性引入整套影子树/样式作用域/事件
  重定向，收益不成比例（§45）；留作独立 DOM 特性的将来工作。
- **D：实现完整的 `appearance` devolved/native/primitive 状态机。** 推迟：在绘制层就绪
  前是空转抽象；当前用 `kNone/kAuto/kButton` 覆盖 CSS-UI-4 中最常用的一档。

## 后果

- 优点：三类要求各落在能真正实现它的阶段，每层可独立测试（样式计算值 / 布局几何 /
  绘制指令）；`getComputedStyle` 返回规范值；作者 CSS 覆盖自然生效；绘制层可后补而不
  改布局契约。
- 缺点/边界：`FormControlKind` 是 layout↔paint 之间的**新缝**，必须保持稳定——
  Wave 1 与 Wave 2 之间以冻结契约（`src/layout/include/neko/layout/form_control.h`
  与 `LayoutBox` 的 `form_control*` 字段）为准，Wave 2 只读依赖、不反向修改。
- 诚实边界：`appearance:auto` 仅对 `<button>` 生效；`select` 的 optgroup 表头与逐项
  option 渲染、`textarea` 的 `pre-wrap` 多行渲染、`details` 三角、password 掩码
  均**未实现**，已在兼容性矩阵逐条列出。`select` 的 list box / drop-down 区分已
  体现在**几何**上（`size`/`multiple` 定高），但渲染内容仍是单个选中项标签。
- 验证：布局单元测试（按 type 的几何）、样式单元测试（`display`/`box-sizing`/
  `text-align`/`white-space` 计算值）、渲染回归基线页 `tests/pages/forms.html`。
