# JavaScript 模块

> 状态：**Partial**（Phase 8 里程碑 1 + 里程碑 2 子集：runtime + DOM 绑定 +
> 页面脚本执行 + 最小事件循环）

## 现状

### 里程碑 1：运行时

- **运行时**：QuickJS（quickjs-ng v0.16.1，MIT），经 FetchContent 固定版本构建。
  封装在 `neko::javascript` 自有接口之后（`ScriptEngine` / `ScriptValue`），
  第三方头文件不泄漏到公共 API（见 ADR 0008）。
- **能力**：ES2025 核心语言、`ScriptEngine::Evaluate/CallGlobal/GetGlobal/
  SetGlobal`、值转换（ToString/ToNumber/ToBoolean/JsonStringify）、
  项目自有 `console` 绑定、执行时限中断、内存上限。
- **集成**：CLI `--eval`；GUI DevTools Console 持久 REPL。

### 里程碑 2（子集）：DOM 绑定 + 页面脚本 + 事件循环

`DomBinder`（`neko/javascript/dom_binding.h`）把一个 `dom::Document` 绑定进
一个专用 ScriptEngine（每页一个 runtime），并注册以下面：

- **全局对象**：`document`、`window`、`setTimeout`、`clearTimeout`、
  `setInterval`、`clearInterval`、`addEventListener`/`removeEventListener`/
  `dispatchEvent`（window 别名，转发到 document）、`navigator`、`screen`、
  `matchMedia`，以及 DOM 接口构造器
  `Node`/`Element`/`HTMLElement`/`Document`/`Text`/`Comment`/
  `DocumentFragment`/`CSSStyleDeclaration`/`Event`/`CustomEvent`
  （其 `.prototype` 指向真实 wrapper 原型，因此 `x instanceof Element` 与
  `Element.prototype.foo = ...` 扩展都可用；直接 `new` 会抛 "Illegal
  constructor"）。
- **window === globalThis**：window 就是全局对象（浏览器语义），因此
  `window._G = {...}` 落在全局作用域，下一个 `<script>` 可用裸 `_G` 读取，
  反之亦然。这是 bing 那串 ~47 个脚本得以依次执行的前提：早期脚本定义全局、
  后期脚本消费。**window.self/top/parent/frames 均 === window**（引擎无帧树）。
  另外全局事件处理属性（HTML §8.1.7.2 的 `onload`/`onerror`/`onclick`/…）
  以可写 null 槽暴露为全局属性，脚本可裸读/裸赋值（事件系统暂不自动触发它们）。
- **window**：`navigator`、`screen`、`innerWidth`/`innerHeight`/
  `devicePixelRatio`（引擎默认视口 800×600@1x，与 `renderer::Page` 默认布局
  宽度一致；真实窗口尺寸的接入是后续工作）、`matchMedia`。
- **navigator**：`userAgent`（与网络栈发送的 UA 一致）、`platform`
  （按 OS 宏）、`language`/`languages`（默认 "en-US"）、`onLine`、
  `cookieEnabled`、`hardwareConcurrency`、`vendor`、`mimeTypes`（稳定的空
  legacy collection，支持 `length`/`item()`/`namedItem()`；尚未实现 MIME
  类型发现）。缺失的接口（如 geolocation/clipboard）不提供，
  `"x" in navigator` 诚实地返回 false。
- **screen**：`width`/`height`/`availWidth`/`availHeight`（800×600）、
  `colorDepth`/`pixelDepth`（24）。
- **Document**：`documentElement`、`body`、`head`、`readyState`（恒为
  `"complete"`，脚本在解析完成后运行）、`title`（读写）、`getElementById`、
  `querySelector(All)`、`createElement`、`createTextNode`、
  `currentScript`（只读：返回正在执行脚本体的 `<script>` 元素，非脚本执行
  期间为 `null`；`browser::RunPageScripts` 在每个脚本体执行前后设置/清除，
  见下）、`domain`（getter 返回页面 host；setter 只允许**缩短**到父域——
  按标签边界判定，非父域抛 `SecurityError`（带 `name`），空串恢复真实
  host；真实站点用它做跨子域通信的判定，bilibili 的域名探测代码依赖它
  可读；值本身尚未参与同源判定，也没有 PSL 守卫，见“未实现”）。
- **Node**：`nodeType`、`nodeName`、`textContent`（读写）、`parentNode`、
  `firstChild`、`lastChild`、`childNodes`、`appendChild`、`append`、
  `replaceChildren`、`insertBefore`、`removeChild`、`hasChildNodes`、
  `cloneNode`、`addEventListener`、`removeEventListener`、`dispatchEvent`。
  （`append`/`replaceChildren` 仅接受节点参数；规范中字符串参数会转换为
  文本节点，此处为文档化限制。）
- **Element**：`tagName`、`id`/`className`（读写）、`attributes`、
  `getAttribute/setAttribute/removeAttribute/hasAttribute`、`children`、
  `firstElementChild`、`querySelector(All)`、`getElementsByTagName`、
  `getElementsByClassName`、`innerHTML`（读写）、`innerText`（读；
  **textContent 近似**——真实 innerText 是渲染文本：隐藏元素剔除、
  空白归一化，本引擎暂未实现布局相关 innerText）、
  `style`（CSSStyleDeclaration）。
- **CSSStyleDeclaration**：`setProperty/getPropertyValue/removeProperty` 以及
  一组直接访问器（width/height/color/background-color/font-size/...），
  均以 style 属性为数据源（读写会改写 style 属性）。
- **事件循环**：`setTimeout/setInterval/clearTimeout/clearInterval` +
  `DomBinder::RunPendingTimers()`（到期即执行，重复定时器按间隔累加避免漂移）；
  `addEventListener` 注册的监听器可通过 `dispatchEvent`（JS 侧，元素或
  document）或 `DomBinder::DispatchEvent` / `DomBinder::DispatchDocumentEvent`
  （C++ 侧）同步派发。window 与 document 共享同一事件目标集：window 级
  监听器存储在 document 节点下。

**页面 Web API（Phase 8 M3 子集）**：`browser::RunPageScripts` 通过
`PageScriptServices` 接线后，页面脚本可使用：

- `window.localStorage`（按页面 origin 分区）：`getItem`/`setItem`/
  `removeItem`/`clear`/`key(i)`/`length`。数据由 C++ `storage::LocalStorage`
  持久化到 profile（跨导航保留）；对象使用全局不可直接构造的 `Storage`
  接口及其 prototype。无 sessionStorage/storage 事件。
- `window.fetch(url)`：返回 Promise，解析为最小 Response 对象
  （`status`/`ok`/`statusText`/`url`/`headers.get(name)`/`text()`/`json()`）；
  相对 URL 按页面 base 解析；网络错误 reject。同步网络调用立即 resolve，
  由 microtask 泵送推进 `await`/`.then` 链。无 Request/AbortController/
  FormData。
- `window.indexedDB`：`open(name[, version])`/`deleteDatabase(name)`（返回带
  `onsuccess`/`onerror`/`onupgradeneeded` 与 `result`/`error`/`readyState`
  的 IDBRequest 风格对象，回调经 microtask 派发）；`IDBDatabase` 提供
  `createObjectStore`/`deleteObjectStore`（仅升级事务内）/`transaction`/
  `objectStoreNames`；`IDBTransaction` 提供 `objectStore`/`oncomplete`/
  `onabort`/`abort`（只读事务写入同步抛 `ReadOnlyError`，auto-commit）； 
  `IDBObjectStore` 提供 `add`/`put`/`get`/`delete`/`clear`/`count`/`getAll`
  （keyPath/autoIncrement 支持，键为 number|string，值走 JSON 结构化克隆
  子集，重复 add 报 `ConstraintError`）。数据由 C++ `storage::IndexedDbStore`
  按 origin 持久化到 `indexed_db.txt`（跨导航保留）。无游标/索引/范围、
  无 Date/BinaryData/循环克隆；错误对象带 DOMException 风格 `.name`。
- `window.matchMedia(query)`：按引擎固定视口（800×600）对常见媒体查询
  求值（`(min|max)-(width|height): Npx`、`orientation`、`prefers-color-scheme`、
  `prefers-reduced-motion`、`(any-)pointer`/`hover`），逗号列表按 OR 求值；
  返回的 MediaQueryList 是静态的（`addEventListener`/`removeListener` 等
  为 no-op），未知特性保守返回 `matches:false`。
- **HTMLMediaElement（`<video>` 子集）**：`play()`/`pause()`（非媒体元素
  上调用抛 TypeError）、`currentTime`（读写，秒）、`duration`/`paused`
  （只读，无媒体时 duration/currentTime 为 NaN、paused 为 true）。状态由
  页面的视频帧时钟驱动（与 GIF/定时器同泵），对应
  `Page::PlayVideo/PauseVideo/SeekVideo/VideoDuration/VideoCurrentTime`。
  无 `controls`/音轨/缓冲（buffered/readyState 未实现）。
- **Canvas 2D（最小真实子集）**：`<canvas>` 使用独立
  `HTMLCanvasElement.prototype`；`getContext("2d")` 对同一元素返回稳定的
  `CanvasRenderingContext2D`，未知 context 类型与 WebGL 返回 `null`。context
  支持 `fillStyle`（CSS 颜色）与 `fillRect(x, y, width, height)`。首次绘制建立
  300×150 透明 RGBA backing store，矩形按画布边界裁剪并执行 source-over
  合成；canvas 作为 replaced element 进入 layout、`DrawImage` 和最终 raster，
  不是只为脚本消错的空对象。JS 绑定测试覆盖 context/颜色/绘制回调，renderer
  像素测试覆盖默认尺寸、透明背景、裁剪及半透明 source-over。CCTV13 直播页
  实测生成 71×71 二维码 canvas，`jquery.qrcode.min.js` 的同步脚本错误由 1 降为
  0。当前无 width/height 属性变化时重建 backing store、clearRect、路径、文字、
  变换、渐变、drawImage、getImageData/putImageData、导出及 WebGL。
- `window.performance`：`now()`、`timeOrigin`、`timing.navigationStart`
  （均为页面加载起点）。bing 的启动脚本读取 `performance.timing.
  navigationStart`。
- `new CustomEvent(type, {detail, bubbles, cancelable})`：Event 的子类构造器，
  `detail` 默认 null；`CustomEvent.prototype` 继承 `Event.prototype`。
  脚本间用 CustomEvent 携带数据派发事件。

**Promise / 微任务**：`ScriptEngine::Evaluate`/`CallGlobal` 在求值后泵送
QuickJS 的 job 队列（promise 的 `.then` 延续、async 函数），因此顶层启动的
`async` 函数能推进到完成；定时器回调和事件派发后同样泵送。未处理的
rejection 通过 `JS_SetHostPromiseRejectionTracker` 记录，在微任务检查点
（job 队列清空后）报告为 `Uncaught (in promise) ...`，与浏览器的
unhandledrejection 时序一致（同一轮内被 `.catch` 的不报告）。执行时限通过
中断处理器同样适用于 job 泵送。

**页面集成**：`browser::RunPageScripts`（`neko/browser/page_scripts.h`）在
HTML 解析后按文档顺序执行页内 `<script>`，全部脚本执行完后向 document
派发 `DOMContentLoaded` 与 `load`（近似：真实浏览器在子资源全部加载后才
触发 `load`，同步引擎没有该信号），随后重跑样式级联（脚本可改 DOM）。
`BrowserController::LoadBytes` 在发布页面之前调用它，并把运行时句柄存到
Tab（`PumpScriptTimers` 供工作者线程推进定时器）；GUI 用 50ms QTimer 驱动；
CLI `--url` 路径同样执行脚本。

**外部脚本与 async/defer**（WHATWG HTML §4.12.1 的 classic 模型）：

- classic（无 async/defer）：按文档序抓取并同步执行，阻塞后续脚本；
- `defer`：在全部 classic 脚本之后按文档序执行（解析已完成，与规范一致）；
- `async`：在 classic+defer 阶段之后按文档序执行 —— 同步引擎的文档化近似
  （不抢占管线，无法先于更早的 classic 运行）。
- 外部脚本经同一网络栈（生产带 Cookie）抓取；module 与动态 import 不支持。

**测试**：107 个 JS 单元测试 + 浏览器集成测试（脚本执行/console/错误/
定时器/外部脚本/文档序/defer/async/失败不中断/生命周期事件/promise 泵送/
未处理 rejection/localStorage/fetch/多图并行解码/页面 origin/`window ===
globalThis` 全局互通/事件处理属性/CustomEvent/innerText/matchMedia/
performance.timing）。ASan 无泄漏、TSan 无数据竞争。

## 生命周期 / GC 注意点

`DomBinder` 把 `window` 建成全局对象本身，因此页面在 `window`/全局上创建的
任意属性（如 bing 的 `window._G`、事件对象的 `window._ev`）都直接挂在全局
对象上，随页面 runtime 一起被 GC 回收。`Event` 类的 `gc_mark` 回调标记其
opaque 中持有的 `target`/`current_target`，否则一个"活"事件的 target（如
document wrapper）只靠 GC 看不到的引用计数存活，runtime 销毁时会触发
`JS_FreeRuntime` 的 `gc_obj_list is empty` 断言。`~Impl` 显式删除它安装到
全局对象的每个属性（含 window/self/top/parent/frames 自引用）以保持 teardown
确定性。

## bing.com 实测（阶段 1：脚本链打通）

`window === globalThis` 修复后，bing 首页的脚本链不再从首个 `window._w` 处
断开（此后 `_G`、`EventsToDuplicate`、`sj_evt`、`Feedback` 相继可用），搜索框
`<textarea id="sb_form_q">` 正常渲染。残余报错与真实浏览器控制台一致，均为
bing 自身问题：

- `_w is not defined`：bing 脚本 1 在脚本 31 定义 `_w` **之前**就执行
  `_w.sj_pt=sj_pt`。任何符合规范的浏览器都会在此抛 ReferenceError（bing
  控制台常年有该报错），且不致命（后续脚本继续执行并定义 `_w`）。
- `Feedback is not defined` / `.controller` / `.trigger` / `.match` of
  undefined：均来自 bing 的动态模块加载器（`_w.rms.js`）与遥测脚本，依赖其
  异步模块时序；同步引擎不仿真该时序，故这些辅助脚本报错但不阻塞主渲染。
- 搜索框本身是服务端渲染进 HTML 的（非纯 JS 现拼），脚本链负责的是建议、
  IOTD、登录态、反馈等增强功能。

**历史注记（2026-10 撤销）**：曾为消除上述 ReferenceError 在引擎里注入过
`window.jQuery`/`$`/`_w`/`_d`/`Feedback`/`BM`/`Log` 的假对象。该做法被
bilibili 实测证伪 —— bundle 检测到 `window.jQuery` 为真值后会写
`jQuery.fn.lazyload`，在假对象上直接崩溃（“cannot set property 'lazyload'
of undefined”）。**伪造全局已全部移除**：浏览器不定义这些名字，feature
detection 必须得到真实答案，页面自行降级或加载真正的库。详见
`docs/compatibility/compatibility-matrix.md` 的 “Web IDL 一致性” 行。

## bilibili.com 实测（2026-10：真实站点驱动的引擎修复）

www.bilibili.com 首页（Vue 3 SSR + hydration + 大量异步 chunk）作为真实站点
靶标，三种执行模式（进程内 / `--network-process` / `--renderer-process`）
加载后 **JS 错误为零**（修复前 46 个），推荐流卡片 394 张。修复链条（各带
回归测试）：

1. **伪造 `window.jQuery`/`$` 全局**（见上节）导致 bundle 在 `jQuery.fn`
   写入处崩溃 → 全部移除。
2. **Proxy 接收者**：Vue reactivity 以 `Reflect.get(target, key, receiver)`
   读取 DOM 属性，接收者是 Proxy。Web IDL 要求 Proxy（目标为实现接口的平台
   对象）透明工作；引擎此前对一切 Proxy 接收者抛 “detached node”，现
   `ResolveProxyReceiver` 在 `UnwrapNode`/`ImplFor` 及其全部调用点统一解包。
3. **Annex B 遗留 RegExp 静态属性**：`RegExp.$1` 等此前为 undefined，而
   bilibili 的日期格式化在 `test()` 后读 `RegExp.$1.length`。引擎包装
   `RegExp.prototype.exec`（test/match/replace/split 均经 `exec` 属性），
   在每次成功匹配时记录 $1-$9/lastMatch/lastParen/leftContext/rightContext/
   input，并把它们作为 RegExp 构造器上的访问器暴露。
4. **`template.content`**：`HTMLTemplateElement` 此前完全缺失，
   Vue 的 `insertStaticContent` 在 `content.firstChild` 处崩溃。现解析器与
   `createElement('template')` 均产出 `HTMLTemplateElement`，按 13.2.5.3
   将子节点路由进 contents fragment；`innerHTML` 读写 contents；
   `template.content` 可直接插入文档（借用 fragment 路径，children 移出、
   fragment 留空）。
5. **XHR 可写属性**：`withCredentials`/`timeout` 补上 setter（jQuery
   transport 对 `xhrFields` 逐属性赋值，此前抛 “no setter for property”）。
6. **`document.createEvent` + `initEvent`**（遗留 DOM Level 2/3 API）：
   LoginInfo emitter 的 `emit` 路径依赖；createEvent 支持常见接口名并抛
   `NotSupportedError`，init*Event 含 `InvalidStateError` 语义。

**后续批次（2026-10，动态图片子资源）**：懒加载站点在首屏后动态设置
`img.src`/背景图，此前不会被抓取（首屏图片仅在载入后统一抓取一次），且
`img` 的 **load/error 事件从未派发**（站点靠 `onload` 移除占位、`onerror`
走回退/换源）。现已修复：

- **认领式晚到抓取**：`Page::ClaimPendingImageSources` 按 (元素, URL)
  记忆每个源只处理一次；浏览器层在滚动/交互/定时器回调产生的 DOM 变更后
  调度复用池的增量 pass（`BrowserController::SchedulePendingImageFetch`，
  按标签页合并并发，中途有新变更则再跑一轮），CLI 在安静泵后跑
  最多 3 轮“抓取→派发事件”（覆盖一次回退换源）。稳态代价 = 一次 DOM 行走、
  零请求。
- **`img` load/error 事件**：抓取层把 (元素, 成功/失败) 经 Page 队列
  归纳，脚本线程上以**非冒泡**事件派发（不会误触 `<body onload>`）。
- **滚动感知 IntersectionObserver**：视口根是文档坐标下的可见带
  （`viewport_size` + `scroll_offset`），新增注册表 +
  `RefreshIntersectionObservers()`（返回是否有存活观察者），浏览器层在
  滚动处理与 50ms 定时泵中调用并当场投递回调——这是真实站点懒加载的
  驱动源。旧限制（“只在 observe() 时计算一次”）已移除。

验证：滚动探针（`intersecting` 随 `scroll_y` 翻转、无变化不重复回调）、
动态 `src` 探针（静态/动态图片均渲染）、`load`/`error` 探针
（`start|a-load|b-err`）、浏览器集成测试（定时器换源 → 晚到抓取 + load
事件到页）。**剩余**：bilibili 卡片封面多数仍为占位——其图片组件按视口
邻近惰性挂载，需真实滚动/交互（CLI 无滚动，等同浏览器不滚动行为）；
GUI 侧滚动已能驱动 IO 重算并级联到抓取与事件。

## 所有权与生命周期

- 每个 `DomBinder` 拥有自己的 `ScriptEngine`（每文档一个 runtime）。
- 节点 wrapper 注册表把每个 wrapper 保活到 binder 销毁；从树中移除的节点
  由 binder 保留（`retained`），JS 可安全重新插入；`createElement` 创建的
  节点在插入文档前由 binder 持有。
- `document` 必须比 binder 活得久（binder 在页面加载时创建、随页面销毁）。
- `PageApis` 回调由浏览器层持有（`LocalStorage*`/网络栈），必须比 binder
  活得久（binder 随页面销毁，先于 controller）。
- 线程约束：与 `ScriptEngine` 一致，单个 binder 同一时刻只能在一个线程使用。

## 未实现（诚实标注）

- 属性 getter 仅覆盖上述子集；`childNodes`/`querySelectorAll` 返回快照数组
  （非活 NodeList）；无事件冒泡/捕获/默认行为；**module 脚本与动态 import**
  不执行；无 storage 事件。
- `append`/`replaceChildren` 只接受节点参数（字符串参数不转为文本节点）。
- Web API 子集：无 `sessionStorage`、`fetch` 无 Request/AbortController/
  FormData/取消；无 WebSocket/XHR。
- `Intl` 未编译进 quickjs-ng v0.16.1（需要升级引擎或引入 ICU 依赖，见
  依赖政策）；`navigator`/`screen`/`window.innerWidth` 等为引擎默认值
  （UA `neko-browser/0.1.0`、语言 "en-US"、视口 800×600@1x），真实窗口
  尺寸与浏览器语言尚未接入。
- async 脚本在同步引擎中按文档序在 classic+defer 之后执行（规范允许先于
  部分 classic 运行，本引擎为文档化近似）。
- microtask/Promise：job 队列在 Evaluate/CallGlobal/定时器/事件派发后泵送
  （见上文），但尚未与浏览器事件循环做完整对接（无宏任务/requestAnimationFrame）。
- Web IDL 完整类型系统（接口继承、字典、枚举转换等）。
- `document.domain` 的赋值**不**放宽引擎自己的同源判定（后者仍用页面真实
  origin），因此它目前只影响脚本读到/写到的值；且缺少 Public Suffix List
  校验（允许缩到 TLD）。在把该值接入 origin 比较之前必须先加 PSL 守卫。

## 长期架构目标

```text
JavaScript Engine → {Parser, AST, Bytecode, VM, GC} + Web IDL 绑定层
```

- QuickJS 仅作 JS runtime，**不得**替代 DOM/CSS/布局/渲染/导航/安全/存储。

## 参考

- ECMAScript Specification
- Web IDL Specification
- CSS Flexbox 1（auto margin/order/align-self）
- QuickJS: https://github.com/quickjs-ng/quickjs
