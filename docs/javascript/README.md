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
  `setInterval`、`clearInterval`。
- **Document**：`documentElement`、`body`、`title`（读写）、`getElementById`、
  `querySelector(All)`、`createElement`、`createTextNode`。
- **Node**：`nodeType`、`nodeName`、`textContent`（读写）、`parentNode`、
  `firstChild`、`lastChild`、`childNodes`、`appendChild`、`insertBefore`、
  `removeChild`、`hasChildNodes`、`cloneNode`、`addEventListener`、
  `removeEventListener`、`dispatchEvent`。
- **Element**：`tagName`、`id`/`className`（读写）、`attributes`、
  `getAttribute/setAttribute/removeAttribute/hasAttribute`、`children`、
  `firstElementChild`、`querySelector(All)`、`getElementsByTagName`、
  `getElementsByClassName`、`innerHTML`（读写）、`style`（CSSStyleDeclaration）。
- **CSSStyleDeclaration**：`setProperty/getPropertyValue/removeProperty` 以及
  一组直接访问器（width/height/color/background-color/font-size/...），
  均以 style 属性为数据源（读写会改写 style 属性）。
- **事件循环**：`setTimeout/setInterval/clearTimeout/clearInterval` +
  `DomBinder::RunPendingTimers()`（到期即执行，重复定时器按间隔累加避免漂移）；
  事件监听器由 `addEventListener` 注册，可通过 `dispatchEvent`（JS 侧）或
  `DomBinder::DispatchEvent`（C++ 侧）同步派发。

**页面集成**：`browser::RunPageScripts`（`neko/browser/page_scripts.h`）在
HTML 解析后按文档顺序执行页内 `<script>`，随后重跑样式级联（脚本可改 DOM）。
`BrowserController::LoadBytes` 在发布页面之前调用它，并把运行时句柄存到
Tab（`PumpScriptTimers` 供工作者线程推进定时器）；GUI 用 50ms QTimer 驱动；
CLI `--url` 路径同样执行脚本。

**外部脚本与 async/defer**（WHATWG HTML §4.12.1 的 classic 模型）：

- classic（无 async/defer）：按文档序抓取并同步执行，阻塞后续脚本；
- `defer`：在全部 classic 脚本之后按文档序执行（解析已完成，与规范一致）；
- `async`：在 classic+defer 阶段之后按文档序执行 —— 同步引擎的文档化近似
  （不抢占管线，无法先于更早的 classic 运行）。
- 外部脚本经同一网络栈（生产带 Cookie）抓取；module 与动态 import 不支持。

**测试**：48 个 JS 单元测试 + 10 个浏览器集成测试（脚本执行/console/错误/
定时器/外部脚本/文档序/defer/async/失败不中断/多图并行解码/页面 origin）。
ASan 无泄漏、TSan 无数据竞争。

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
- async 脚本在同步引擎中按文档序在 classic+defer 之后执行（规范允许先于
  部分 classic 运行，本引擎为文档化近似）。
- microtask/Promise 与浏览器事件循环的完整对接（当前为同步任务泵）。
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
