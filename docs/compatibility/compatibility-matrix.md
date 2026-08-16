# 兼容性矩阵

> 本文档诚实记录每个特性的支持状态。**禁止**把"接口存在"写成"已实现"。
> 状态取值：Not Started / Planned / In Progress / Partial / Implemented / Tested。
> 最后更新：2026-08（WHATWG 字符编码、并行带栅格化、显示列表缓存、滚动 blit 视口缓存、
> 字体缓存线程安全、`<style>` 解析缓存、多字节编码中文站点解码、GIF 动画、
> Grid minmax()/命名线/命名区域/auto-flow/inline-grid）。

| 特性 | 状态 | 测试证据 | 备注 |
| --- | --- | --- | --- |
| URL 解析 | Tested | 19 单元测试 | RFC 3986 相对解析样例 |
| HTTP/1.1 | Tested | 8 单元测试 | GET、chunked、重定向、Content-Length；**增量读取**（按 framing 精确读取响应体，而非读到关闭），Content-Length 截断校验（防截断攻击的完整性兜底） |
| HTTPS / TLS | Tested | 5 单元测试（本地 TLS 服务器 + 自签名 CA） | OpenSSL 封装（ADR 0010），证书+主机名校验、SNI、TLS≥1.2；gzip/deflate 协商；**兼容 CDN 无 close_notify 关闭**（sohu/bing 实测，完整性由 HTTP 层 Content-Length 校验兜底） |
| gzip/deflate | Tested | 12 单元测试（含链式编码、raw deflate、服务器往返） | RFC 7231 内容编码解码，64 MiB 输出上限 |
| HTML tokenizer | Tested | HTML 套件 | 完整 WHATWG 命名字符引用表（2125 项，含双码点，生成代码）+ RAWTEXT(style/xmp/iframe/noembed)/RCDATA(title/textarea)/PLAINTEXT/script data；CRLF 归一化、EOF-in-tag 丢弃、属性上下文实体 `=`/alnum 字面规则、DOCTYPE public/system identifier 状态机 |
| HTML parser | Tested | HTML 套件 | 插入模式：initial/before html/before head/in head/after head/in body/text/**in table/in table text/in caption/in column group/in table body/in row/in cell**；**表格容错（foster parenting）**、活动格式化元素 marker、hr/center 关闭 p、dd/dt 互闭、after head 元素进 head；隐含 p 闭合按 button 作用域判定 |
| DOM | Tested | DOM 套件 | 树操作、querySelector 子集；CharacterData `data`/`nodeValue` getter/setter、Comment.textContent 返回数据、DocumentFragment 插入搬移子节点、树变更抛 **DOMException**（HierarchyRequestError/NotFoundError） |
| CSS tokenizer/parser | Tested | CSS 套件 + 解析器健壮性回归 | 规则、声明、!important、@media；**分号/花括号/`@font-face` 等声明块不再导致死循环**（曾致 15 GB 内存暴涨）；外部 `<link rel=stylesheet>` 抓取+解析+应用（并行） |
| 选择器匹配 | Tested | CSS 套件 | 属性/伪类(:first-child/:last-child/:nth-child/:root)/组合器子集 |
| 级联 / 计算样式 | Tested | Style 套件 | 特异性、继承、内联样式；**规则按最右复合选择器分桶**（id/class/tag/universal），实测 6004 元素页面级联 0.688s→0.144s（约 5×） |
| CSS 自定义属性 | Partial | 4 Style 单元测试 | `--name` 定义 + `var()` 引用（含 fallback）、默认继承、var() 无法解析时声明无效；无嵌套 var()/同元素链式引用 |
| 逻辑属性 | Partial | 3 Style 单元测试 | inline/block-size、margin/padding-inline/block（1–2 值）、-start/end 长手属性、border-block-start/end、place-items→align-items；无 inline 轴 justify |
| CSS 数学函数 | Partial | 5 Style + 4 Layout 单元测试 | `calc()`（+/- 线性组合）、`min()`/`max()`/`clamp()`（可嵌套 calc 与 var()）、vw/vh/vmin/vmax 单位，布局时对包含块求值；无 calc * /、嵌套 min/max |
| aspect-ratio | Partial | 2 Style + 2 Layout 单元测试 | `1`/`16/9` 形式；宽度确定+高度 auto 时推导高度；显式高度优先；双 auto 忽略 |
| border-radius | Partial | 1 Style 单元测试 | 单一圆角（长度/百分比），背景按圆角绘制；无多值/椭圆/圆角边框 |
| Block layout | Tested | Layout 套件 | 盒模型、堆叠、宽度填充；**box-sizing: border-box 全局生效**（现代站点普遍 `*{box-sizing:border-box}`，修复了真实站点布局溢出/塌陷）；block 容器内 inline 内容先行、block 级子元素（嵌套列表/表格）随后垂直排布 |
| Lists | Partial | Layout + HTML 套件 | `li { display: list-item }`；marker（`ul`→disc、嵌套 `ul`→circle→square、`ol`→decimal、alpha/roman）作为首行文本 run 绘制在内容左侧 gutter，`ol` 按序编号；`<li>` 闭合按 WHATWG（遇嵌套 `<ul>/<ol>` 不关闭外层 li）；`<dl>/<dt>/<dd>`：dd 缩进 40px、dl 块级边距、多 dt/dd 与 div 包装组；无 `list-style-position: inside`/`::marker` |
| box-sizing | Implemented | 6 布局 + 3 样式单元测试 | content-box（初始）/ border-box：width/height、min/max、flex 项、absolute、表格、grid 尺寸均按 border-box 解析 |
| white-space | Partial | 2 布局 + 3 样式单元测试 | normal（初始）/nowrap（整段视为不可断行、溢出不换行）/pre/pre-wrap/pre-line（解析但按 normal 处理——无预格式化文本）；继承 |
| overflow | Partial | 2 绘制 + 3 样式单元测试 | visible（初始）/hidden/auto/scroll；hidden 与 scroll 按 padding box 裁剪（绘制级裁剪栈，嵌套交集），auto/scroll 无滚动溢出交互；`overflow-x/y` 简写未做 |
| Inline layout | Tested | Layout 套件 | 文字换行、行盒 |
| inline-block | Partial | Layout + Style + Paint 套件 | 行内原子块盒：显式/shrink-to-fit 宽度、内部块格式化上下文、background/border/padding、vertical-align 与行高参与行盒；精确基线对齐（多行内块按自身最后一行盒基线）未做 |
| Flexbox | Partial | 25 布局单元测试 + 9 样式解析测试 + 端到端截图 | display:flex/inline-flex、flex-direction row/column(+reverse)、flex-wrap、flex-grow/shrink/basis（含 flex 简写）、justify-content（6 值）、align-items（含 baseline）、align-content（确定 cross 尺寸时）、gap、order、align-self、min/max-width/height、auto margin（主轴/交叉轴）；无 flex-basis 百分比精确高度、flex 容器自身 min/max、grow 后剩余空间再分配（max-width 截断不回流） |
| Table layout | Partial | Layout 套件 | 行列网格、colspan/rowspan（含 rowspan=0 跨行组末尾、WHATWG 截断）、显式列宽、auto 列分配；无 border-collapse/vertical-align/caption |
| 超链接（`<a>`） | Partial | Browser + Renderer 套件 | 蓝色+下划线样式、命中测试、相对 URL 解析、点击导航；无 fragment 滚动与 :visited/:hover/:active 伪类 |
| Grid | Partial | 19 布局单元测试 + 14 样式解析测试 | display:grid/**inline-grid**、grid-template-columns/rows（px/%/fr/auto/min-content/max-content + **repeat()** + **minmax(min,max)**）、**命名线** `[name] ...`（含 repeat 内）、**grid-template-areas**（含隐式 `name-start`/`name-end` 线）与 **grid-area** 简写、grid-column/row 行/span/**命名线/命名区域**放置（**负数行**从尾计数）、**grid-auto-flow row/column × sparse/dense**、column/row gap；无 grid-template/grid 简写、auto-fill/auto-fit、fit-content()、spanning 项跨轨分配（按起始轨简化）、justify/align-self |
| float | Partial | Layout + Style 套件 | 行外锚定 block 一侧（left/right）、行盒环绕让位、shrink-to-fit/显式宽高；无 clear、多 float 相交、跨 BFC 布局 |
| `appearance` / `<button>` 原生外观 | Partial | Style + Paint + Renderer 套件 | appearance none/auto/button（CSS-UI-4 §7.2）：button UA 默认 inline-block + 文本居中 + buttonface 背景与 outset 边框（作者 background/border 优先，与浏览器一致），button 可强制任意元素；无 hover/active/disabled 状态、box-sizing:border-box、min 尺寸、内容垂直居中 |
| 表单控件（`<input>`/`<textarea>`/`<select>`） | Partial | 3 Browser 集成 + 2 UI 端到端 + Layout/Paint 验证 | 原子行内盒渲染（默认 1px 边框 + 白底 + value/placeholder 文本 run，text/textarea/select 内容、默认 170px 宽与行高）；点击聚焦（元素级焦点，后续键盘输入可达）；键盘输入默认行为：可打印字符追加、Backspace 删除、Enter 隐含提交表单；GUI 点击窃取键盘焦点（Qt setFocus）；聚焦文本控件绘制**闪烁文本光标（caret）**（500ms QTimer，置于 value 文本末尾，焦点离开/导航即消失，UI 测试逐帧抓图验证闪烁）；无 focus outline、textarea/select 就地编辑、IME、剪贴板、maxlength |
| position absolute | Partial | Layout 套件 | 包含块判定（最近 positioning 祖先 padding box）、top/left/right/bottom、shrink-to-fit 与 left+right 约束方程；fixed 暂按 absolute 处理，无 z-index/百分比 offset |
| 文本（位图字体回退） | Tested | Paint 套件 | 无系统字体时的 8x8 ASCII 回退 |
| 文本（FreeType） | Partial | Graphics + Paint 套件 | 系统字体、抗锯齿、任意字号、UTF-8、glyph 缓存、布局真实 advance、font-family 匹配、逐字符回退 + CJK 回退链（中文可显示）、粗体/斜体变体匹配；**glyph/字体选择器/字形缓存均线程安全（互斥锁，支持并行栅格化）**、**TextWidth 记忆化**（同 (text,px) 命中缓存）；无 HarfBuzz 整形 |
| 字符编码（HTML/文本） | Tested | 21 编码单元测试（含全 GBK 文档往返） | **WHATWG Encoding 标准**：UTF-8（含截断序列边界）、UTF-16BE/LE、gb18030/GBK（2 字节 + 4 字节码点范围）、Big5、Shift_JIS（含 EUDC 私有区）、EUC-JP、EUC-KR、ISO-2022-JP、windows-125x/iso-8859-x/koi8-r/koi8-u/macintosh/ibm866/x-mac-cyrillic 等 28 种单字节表、x-user-defined、replacement；**HTML 字符集预扫描**（meta charset/http-equiv、UTF-16 签名、`<?xml`、注释）、**BOM 嗅探覆盖一切**、HTTP `Content-Type` 提示优先级高于预扫描；编码表由 `tools/gen_encoding_tables.py` 从 WHATWG 官方索引生成（离线提交，构建无需网络）；页面加载后统一转码为 UTF-8 再进解析器 |
| 图像解码 PNG | Tested | 16 图像单元测试 | 自研解码器（chunk/CRC/滤波/Adam7/全部颜色类型） |
| 图像解码 JPEG | Tested | 16 图像单元测试 | 封装 libjpeg，接口统一为 neko::image |
| 图像解码 GIF | Tested | 17 图像单元测试（含测试内 LZW 编码器）+ 2 渲染器动画测试 | 自研解码器（GIF87a/89a、全局/局部色表、LZW 变长码宽、交错、GCE 透明/disposal/延迟、NETSCAPE2.0/ANIMEXTS1.0 循环次数）；**动画**：全帧预合成（disposal 0-3，4 归一化为 3）、≤2cs 延迟按浏览器惯例钳制为 10cs（100ms）、无循环扩展时默认无限循环（同 Blink/Gecko）；GUI 50ms 帧时钟驱动页面内 `<img>`/背景图动画（帧推进原地更新像素并失效显示列表）；解码内存有界（画布 128 MiB、帧合计 64 MiB/2048 帧封顶，超预算截断）；直接导航到 .gif 仍显示首帧 |
| 图像解码 SVG | Partial | 6 图像单元测试 + 端到端截图 | 自研最小栅格化器：svg/g/a/rect(含圆角)/circle/ellipse/line/polyline/polygon/path（M/L/H/V/C/S/Q/T/A/Z + 相对）、fill/stroke/stroke-width/透明度、transform（translate/scale/rotate/matrix）、viewBox meet 居中、2× 超采样抗锯齿；无 <text>/渐变/图案/滤镜/use/clip-path 蒙版 |
| 图像解码 WebP | Implemented | 3 WebP 单元测试（无损 VP8L 色块、magic 检测、拒绝坏 magic） | libwebp 封装 |
| 图像解码 AVIF | Implemented | 4 AVIF 单元测试（真实 AV1 无损夹具、magic 检测、分发） | libavif 封装（ISO-BMFF ftypavif/avis 检测，8 位 RGBA，画布 128 MiB 上限）；动画 AVIF（avis）仅首帧 |
| 页面内 `<img>` 渲染 | Partial | Renderer + Layout + Paint 套件 | 子资源抓取+解码注入、行内原子盒（与文字同行）、replaced 尺寸（固有/显式/比例、presentational width/height）、object-fit fill/contain/cover/none/scale-down、vertical-align baseline/middle/top/bottom |
| WAV 音频解码 | Tested | 13 媒体单元测试 | 自研 RIFF/WAVE，PCM+float，8/16/24/32-bit |
| 视频解码（H.264/VP9 等） | Partial | 5 视频单元测试 | FFmpeg（ADR 0014）解复用/解码/像素转换，封装在 MediaSource/DecodeVideo 后（MP4/WebM 实测）；音频轨道忽略；`<video>` 元素播放尚未接入 |
| PDF 文本提取 | Partial | 13 文本提取测试 | xref（含 /Prev）与 xref stream、FlateDecode、内容流文本操作符；无 CMap/加密 |
| PDF 页面渲染 | Partial | 8 渲染测试（含 xref stream/ObjStm） | 矢量路径（非零/奇偶填充、描边）、q-Q/cm、文本（FreeType + /Widths）、/ObjStm、/MediaBox 继承与实数值；无图像 XObject/裁剪/pattern/CMap |
| Cookie（RFC 6265 子集） | Tested | 29 存储单元测试 | Set-Cookie、域/路径匹配、Max-Age、Secure/HttpOnly/SameSite；PSL 与 SameSite 强制标注为限制 |
| LocalStorage | Tested | 6 存储单元测试 + 浏览器生命周期接线 | 按 origin 分区的键值存储，行式文件 + 百分号编码 + 原子写入，已接入 Profile Load/Save/ClearAll；无配额、无 storage 事件、未接 JS（Phase 8 M2） |
| 历史记录 | Tested | 29 存储单元测试 | 去重访问、搜索、持久化 |
| 书签 | Tested | 29 存储单元测试 | 增删改、文件夹、持久化 |
| 下载器 | Tested | Browser 套件 | Content-Disposition/URL 文件名、原子写入 |
| 绘制 / 光栅化 | Tested | Paint 套件 | 纯色、边框、文字、PPM；**整数定点混合（替代浮点）**、**缓冲复用（Resize 不重分配）**、**分带 Clear/可见带裁剪**、**并行带栅格化**（`RasterizeParallel`，共享线程池，串/并行结果一致）、**滚动 blit**（`ShiftRows` 内存搬移复用上一帧像素，仅重绘露出带） |
| 渲染管线缓存 | Tested | Renderer + UI 套件 | **显示列表缓存**（Painter 输出按版本号增量重建，仅在 DOM/样式变化时失效）、**WebView 视口光栅缓存**（滚动时 blit 复用，仅补绘露出带）；布局/绘制不再全量重做 |
| 合成器 | Not Started | — | — |
| JavaScript（runtime，QuickJS） | Partial | 107 JS 单元测试 + 浏览器集成测试 + CLI/GUI 集成 + bing 实测 | ES2025 核心语言、console、执行时限/内存上限、**DOM 绑定**（document/Node/Element/Text/Comment/DocumentFragment/CSSStyleDeclaration/**Event**/**CustomEvent**）、**页内 `<script>` 执行（内联 + 外部 src=、async/defer）**、**最小事件循环**（setTimeout/setInterval 同步泵、**requestAnimationFrame**）、**事件传播**（capture→target→bubble、preventDefault/stopPropagation/stopImmediatePropagation、once/capture 选项、composedPath）、**microtask 泵送**、**localStorage/fetch（Phase 8 M3 子集）**、**window.location 导航**、**classList/dataset/matches/closest**、**表单控件 value/checked/type/placeholder/disabled/name**、**a.href/img.src 绝对化与 img.naturalWidth/Height/complete**、**getComputedStyle**（浏览器层接线 StyleEngine）、**history/performance.now + performance.timing.navigationStart/timeOrigin**、**window === globalThis**（`window._G={...}` 与裸 `_G` 读写互通，bing 47 脚本链的根因修复）、**window.self/top/parent/frames**、**innerText（读，textContent 近似）**、**matchMedia**（基础媒体查询求值，固定 800×600 视口）、**全局事件处理属性**（onload/onerror/...，可裸读可赋值）；无完整 Web IDL、无 WebSocket/XHR/sessionStorage/FormData、module 脚本不执行 |
| Fetch（浏览器 API） | Partial | JS 单元测试 + 浏览器集成测试 | window.fetch Promise<Response>（status/ok/headers.get/text/json）、相对 URL 解析、网络错误 reject；无 CORS preflight/streaming/FormData |
| DOM 元素几何 API | Partial | 1 JS 回调测试 + 1 浏览器集成测试 + Renderer 几何查询 | **getBoundingClientRect** 返回真实布局矩形（border box，文档坐标，含 toJSON）、**offsetWidth/Height**（border box 尺寸）、**offsetLeft/Top**（文档坐标）、**offsetParent**（恒为 body）、**clientWidth/Height**（padding box）、**clientTop/Left**（border 宽）；块级/原子元素读自身布局盒，纯 inline 元素聚合其文本 run 的并集矩形；布局缺失时按需构建（脚本先于 UI 布局）；无滚动感知（scrollWidth/Height/scrollTop 未建模、getBoundingClientRect 未减滚动偏移）、offsetParent 恒为 body（positioned 祖先/表格单元格未建模） |
| 用户交互事件（阶段 2） | Partial | 8 JS 事件测试 + 5 浏览器集成 + 4 UI 端到端 | 浏览器派发 **MouseEvent**（clientX/clientY/button，mousedown→mouseup→click）、**mouseover/mouseout**（悬停元素变化时经 Qt MouseMove 接线派发，worker 端命中）、**KeyboardEvent keyCode**、**focus/blur**（点击控件聚焦/失焦触发）、**input**（输入后 bubbling 触发 oninput/listener）、**wheel**（滚动 deltaY，Qt wheelEvent 接线）；**元素全局事件处理属性**：IDL 赋值（element.onclick=fn）与 content attribute（on*="code" 编译执行）都在事件到达元素时触发，click 的 preventDefault 仍控制导航/提交默认行为；**事件 handler 改 DOM → 立即重算+重绘**（DomBinder 脏检测 + ReapplyStyles 立即重建布局，页面脚本交互及时反映）；点击在导航加载中不丢失（worker 端实时命中）；无 mousemove（高频未接线）、mouseenter/mouseleave、dblclick、wheel 的 preventDefault 不阻止默认滚动、on* content attribute 每次触发重新编译（无缓存） |
| IndexedDB | Partial | 12 核心 + 10 绑定 + 1 集成测试 | 版本化数据库/open+onupgradeneeded、对象存储（keyPath/autoIncrement）、add/put/get/delete/clear/count/getAll（事务 + IDBRequest 微任务回调 + oncomplete）、JSON 结构化克隆子集、按 origin 持久化（indexed_db.txt 原子写入）；无游标/索引/范围、无 Date/BinaryData/循环克隆 |
| 多线程 | Partial | 7 base 单元测试 + TSan 通过 | `base::ThreadPool`（固定 worker、Post/Submit、WaitIdle、析构排空）、页内多 `<img>` 与外部 `<link rel=stylesheet>` **抓取+解析/解码并行**（FetchFn 需线程安全）、**并行带栅格化**（大页面渲染按水平带并行）、**共享线程池**（浏览器控制器持有，样式/图像抓取与渲染复用同一池）；无多进程（Phase 12） |
| 样式解析缓存 | Tested | Style 套件 | `<style>` 元素按文本内容记忆化解析（内容未变不重解析，元素删除时清理）；选择器分桶 + 计算样式缓存配合下，重复样式应用不再重解析 |
| 安全（Origin/SOP） | Partial | 8 security 单元测试 + 浏览器集成测试 | `security::Origin`（scheme+host+port 三元组、同源判定、不透明 origin）、标签页记录页面 origin；SOP 实施/CORS/CSP 未开始（Phase 10 后续） |
| GUI（Qt6） | Partial | UI 冒烟测试（offscreen）+ 端到端截图 | 标签页（含 **“+”新建按钮与 Ctrl+T/Ctrl+W/Alt+←/→/F5/Ctrl+L/Ctrl+1..9 快捷键**）/地址栏（**焦点期间周期刷新不再重置文本与光标，退格键编辑正常**）/工具栏/DevTools/历史/书签/下载/设置停靠面板；未做像素级渲染对比 |
| DevTools | Partial | GUI 验证 | DOM 树（选中节点显示**计算样式面板**）/网络日志（含**清空按钮**）/**Cookies 查看**/Console（引擎日志 + JS REPL）；无断点调试 |
| 日志系统 | Tested | 单元测试 | — |
| Error/Result 模型 | Tested | 单元测试 | — |
| CLI 参数解析 | Tested | 单元测试 | — |
| 真实网页渲染 | Tested | 端到端手工验证 | http://example.com/ 与 https://example.com/ 截图 |

更新规则：任何特性状态变化必须同步更新本矩阵与对应模块文档。
