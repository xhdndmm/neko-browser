# 线程、进程与异步 I/O 模型

- 状态：**当前实现**（2026-09 校验，随代码演进更新）
- 相关 ADR：[0013 渲染缓存与并行栅格化](adr/0013-renderer-caching-parallelism.md)、
  [0016 多进程架构](adr/0016-multiprocess-architecture.md)、
  [0018 内建 DNS 解析器](adr/0018-built-in-dns-resolver.md)

本文档是并发行为的**唯一入口**：每个使用并发的子系统在此登记其线程归属、
同步方式与线程安全边界。新增后台线程前必须先在这里登记（AGENTS.md §10）。

---

## 1. 线程一览

| 线程 | 所有者 | 职责 | 备注 |
| --- | --- | --- | --- |
| GUI 线程（Qt 主线程） | `ui::MainWindow` | Qt 事件循环、绘制 `WebView` 帧、把用户输入投递给 `BrowserWorker` | 渲染路径只消费 `TabSnapshot::frame`，不触碰 DOM/布局；DevTools 的 DOM 遍历在 `Page` 的 DOM 锁下进行（ADR 0020） |
| Worker 线程 | `ui::BrowserWorker` | 执行所有 `BrowserController` 调用（导航、脚本、布局、光栅化、快照） | 串行执行队列中的任务；这是**引擎的唯一变更线程** |
| Worker 池线程（N 个） | `BrowserController::pool_`（默认 = 硬件并发数） | 并行子资源抓取（样式表/图片/视频/网页字体）、并行带光栅化 | 任务是「每请求一线程」的阻塞式 I/O，不是事件循环 |
| 子进程 I/O 线程 | 每个渲染进程会话一个 | `RendererSession` 读写子进程管道、解析帧 | 只入队 C++ 事件；帧的消费发生在 Worker 线程 |
| WebSocket I/O 线程 | 每个 `WebSocket` 连接一个 | socket 收发、帧解析、ping→pong | 事件入队后由 Worker 线程泵（与定时器同一时钟） |
| 媒体解码线程 | 临时池任务（`FetchPageVideos`） | FFmpeg 解复用/解码/像素转换 | 预算有界（帧数 + RGBA 字节封顶） |
| DNS | 调用方线程 | 每次解析自建 UDP socket + 超时重试 | 无独立线程；高速缓存加锁（ADR 0018） |
| 渲染子进程 | `ipc::Subprocess` | 解析 HTML/CSS、布局、光栅化（M2 隔离模式） | 独立地址空间；通过 stdin/stdout 管道通信 |

线程总数上限：GUI + 1（Worker）+ 池大小 + 每会话 I/O 线程 + 每 WebSocket
一个。池线程数由硬件并发决定，没有无限增长的线程创建路径。

---

## 2. 归属与不变量

1. **引擎状态只有一个变更线程**：`BrowserController` 的公开方法只允许在
   Worker 线程调用（`BrowserWorker` 通过任务队列保证）。GUI 线程通过
   `TabSnapshot`（互斥锁保护的**值拷贝**）观察状态；渲染路径只消费
   `TabSnapshot::frame`（不可变视口位图，ADR 0020），不触碰 DOM/布局。
   **DOM 归 Worker 线程独占**：Worker 在执行脚本/定时器/事件期间持有
   `Page` 的递归 DOM 锁（`AcquireDomLock`）；池线程的子资源注入与 DevTools
   的 DOM 遍历取同一把锁，因此与 Worker 的 DOM 修改序列化。
2. **快照不可变**：`renderer::Page` 在发布后不再被 Worker 线程原地大改；
   会原地变的只有「动画帧像素」（GIF/`<video>` 帧覆盖，见 ADR 0013 的
   display list 版本号规则）与画布 backing store。GUI 线程在同一时刻只读
   这些缓冲，写入方在版本号变化后才可能重写对应区域。
3. **不跨线程传递裸指针**：跨线程传递的所有权一律是值或 `shared_ptr`
   （例如 `TabSnapshot::page`、`Tab::image`、`GifAnimation`）。
4. **叶子原语的线程安全是显式契约**：
   - `graphics::FontFace`：内部互斥锁串行化 FreeType 访问（face 状态非线程
     安全），`RenderGlyph`/`OutlineGlyph`/`Advance` 可从任意线程调用。
   - `graphics::GlyphCache`：进程级缓存，自带上限与锁；取用返回**自有副本**，
     因此并发淘汰不会让调用方悬空。
   - `graphics::FontRegistry`：选择器缓存加锁；web 字体注册会失效缓存。
   - `network::DnsResolver`：缓存加锁；`Resolve` 每次自建 socket，可并发。
   - `network` 的系统信任库（`system_trust_store.cpp`）：**进程级记忆化**的
     宿主 CA 锚点（`shared_ptr<X509>`，按候选列表为键），互斥锁保护；每个
     连接把锚点插入自己的 `X509_STORE`（`X509_STORE_add_cert` 内部加引用）。
     缓存只在首次发现时写入、**从不失效**（改 CA 库需重启，与浏览器的根证书
     取舍一致），因此不存在并发失效/悬空窗口。刻意不析构（进程退出时
     OpenSSL 的清理顺序不确定）。
   - `base::ThreadPool`：任意线程可 `Post`/`Submit`。
5. **析构顺序**：`BrowserController::pool_` 声明在最后，析构最先 ——
   `~ThreadPool` 会排空队列，此时其后捕获 `this` 的抓取任务所引用的
   存储/注册表仍存活。

---

## 3. 同步机制

| 场景 | 机制 |
| --- | --- |
| GUI ↔ Worker 的命令 | 任务队列 + 条件变量；每条命令执行后 `emit StateChanged()` |
| GUI ↔ Worker 的快照 | `std::mutex` 保护控制器状态，返回拷贝 |
| DOM 访问（Worker 脚本 ↔ 池注入 / DevTools） | `Page` 的递归 `std::recursive_mutex`；Worker 执行脚本时持锁，池线程与 DevTools 取同一锁（ADR 0020） |
| Worker ↔ 池任务 | `std::future`（`Submit`）与 fire-and-forget（`Post`）；导航在池任务里等待子资源 future 前会先确保池大小 > 1（单线程池走内联路径，避免自等待死锁） |
| 渲染子进程 | 长度前缀的帧 + 版本号；`Pump()` 非阻塞读，超时/EOF 视为会话失败 |
| WebSocket | 有界事件队列；binder 析构时 join I/O 线程 |
| 动画帧 | 单一时钟（`PumpScriptTimers`，GUI 50 ms 定时器触发）推进，帧推进与显示列表失效在同一次 tick 内完成 |

**禁止**：共享可变状态 + 多线程 + 隐式加锁的组合。需要并行的循环一律
「切分只读输入 → 各自产出独立输出 → 汇总」，不共享中间状态。

### 3.1 锁序（Lock order）

引擎里有两把会嵌套的锁：`Page` 的 DOM 锁（`std::recursive_mutex`）和
`BrowserController::mutex_`（保护 GUI 可见状态）。**唯一合法的顺序是：

> 先取 Page 的 DOM 锁，再取控制器 mutex。反向禁止。**

原因：`ProduceFrame` 在 worker 上做完布局/光栅化（每次 `tab.page->*` 调用内部
各取一次 DOM 锁）之后，才在末尾取 `mutex_` 发布帧——这是 **page → controller**。
若另一条路径先取 `mutex_` 再伸手进 `tab->page`（例如在控制器锁内调用
`page->FindMatches()` / `page->DumpDom()`），就构成 ABBA 死锁。TSan 会把它报成
`lock-order-inversion (potential deadlock)`。

因此约定：

- **所有 Page 访问必须在 `mutex_` 之外进行。** 需要控制器状态时，先在锁内取到
  需要的值并释放，再做 Page 工作，最后再取一次 `mutex_` 写回结果。
- `mutex_` 只用于短读写，**不得跨越网络抓取、HTML 解析或 Page 布局**。

### 3.2 持 DOM 锁时必须让 Page 存活

持有 DOM 锁的整个期间，必须有一份 `std::shared_ptr<renderer::Page>` 保活该
Page，且该 shared_ptr **声明在锁之前**（局部变量逆序析构，保活要最后释放）。

原因：同一函数内完全可能发生导航（脚本 `location` 赋值、表单提交、定时器
回调），而导航会替换 tab 的 Page 并释放旧的。此时若再去解锁旧 Page 的
mutex，就是 use-after-free。`browser_controller.cpp` 中所有「取 DOM 锁」的
路径都遵循这一模式。

### 3.3 并行光栅化与 DOM 锁的交互

`ProduceFrame` 在**持 DOM 锁**的帧泵路径上调用带池光栅化（`pump` →
`ProduceFrame` → `RasterizeFull(..., pool_)`），因此并行带的提交必须满足：

- **调用线程亲自光栅化第 0 带**，只向池提交 `带数-1` 个任务。任务数严格
  小于 worker 数，保证即使有 worker 阻塞在 DOM 锁上（例如池上的图片附加
  任务正等锁），剩余 worker 也能把已提交的带跑完。若提交数等于 worker
  数，就会出现「调用方持锁等带、池任务等锁」的循环等待。
- 带内任务只写互不重叠的行区间，不取 DOM 锁，不依赖池上其它任务；
  `RasterizeParallel` 内部的可见带交叉（`band_y0_/band_y1_` 取交）保证
  带视图只写串行路径会写的行。
- 文本与图形命令在带视图里用 `RowRangeVisible()` 提前跳过：字形缓存的
  查表是全局锁 + 像素拷贝，若每个带都重做，N 个带会把一次串行工作放大
  N 倍并让池线程在全局锁上互相踩踏（实测 20 带下总 CPU 从 1.9s 涨到 42s）。
- 定时器泵（`PumpScriptTimers` / `PumpScriptTimersUntilQuiet`）全程持
  DOM 锁：泵里的定时器/事件回调会写 DOM，而池上的子资源任务在锁下读
  DOM，不加锁就是数据竞争（TSan 实测报出 `SetAttribute` vs
  `CollectImageSourcesLocked`）。
- **池线程绝不触碰 Tab**：Tab（及其 `frame_dirty` 等标志）归 worker 线程
  所有，而 worker 写这些标志时并不同时持 `mutex_`（输入派发只持 DOM
  锁），所以「池任务在 `mutex_` 下写 `frame_dirty`」与 worker 的写是同一
  字段上的两种锁 → 数据竞争（TSan：`DispatchKeyboard` vs
  `SchedulePendingImageFetch` 的池任务）。晚到图片抓取完成后只升
  `Tab::ImageFetchState` 里的原子信号（`wake_frame`/`reschedule`），由
  worker 的泵调用 `ApplyDeferredImageFetchSignals()` 落地：置脏帧、
  补一次抓取。池侧也负责不了重排 —— Tab 状态只能由 owner 线程改。

---

## 4. 进程模型

### 4.1 默认：单进程多线程

日常路径（GUI、headless CLI、测试）默认单进程：Worker 线程负责抓取/解析/
布局/光栅化，池线程并行子资源与带光栅化。

### 4.2 隔离模式：渲染进程（ADR 0016 M2）

- `neko_browser --renderer-process` / GUI 的隔离开关会让**每个站点**拥有一个
  渲染子进程（`ipc::Subprocess`：`fork` + `execvp`，stdin/stdout 管道）。
- 子进程负责 HTML/CSS/布局/光栅化和页内脚本，回传位图 + DOM 摘要 + 标题；
  父进程保留网络、Cookie、存储、下载、UI。
- 回退与容错：子进程缺失/崩溃/协议版本不匹配 → 该标签页标记为会话失败
  （错误页），下一次导航重新拉起子进程；不会把崩溃的会话伪装成正常页面。
- 尚未实现：进程沙箱（seccomp/AppArmor）、GPU 进程、网络进程、跨进程
  共享内存位图（当前走管道字节流）、子进程资源配额。

---

## 5. 异步 I/O 现状（诚实说明）

当前 I/O 模型是**线程池 + 阻塞套接字**，不是事件循环：

- HTTP(S) 抓取：每个请求一个池线程，内部同步 `Socket::Connect` → TLS 握手
  → 读写，带连接/读/总超时与重定向、压缩、缓存（见 `docs/networking/`）。
- DNS：每次解析自建 UDP socket + 重试（3 次默认），缓存命中则完全不发包；
  没有 getaddrinfo 阻塞池。
- 好处：实现简单、超时可预期、无回调地狱；代价：请求数受池大小限制，
  大量并发请求会排队。
- **NOT IMPLEMENTED**：epoll/kqueue/IOCP 事件循环、HTTP/2 多路复用、
  连接池复用、QUIC/HTTP/3。这些是性能演进项，接口边界已保留在
  `network::` 内部（引擎其他部分只见项目自有接口，不见第三方网络 API）。

---

## 6. 测试覆盖

| 主题 | 覆盖 |
| --- | --- |
| 并行带光栅化与串行逐像素一致 | `tests/unit/paint`（`RasterizerTest.ParallelRasterizationMatchesSerial`） |
| 并行光栅化尊重可见带（带视图不外写） | `tests/unit/paint`（`RasterizerTest.ParallelRasterizationRespectsVisibleBand`） |
| `RasterizeInto` 分带并行与串行逐字节一致（含尾部不满带） | `tests/unit/renderer`（`PageTest.ParallelBandedRasterizeMatchesSerial`） |
| 字形缓存跨线程 | `tests/unit/graphics`（缓存/并发用例） |
| DNS 缓存与超时 | `tests/unit/network`（12 个用例，含本地 UDP 服务器） |
| WebSocket 线程模型 | `tests/unit/network` + JS 绑定测试（事件在泵中派发） |
| 渲染进程协议/会话 | `tests/unit/browser`（`renderer_protocol_test`、`renderer_session_test`、`renderer_mode_test`） |
| 崩溃与回退路径 | `renderer_session_test`（`ChildCrashIsDetectedAndReported`、子进程无法启动、协议往返/版本） |
| 动画时钟 | `tests/unit/renderer`（GIF 帧推进）、`tests/unit/browser`（直接导航 GIF 播放） |
| DOM 竞争（ADR 0020） | TSan 下加载真实页面：`src/dom|style|renderer` 无数据竞争；渲染路径不再有跨线程 DOM 访问（剩余 TSan 报告为 Qt 信号/槽内部机制） |
| 定时器泵持 DOM 锁（JS 写 vs 池读） | TSan 下 `BrowserControllerTest` 全组（98 用例）：无锁泵曾稳定报 `Element::SetAttribute` vs 池线程 `CollectImageSourcesLocked` 竞争，加锁后连续多轮全绿 |
| 池线程不触碰 Tab（晚到抓取信号延迟落地） | TSan 下 `BrowserControllerTest` + `UiSmokeTest` 全组：池任务曾在 `mutex_` 下写 `tab->frame_dirty`，与 worker 在 DOM 锁下的同名写竞争（CI tsan #141 `DispatchKeyboard` vs `SchedulePendingImageFetch` 池任务）；改为原子信号 + worker 泵落地后全绿 |

---

## 7. 已知限制与后续工作

- 单线程池大小固定（硬件并发数），没有按优先级/域限流（例如图片抓取不会
  让位给关键路径请求）。
- **页面脚本（QuickJS，无 JIT）在单一线程上执行**：真实站点的加载 CPU
  几乎全在 `RunPageScripts` + 定时器泵里的 JS 上（实测 bilibili：单线程
  ~6.8s user，池线程合计 ~0.2s）。任何带内并行都无法切分单个脚本的执行——
  这与所有浏览器一致；可并行的是栅格化（本次已接入）、图片解码与子资源
  抓取（此前已在池上）。Debug 构建（-O0，QuickJS 同为 -O0）会显著放大这
  一段耗时，真实使用应用 Release 构建。
- DevTools 仍读取活 DOM（在 DOM 锁下），脚本长跑时会短暂阻塞；完全
  worker 侧序列化快照是后续可选工作（ADR 0020 B2）。
- 无事件循环：连接复用与 HTTP/2 需要它，属于后续阶段。
- 进程隔离模式下子进程崩溃只影响该站点，但**不隔离**：没有沙箱、没有
  内存/CPU 配额、没有共享内存位图。
- 线程数随「每站点一个子进程 + 每连接一个 WebSocket 线程」线性增长；标签页
  数量极大时需要上限策略（尚未实现）。
