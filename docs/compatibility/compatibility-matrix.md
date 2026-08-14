# 兼容性矩阵

> 本文档诚实记录每个特性的支持状态。**禁止**把"接口存在"写成"已实现"。
> 状态取值：Not Started / Planned / In Progress / Partial / Implemented / Tested。
> 最后更新：2026-08（Phases 0–8 M1，含存储/图像/媒体/PDF/GUI/JS runtime）。

| 特性 | 状态 | 测试证据 | 备注 |
| --- | --- | --- | --- |
| URL 解析 | Tested | 19 单元测试 | RFC 3986 相对解析样例 |
| HTTP/1.1 | Tested | 8 单元测试 | GET、chunked、重定向、Content-Length |
| HTTPS / TLS | Not Started | — | Socket 层已预留 |
| gzip/deflate | Not Started | — | 返回显式 NOT IMPLEMENTED |
| HTML tokenizer | Tested | HTML 套件 | 字符引用子集、RAWTEXT/RCDATA |
| HTML parser | Tested | HTML 套件 | 插入模式子集（无 table 容错）；隐含 p 闭合按 button 作用域判定 |
| DOM | Tested | DOM 套件 | 树操作、querySelector 子集 |
| CSS tokenizer/parser | Tested | CSS 套件 | 规则、声明、!important、@media |
| 选择器匹配 | Tested | CSS 套件 | 属性/伪类/组合器子集 |
| 级联 / 计算样式 | Tested | Style 套件 | 特异性、继承、内联样式 |
| Block layout | Tested | Layout 套件 | 盒模型、堆叠、宽度填充 |
| Inline layout | Tested | Layout 套件 | 文字换行、行盒 |
| inline-block | Partial | Layout + Style + Paint 套件 | 行内原子块盒：显式/shrink-to-fit 宽度、内部块格式化上下文、background/border/padding、vertical-align 与行高参与行盒；精确基线对齐（多行内块按自身最后一行盒基线）未做 |
| Table layout | Partial | Layout 套件 | 行列网格、colspan/rowspan（含 rowspan=0 跨行组末尾、WHATWG 截断）、显式列宽、auto 列分配；无 border-collapse/vertical-align/caption |
| 超链接（`<a>`） | Partial | Browser + Renderer 套件 | 蓝色+下划线样式、命中测试、相对 URL 解析、点击导航；无 fragment 滚动与 :visited/:hover/:active 伪类 |
| Flexbox | Not Started | — | display:flex 按 block 处理 |
| Grid | Not Started | — | display:grid 按 block 处理 |
| float | Partial | Layout + Style 套件 | 行外锚定 block 一侧（left/right）、行盒环绕让位、shrink-to-fit/显式宽高；无 clear、多 float 相交、跨 BFC 布局 |
| position absolute | Partial | Layout 套件 | 包含块判定（最近 positioning 祖先 padding box）、top/left/right/bottom、shrink-to-fit 与 left+right 约束方程；fixed 暂按 absolute 处理，无 z-index/百分比 offset |
| 文本（位图字体回退） | Tested | Paint 套件 | 无系统字体时的 8x8 ASCII 回退 |
| 文本（FreeType） | Partial | Graphics + Paint 套件 | 系统字体、抗锯齿、任意字号、UTF-8、glyph 缓存、布局真实 advance、font-family 匹配、逐字符回退 + CJK 回退链（中文可显示）、粗体/斜体变体匹配；无 HarfBuzz 整形 |
| 图像解码 PNG | Tested | 16 图像单元测试 | 自研解码器（chunk/CRC/滤波/Adam7/全部颜色类型） |
| 图像解码 JPEG | Tested | 16 图像单元测试 | 封装 libjpeg，接口统一为 neko::image |
| 页面内 `<img>` 渲染 | Partial | Renderer + Layout + Paint 套件 | 子资源抓取+解码注入、行内原子盒（与文字同行）、replaced 尺寸（固有/显式/比例、presentational width/height）、object-fit fill/contain/cover/none/scale-down、vertical-align baseline/middle/top/bottom |
| 图像解码 GIF/WebP/AVIF | Not Started | — | 返回显式 NOT IMPLEMENTED |
| WAV 音频解码 | Tested | 13 媒体单元测试 | 自研 RIFF/WAVE，PCM+float，8/16/24/32-bit |
| 视频解码（H.264/VP9 等） | Not Started | — | 显式 NOT IMPLEMENTED，架构预留 |
| PDF 文本提取 | Partial | 12 PDF 单元测试 | xref（含 /Prev）、FlateDecode、内容流文本操作符；无 xref stream/渲染/CMap |
| Cookie（RFC 6265 子集） | Tested | 29 存储单元测试 | Set-Cookie、域/路径匹配、Max-Age、Secure/HttpOnly/SameSite；PSL 与 SameSite 强制标注为限制 |
| 历史记录 | Tested | 29 存储单元测试 | 去重访问、搜索、持久化 |
| 书签 | Tested | 29 存储单元测试 | 增删改、文件夹、持久化 |
| 下载器 | Tested | Browser 套件 | Content-Disposition/URL 文件名、原子写入 |
| 绘制 / 光栅化 | Tested | Paint 套件 | 纯色、边框、文字、PPM |
| 合成器 | Not Started | — | — |
| JavaScript（runtime，QuickJS） | Partial | 27 JS 单元测试 + CLI/GUI 集成 | ES2025 核心语言、console、执行时限/内存上限；**无 DOM 绑定**（里程碑 2） |
| Fetch（浏览器 API） | Not Started | — | — |
| LocalStorage / IndexedDB | Not Started | — | — |
| 多进程 | Not Started | — | Phase 12 |
| GUI（Qt6） | Partial | UI 冒烟测试（offscreen）+ 端到端截图 | 标签页/地址栏/工具栏/DevTools/历史/书签/下载/设置停靠面板；未做像素级渲染对比 |
| DevTools | Partial | GUI 验证 | DOM 树 / 网络日志 / Console；无断点调试 |
| 日志系统 | Tested | 单元测试 | — |
| Error/Result 模型 | Tested | 单元测试 | — |
| CLI 参数解析 | Tested | 单元测试 | — |
| 真实网页渲染 | Tested | 端到端手工验证 | http://example.com/ 截图 |

更新规则：任何特性状态变化必须同步更新本矩阵与对应模块文档。
