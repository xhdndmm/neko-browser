# 兼容性矩阵

> 本文档诚实记录每个特性的支持状态。**禁止**把"接口存在"写成"已实现"。
> 状态取值：Not Started / Planned / In Progress / Partial / Implemented / Tested。
> 最后更新：2026-08（box-sizing、white-space:nowrap、overflow 裁剪、HTTP 增量读取 +
> 无 close_notify 关闭兼容、GUI 多标签页/快捷键/DevTools 增强、地址栏编辑保护）。

| 特性 | 状态 | 测试证据 | 备注 |
| --- | --- | --- | --- |
| URL 解析 | Tested | 19 单元测试 | RFC 3986 相对解析样例 |
| HTTP/1.1 | Tested | 8 单元测试 | GET、chunked、重定向、Content-Length；**增量读取**（按 framing 精确读取响应体，而非读到关闭），Content-Length 截断校验（防截断攻击的完整性兜底） |
| HTTPS / TLS | Tested | 5 单元测试（本地 TLS 服务器 + 自签名 CA） | OpenSSL 封装（ADR 0010），证书+主机名校验、SNI、TLS≥1.2；gzip/deflate 协商；**兼容 CDN 无 close_notify 关闭**（sohu/bing 实测，完整性由 HTTP 层 Content-Length 校验兜底） |
| gzip/deflate | Tested | 12 单元测试（含链式编码、raw deflate、服务器往返） | RFC 7231 内容编码解码，64 MiB 输出上限 |
| HTML tokenizer | Tested | HTML 套件 | 完整 WHATWG 命名字符引用表（2125 项，含双码点，生成代码）+ RAWTEXT/RCDATA |
| HTML parser | Tested | HTML 套件 | 插入模式子集（无 table 容错）；隐含 p 闭合按 button 作用域判定 |
| DOM | Tested | DOM 套件 | 树操作、querySelector 子集 |
| CSS tokenizer/parser | Tested | CSS 套件 + 解析器健壮性回归 | 规则、声明、!important、@media；**分号/花括号/`@font-face` 等声明块不再导致死循环**（曾致 15 GB 内存暴涨）；外部 `<link rel=stylesheet>` 抓取+解析+应用（并行） |
| 选择器匹配 | Tested | CSS 套件 | 属性/伪类(:first-child/:last-child/:nth-child/:root)/组合器子集 |
| 级联 / 计算样式 | Tested | Style 套件 | 特异性、继承、内联样式；**规则按最右复合选择器分桶**（id/class/tag/universal），实测 6004 元素页面级联 0.688s→0.144s（约 5×） |
| CSS 自定义属性 | Partial | 4 Style 单元测试 | `--name` 定义 + `var()` 引用（含 fallback）、默认继承、var() 无法解析时声明无效；无嵌套 var()/同元素链式引用 |
| 逻辑属性 | Partial | 3 Style 单元测试 | inline/block-size、margin/padding-inline/block（1–2 值）、-start/end 长手属性、border-block-start/end、place-items→align-items；无 inline 轴 justify |
| CSS 数学函数 | Partial | 5 Style + 4 Layout 单元测试 | `calc()`（+/- 线性组合）、`min()`/`max()`/`clamp()`（可嵌套 calc 与 var()）、vw/vh/vmin/vmax 单位，布局时对包含块求值；无 calc * /、嵌套 min/max |
| aspect-ratio | Partial | 2 Style + 2 Layout 单元测试 | `1`/`16/9` 形式；宽度确定+高度 auto 时推导高度；显式高度优先；双 auto 忽略 |
| border-radius | Partial | 1 Style 单元测试 | 单一圆角（长度/百分比），背景按圆角绘制；无多值/椭圆/圆角边框 |
| Block layout | Tested | Layout 套件 | 盒模型、堆叠、宽度填充；**box-sizing: border-box 全局生效**（现代站点普遍 `*{box-sizing:border-box}`，修复了真实站点布局溢出/塌陷） |
| box-sizing | Implemented | 6 布局 + 3 样式单元测试 | content-box（初始）/ border-box：width/height、min/max、flex 项、absolute、表格、grid 尺寸均按 border-box 解析 |
| white-space | Partial | 2 布局 + 3 样式单元测试 | normal（初始）/nowrap（整段视为不可断行、溢出不换行）/pre/pre-wrap/pre-line（解析但按 normal 处理——无预格式化文本）；继承 |
| overflow | Partial | 2 绘制 + 3 样式单元测试 | visible（初始）/hidden/auto/scroll；hidden 与 scroll 按 padding box 裁剪（绘制级裁剪栈，嵌套交集），auto/scroll 无滚动溢出交互；`overflow-x/y` 简写未做 |
| Inline layout | Tested | Layout 套件 | 文字换行、行盒 |
| inline-block | Partial | Layout + Style + Paint 套件 | 行内原子块盒：显式/shrink-to-fit 宽度、内部块格式化上下文、background/border/padding、vertical-align 与行高参与行盒；精确基线对齐（多行内块按自身最后一行盒基线）未做 |
| Flexbox | Partial | 25 布局单元测试 + 9 样式解析测试 + 端到端截图 | display:flex/inline-flex、flex-direction row/column(+reverse)、flex-wrap、flex-grow/shrink/basis（含 flex 简写）、justify-content（6 值）、align-items（含 baseline）、align-content（确定 cross 尺寸时）、gap、order、align-self、min/max-width/height、auto margin（主轴/交叉轴）；无 flex-basis 百分比精确高度、flex 容器自身 min/max、grow 后剩余空间再分配（max-width 截断不回流） |
| Table layout | Partial | Layout 套件 | 行列网格、colspan/rowspan（含 rowspan=0 跨行组末尾、WHATWG 截断）、显式列宽、auto 列分配；无 border-collapse/vertical-align/caption |
| 超链接（`<a>`） | Partial | Browser + Renderer 套件 | 蓝色+下划线样式、命中测试、相对 URL 解析、点击导航；无 fragment 滚动与 :visited/:hover/:active 伪类 |
| Grid | Partial | 8 布局单元测试 + 5 样式解析测试 | display:grid、grid-template-columns/rows（px/%/fr/auto/min-content/max-content + repeat()）、row-major 自动放置、grid-column/row 行与 span 放置、column/row gap、隐式轨道按 auto 尺寸；无 inline-grid、命名区域、dense 打包、minmax()、grid-auto-flow 除 row 外、网格项目内绝对定位精确定位 |
| float | Partial | Layout + Style 套件 | 行外锚定 block 一侧（left/right）、行盒环绕让位、shrink-to-fit/显式宽高；无 clear、多 float 相交、跨 BFC 布局 |
| `appearance` / `<button>` 原生外观 | Partial | Style + Paint + Renderer 套件 | appearance none/auto/button（CSS-UI-4 §7.2）：button UA 默认 inline-block + 文本居中 + buttonface 背景与 outset 边框（作者 background/border 优先，与浏览器一致），button 可强制任意元素；无 hover/active/disabled 状态、box-sizing:border-box、min 尺寸、内容垂直居中 |
| position absolute | Partial | Layout 套件 | 包含块判定（最近 positioning 祖先 padding box）、top/left/right/bottom、shrink-to-fit 与 left+right 约束方程；fixed 暂按 absolute 处理，无 z-index/百分比 offset |
| 文本（位图字体回退） | Tested | Paint 套件 | 无系统字体时的 8x8 ASCII 回退 |
| 文本（FreeType） | Partial | Graphics + Paint 套件 | 系统字体、抗锯齿、任意字号、UTF-8、glyph 缓存、布局真实 advance、font-family 匹配、逐字符回退 + CJK 回退链（中文可显示）、粗体/斜体变体匹配；无 HarfBuzz 整形 |
| 图像解码 PNG | Tested | 16 图像单元测试 | 自研解码器（chunk/CRC/滤波/Adam7/全部颜色类型） |
| 图像解码 JPEG | Tested | 16 图像单元测试 | 封装 libjpeg，接口统一为 neko::image |
| 图像解码 GIF | Tested | 8 图像单元测试（含测试内 LZW 编码器） | 自研解码器（GIF87a/89a、全局/局部色表、LZW 变长码宽、交错、GCE 透明）；仅渲染首帧（无动画） |
| 图像解码 SVG | Partial | 6 图像单元测试 + 端到端截图 | 自研最小栅格化器：svg/g/a/rect(含圆角)/circle/ellipse/line/polyline/polygon/path（M/L/H/V/C/S/Q/T/A/Z + 相对）、fill/stroke/stroke-width/透明度、transform（translate/scale/rotate/matrix）、viewBox meet 居中、2× 超采样抗锯齿；无 <text>/渐变/图案/滤镜/use/clip-path 蒙版 |
| 图像解码 WebP/AVIF | Not Started | — | 返回显式 NOT IMPLEMENTED |
| 页面内 `<img>` 渲染 | Partial | Renderer + Layout + Paint 套件 | 子资源抓取+解码注入、行内原子盒（与文字同行）、replaced 尺寸（固有/显式/比例、presentational width/height）、object-fit fill/contain/cover/none/scale-down、vertical-align baseline/middle/top/bottom |
| 图像解码 WebP/AVIF | Not Started | — | 返回显式 NOT IMPLEMENTED |
| WAV 音频解码 | Tested | 13 媒体单元测试 | 自研 RIFF/WAVE，PCM+float，8/16/24/32-bit |
| 视频解码（H.264/VP9 等） | Not Started | — | 显式 NOT IMPLEMENTED，架构预留 |
| PDF 文本提取 | Partial | 12 PDF 单元测试 | xref（含 /Prev）、FlateDecode、内容流文本操作符；无 xref stream/渲染/CMap |
| Cookie（RFC 6265 子集） | Tested | 29 存储单元测试 | Set-Cookie、域/路径匹配、Max-Age、Secure/HttpOnly/SameSite；PSL 与 SameSite 强制标注为限制 |
| LocalStorage | Tested | 6 存储单元测试 + 浏览器生命周期接线 | 按 origin 分区的键值存储，行式文件 + 百分号编码 + 原子写入，已接入 Profile Load/Save/ClearAll；无配额、无 storage 事件、未接 JS（Phase 8 M2） |
| 历史记录 | Tested | 29 存储单元测试 | 去重访问、搜索、持久化 |
| 书签 | Tested | 29 存储单元测试 | 增删改、文件夹、持久化 |
| 下载器 | Tested | Browser 套件 | Content-Disposition/URL 文件名、原子写入 |
| 绘制 / 光栅化 | Tested | Paint 套件 | 纯色、边框、文字、PPM |
| 合成器 | Not Started | — | — |
| JavaScript（runtime，QuickJS） | Partial | 97 JS 单元测试 + 浏览器集成测试 + CLI/GUI 集成 | ES2025 核心语言、console、执行时限/内存上限、**DOM 绑定**（document/Node/Element/Text/Comment/DocumentFragment/CSSStyleDeclaration/**Event**）、**页内 `<script>` 执行（内联 + 外部 src=、async/defer）**、**最小事件循环**（setTimeout/setInterval 同步泵、**requestAnimationFrame**）、**事件传播**（capture→target→bubble、preventDefault/stopPropagation/stopImmediatePropagation、once/capture 选项、composedPath）、**microtask 泵送**、**localStorage/fetch（Phase 8 M3 子集）**、**window.location 导航**、**classList/dataset/matches/closest**、**表单控件 value/checked/type/placeholder/disabled/name**、**a.href/img.src 绝对化与 img.naturalWidth/Height/complete**、**getComputedStyle**（浏览器层接线 StyleEngine）、**history/performance.now**；无完整 Web IDL、无 WebSocket/XHR/sessionStorage、module 脚本不执行 |
| Fetch（浏览器 API） | Partial | JS 单元测试 + 浏览器集成测试 | window.fetch Promise<Response>（status/ok/headers.get/text/json）、相对 URL 解析、网络错误 reject；无 CORS preflight/streaming/FormData |
| IndexedDB | Not Started | — | — |
| 多线程 | Partial | 7 base 单元测试 + TSan 通过 | `base::ThreadPool`（固定 worker、Post/Submit、WaitIdle、析构排空）、页内多 `<img>` 与外部 `<link rel=stylesheet>` **抓取+解析/解码并行**（FetchFn 需线程安全）；无多进程（Phase 12） |
| 安全（Origin/SOP） | Partial | 8 security 单元测试 + 浏览器集成测试 | `security::Origin`（scheme+host+port 三元组、同源判定、不透明 origin）、标签页记录页面 origin；SOP 实施/CORS/CSP 未开始（Phase 10 后续） |
| GUI（Qt6） | Partial | UI 冒烟测试（offscreen）+ 端到端截图 | 标签页（含 **“+”新建按钮与 Ctrl+T/Ctrl+W/Alt+←/→/F5/Ctrl+L/Ctrl+1..9 快捷键**）/地址栏（**焦点期间周期刷新不再重置文本与光标，退格键编辑正常**）/工具栏/DevTools/历史/书签/下载/设置停靠面板；未做像素级渲染对比 |
| DevTools | Partial | GUI 验证 | DOM 树（选中节点显示**计算样式面板**）/网络日志（含**清空按钮**）/**Cookies 查看**/Console（引擎日志 + JS REPL）；无断点调试 |
| 日志系统 | Tested | 单元测试 | — |
| Error/Result 模型 | Tested | 单元测试 | — |
| CLI 参数解析 | Tested | 单元测试 | — |
| 真实网页渲染 | Tested | 端到端手工验证 | http://example.com/ 与 https://example.com/ 截图 |

更新规则：任何特性状态变化必须同步更新本矩阵与对应模块文档。
