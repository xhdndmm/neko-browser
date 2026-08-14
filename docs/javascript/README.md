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
  `dispatchEvent`（window 别名，转发到 document）、`navigator`、`screen`，
  以及 DOM 接口构造器
  `Node`/`Element`/`HTMLElement`/`Document`/`Text`/`Comment`/
  `DocumentFragment`/`CSSStyleDeclaration`（其 `.prototype` 指向真实 wrapper
  原型，因此 `x instanceof Element` 与 `Element.prototype.foo = ...` 扩展都
  可用；直接 `new` 会抛 "Illegal constructor"）。
- **window**：`navigator`、`screen`、`innerWidth`/`innerHeight`/
  `devicePixelRatio`（引擎默认视口 800×600@1x，与 `renderer::Page` 默认布局
  宽度一致；真实窗口尺寸的接入是后续工作）。
- **navigator**：`userAgent`（与网络栈发送的 UA 一致）、`platform`
  （按 OS 宏）、`language`/`languages`（默认 "en-US"）、`onLine`、
  `cookieEnabled`、`hardwareConcurrency`、`vendor`。缺失的接口（如
  geolocation/clipboard）不提供，`"x" in navigator` 诚实地返回 false。
- **screen**：`width`/`height`/`availWidth`/`availHeight`（800×600）、
  `colorDepth`/`pixelDepth`（24）。
- **Document**：`documentElement`、`body`、`head`、`readyState`（恒为
  `"complete"`，脚本在解析完成后运行）、`title`（读写）、`getElementById`、
  `querySelector(All)`、`createElement`、`createTextNode`。
- **Node**：`nodeType`、`nodeName`、`textContent`（读写）、`parentNode`、
  `firstChild`、`lastChild`、`childNodes`、`appendChild`、`append`、
  `replaceChildren`、`insertBefore`、`removeChild`、`hasChildNodes`、
  `cloneNode`、`addEventListener`、`removeEventListener`、`dispatchEvent`。
  （`append`/`replaceChildren` 仅接受节点参数；规范中字符串参数会转换为
  文本节点，此处为文档化限制。）
- **Element**：`tagName`、`id`/`className`（读写）、`attributes`、
  `getAttribute/setAttribute/removeAttribute/hasAttribute`、`children`、
  `firstElementChild`、`querySelector(All)`、`getElementsByTagName`、
  `getElementsByClassName`、`innerHTML`（读写）、`style`（CSSStyleDeclaration）。
- **CSSStyleDeclaration**：`setProperty/getPropertyValue/removeProperty` 以及
  一组直接访问器（width/height/color/background-color/font-size/...），
  均以 style 属性为数据源（读写会改写 style 属性）。
- **事件循环**：`setTimeout/setInterval/clearTimeout/clearInterval` +
  `DomBinder::RunPendingTimers()`（到期即执行，重复定时器按间隔累加避免漂移）；
  `addEventListener` 注册的监听器可通过 `dispatchEvent`（JS 侧，元素或
  document）或 `DomBinder::DispatchEvent` / `DomBinder::DispatchDocumentEvent`
  （C++ 侧）同步派发。window 与 document 共享同一事件目标集：window 级
  监听器存储在 document 节点下。

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

**测试**：63 个 JS 单元测试 + 浏览器集成测试（脚本执行/console/错误/
定时器/外部脚本/文档序/defer/async/失败不中断/生命周期事件/promise 泵送/
未处理 rejection/多图并行解码/页面 origin）。ASan 无泄漏、TSan 无数据竞争。

## 所有权与生命周期

- 每个 `DomBinder` 拥有自己的 `ScriptEngine`（每文档一个 runtime）。
- 节点 wrapper 注册表把每个 wrapper 保活到 binder 销毁；从树中移除的节点
  由 binder 保留（`retained`），JS 可安全重新插入；`createElement` 创建的
  节点在插入文档前由 binder 持有。
- `document` 必须比 binder 活得久（binder 在页面加载时创建、随页面销毁）。
- 线程约束：与 `ScriptEngine` 一致，单个 binder 同一时刻只能在一个线程使用。

## 未实现（诚实标注）

- 属性 getter 仅覆盖上述子集；`childNodes`/`querySelectorAll` 返回快照数组
  （非活 NodeList）；无事件冒泡/捕获/默认行为；**module 脚本与动态 import**
  不执行；无 fetch/XHR；无 storage 事件。
- `append`/`replaceChildren` 只接受节点参数（字符串参数不转为文本节点）。
- `Intl` 未编译进 quickjs-ng v0.16.1（需要升级引擎或引入 ICU 依赖，见
  依赖政策）；`navigator`/`screen`/`window.innerWidth` 等为引擎默认值
  （UA `neko-browser/0.1.0`、语言 "en-US"、视口 800×600@1x），真实窗口
  尺寸与浏览器语言尚未接入。
- async 脚本在同步引擎中按文档序在 classic+defer 之后执行（规范允许先于
  部分 classic 运行，本引擎为文档化近似）。
- microtask/Promise：job 队列在 Evaluate/CallGlobal/定时器/事件派发后泵送
  （见上文），但尚未与浏览器事件循环做完整对接（无宏任务/requestAnimationFrame）。
- Web IDL 完整类型系统（接口继承、字典、枚举转换等）。

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
