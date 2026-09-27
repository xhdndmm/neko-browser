# 架构决策记录 0020：DOM 归 worker 线程独占 + GUI 消费不可变帧

- 状态：**Accepted**（2026-09）
- 决策者：架构组
- 关联：ADR 0016（多进程/渲染器会话）、AGENTS.md §9/§10/§62/§66

## 背景

一次真实崩溃（`neko_browser_gui` 加载 baidu 时 SIGSEGV）暴露了一个数据竞争：

```
gdb:  Node::node_type(this=0x0)          src/dom/include/neko/dom/node.h:94
  ← StyleEngine::ComputeElement          （Qt 主线程遍历 children_）
  ← Page::LayoutLocked ← WebView::EnsureLayout
TSan: Read  children_  by T4  (ThreadPool)  Page::SetElementImages（持 Page mutex）
      Write children_  by T35 (BrowserWorker) DomBinder::RunPendingTimers → JS innerHTML
```

根因：**DOM 树被三个线程无统一同步地访问**：

| 访问者 | 线程 | 是否持 `Page::mutex_` |
| --- | --- | --- |
| 布局 / 光栅化 / 命中测试 / 光标 | Qt 主线程（WebView） | 是 |
| DevTools DOM 树 / 计算样式 | Qt 主线程（MainWindow） | 是（`Page` 方法内） |
| 子资源注入（图片/字体/视频） | ThreadPool worker | 是（`Page` 方法内） |
| 脚本 / 定时器 / 事件改 DOM | BrowserWorker 线程 | **否** |

`Page::mutex_` 只能保护「调用 `Page` 方法」的访问；脚本经 `DomBinder` 直接改
DOM 树，不经过该锁。`std::vector<std::unique_ptr<Node>> children_` 在遍历中被
`push_back`/`insert`/`erase` 重分配，遍历方读到空/野指针即崩溃。

Tab 注释里「published page 不再被 worker 修改，持有句柄可安全 Layout/Rasterize/read」
的假设**不成立**：发布之后 worker 仍持续运行脚本/定时器原地修改同一 `Page`。

## 决策

**DOM 树（及其布局树）归 BrowserWorker 线程独占。** 其它线程一律不直接读写
`Page`/`dom::*`，只经不可变数据与 worker 交互。

```text
BrowserWorker 线程（唯一 DOM 所有者）
  ├─ 抓取/解析/样式/布局/光栅化
  ├─ 脚本 / 定时器 / 事件（唯一改 DOM 的地方）
  └─ 产出不可变帧 RenderFrame（视口 RGBA8888）+ 元数据
        │  TabSnapshot（共享句柄，只读）
        ▼
Qt 主线程（WebView / DevTools）
  └─ 只绘制帧与快照元数据；命中测试/滚动/查找/光标全部回 worker
```

具体约定：

1. **帧**：worker 在「视口/滚动/内容/焦点/悬停变化」时按需把当前视口光栅化为
   `RemoteFrame`（复用 ADR 0016 M2 的类型），连同内容高度、hover 链接、
   光标几何、查找结果一起放进 `TabSnapshot`。**渲染路径**（WebView）不再调用
   `Page::Layout/Rasterize/ElementAt/ContentHeight/...`，只画帧。
2. **worker 独占 DOM 修改，并持有一把 DOM 锁**：`Page` 的内部锁改为
   `std::recursive_mutex`，worker 在执行脚本/定时器/事件派发（`PumpScriptTimers`、
   `DispatchClick/Hover/HoverClear/Wheel/Keyboard`、`SubmitForm`、`SetTabViewport`）
   期间持有它。这样：
   - 池线程的子资源注入（`Page::SetElementImages` 等，本已持同一锁）与 worker 的
     DOM 修改序列化；
   - DevTools 的 DOM 树遍历（`MainWindow::PopulateDomTree`）在遍历前取同一锁，
     与 worker 序列化。
   锁是递归的：脚本回调可重入 `Page` 方法（同线程）不会自锁。
3. **DevTools**：暂保留对活 DOM 的读取，但已在 DOM 锁下进行（正确，代价是脚本
   长跑时 DevTools 短暂阻塞）。完全 worker 侧序列化（节点 id 快照）列为后续可选。
4. **`TabSnapshot::page` 仅为 DevTools 保留**：WebView 不再使用它；渲染只消费
   `TabSnapshot::frame` 与元数据。

## 里程碑

- **B1（已完成）**：worker 侧帧产出 + `TabSnapshot` 帧字段；WebView 改为只画帧
  （移除全部 `snapshot_.page->*`）；worker 在执行脚本时持 `Page` 的递归 DOM 锁，
  DevTools 遍历前取同一锁。
- **B2（可选，后续）**：DevTools 改为消费 worker 序列化的 DOM 树 / 计算样式快照，
  彻底移除 GUI 对活 DOM 的读取（目前已在锁下，正确但会在脚本长跑时阻塞）。
- **B3（后续）**：把「DOM 锁下并发」的行为固化为 TSan 回归用例并纳入 CI。

## 备选方案

- **方案 A（worker 执行脚本时持 `Page::mutex_`）**：改动最小，但 GUI 布局会与
  脚本执行互相阻塞（脚本长跑时 UI 卡顿），且没有解决「GUI 在线程上重建布局
  树」的架构错位。作为过渡可选，不作为终态。
- **保持现状 + 仅加锁**：治标；DOM 仍被多线程共享，未来任何新访问点都会
  重新引入同类崩溃。

## 后果

- 优点：DOM 单线程独占，消除整类竞争；GUI 不再因脚本/布局阻塞；与渲染进程
  模式（ADR 0016 M2）统一为「帧」模型，为 M3/M4（Network/GPU 进程、共享内存
  帧传输）铺路。
- 缺点：帧经内存拷贝（进程内为一次光栅化 + 共享句柄，成本可接受）；交互
  反馈依赖 worker 的下一次快照（现有 50ms 泵已覆盖）；光栅化现在在 worker 线程
  串行进行（不再用 UI 光栅池），大页面单帧成本上升——可用控制器自己的光栅池
  优化（后续）。
- 验证：TSan 下加载 baidu，`src/dom|style|renderer` 帧的数据竞争从多组降至 0；
  剩余 TSan 报告均为 Qt 信号/槽内部机制（非 neko 数据结构）。
- 诚实边界：DevTools 仍读活 DOM（在锁下）；B2 完成前，`TabSnapshot::page`
  仍存在，仅限 DevTools 使用。
