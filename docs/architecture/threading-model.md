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
| GUI 线程（Qt 主线程） | `ui::MainWindow` | Qt 事件循环、绘制 `WebView`、把用户输入投递给 `BrowserWorker` | 从不直接访问 DOM/布局/渲染器内部状态 |
| Worker 线程 | `ui::BrowserWorker` | 执行所有 `BrowserController` 调用（导航、脚本、布局、光栅化、快照） | 串行执行队列中的任务；这是**引擎的唯一变更线程** |
| Worker 池线程（N 个） | `BrowserController::pool_`（默认 = 硬件并发数） | 并行子资源抓取（样式表/图片/视频/网页字体）、并行带光栅化 | 任务是「每请求一线程」的阻塞式 I/O，不是事件循环 |
| UI 光栅池（2 个） | `BrowserWorker::raster_pool_` | `WebView` 的 `Page::RasterizeFull(..., pool)` 并行带光栅化 | GUI 线程不得直接光栅化大页面，否则掉帧 |
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
   `TabSnapshot`（互斥锁保护的**值拷贝**：`shared_ptr<Page>`/`Image` 等）
   观察状态，绝不持有指向 DOM 的内部指针。
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
| Worker ↔ 池任务 | `std::future`（`Submit`）与 fire-and-forget（`Post`）；导航在池任务里等待子资源 future 前会先确保池大小 > 1（单线程池走内联路径，避免自等待死锁） |
| 渲染子进程 | 长度前缀的帧 + 版本号；`Pump()` 非阻塞读，超时/EOF 视为会话失败 |
| WebSocket | 有界事件队列；binder 析构时 join I/O 线程 |
| 动画帧 | 单一时钟（`PumpScriptTimers`，GUI 50 ms 定时器触发）推进，帧推进与显示列表失效在同一次 tick 内完成 |

**禁止**：共享可变状态 + 多线程 + 隐式加锁的组合。需要并行的循环一律
「切分只读输入 → 各自产出独立输出 → 汇总」，不共享中间状态。

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
| 字形缓存跨线程 | `tests/unit/graphics`（缓存/并发用例） |
| DNS 缓存与超时 | `tests/unit/network`（12 个用例，含本地 UDP 服务器） |
| WebSocket 线程模型 | `tests/unit/network` + JS 绑定测试（事件在泵中派发） |
| 渲染进程协议/会话 | `tests/unit/browser`（`renderer_protocol_test`、`renderer_session_test`、`renderer_mode_test`） |
| 崩溃与回退路径 | `renderer_session_test`（`ChildCrashIsDetectedAndReported`、子进程无法启动、协议往返/版本） |
| 动画时钟 | `tests/unit/renderer`（GIF 帧推进）、`tests/unit/browser`（直接导航 GIF 播放） |

---

## 7. 已知限制与后续工作

- 单线程池大小固定（硬件并发数），没有按优先级/域限流（例如图片抓取不会
  让位给关键路径请求）。
- 无事件循环：连接复用与 HTTP/2 需要它，属于后续阶段。
- 进程隔离模式下子进程崩溃只影响该站点，但**不隔离**：没有沙箱、没有
  内存/CPU 配额、没有共享内存位图。
- 线程数随「每站点一个子进程 + 每连接一个 WebSocket 线程」线性增长；标签页
  数量极大时需要上限策略（尚未实现）。
