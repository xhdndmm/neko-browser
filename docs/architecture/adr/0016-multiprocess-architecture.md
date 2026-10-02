# 架构决策记录 0016：多进程架构（进程模型 + IPC 设计 + 迁移路线）

- 状态：**Accepted**（2026-08，M1 已实现；2026-09，M2 已实现；2026-10，M3a 已实现；M3b+/M4/M5 见下）
- 决策者：架构组

## 背景

AGENTS.md 的长远目标包含多进程（Browser / Renderer / Network / GPU /
Utility 进程 + IPC），安全模型要求"沙箱、进程隔离、导航安全"。当前实现
是单进程：GUI 线程 + BrowserWorker 线程，网络、解析、样式、布局、光栅化、
JS 全部在同一个地址空间。任何一处崩溃（恶意页面、解码器缺陷）都会带崩
整个浏览器，且沙箱无从谈起。

多进程改造不能一步到位（AGENTS.md §42/§54：小步可验证），需要一个
可演进的进程模型 + 稳定的 IPC 基础设施，然后按里程碑把子系统逐个搬出
浏览器进程。

## 决策

### 进程模型（目标形态）

```text
Browser 进程（UI + 控制器 + profile/storage + cookie/权限裁决）
    │ IPC
    ├── Renderer 进程（每站点一个：HTML/CSS/JS/DOM/布局/光栅化）
    ├── Network 进程（HTTP/TLS/DNS/cache）
    ├── GPU 进程（合成 + 光栅化加速，软件合成器的 GPU 后端宿主）
    └── Utility 进程（按需：媒体/图像解码、PDF、下载后处理）
```

- Renderer 是纯"被隔离的计算端"：只经 IPC 收发（LoadRequest →
  帧/位图、DOM 文本、输入事件、脚本调用），无文件系统/网络直连
  （M2+ 交给 Network 进程；M1 沿用进程内网络栈，见"诚实边界"）。
- 每个 Renderer 对应一个 origin 站点实例（同站导航复用），崩溃即
  销毁重建，Browser 进程不受影响。

### IPC 基础设施

- 新增 **`neko::ipc`** 模块（依赖仅 base）：
  - `Channel`：字节流通道（POSIX pipe / Windows 匿名管道），
    **帧协议**：`u32le 长度 | 载荷`，单帧上限 64 MiB（防恶意子进程
    或损坏帧打爆内存）；阻塞读写，EINTR 重试。
  - `Subprocess`：跨平台子进程封装（POSIX `fork+exec`；Windows
    `CreateProcess` + `SetHandleInformation` 继承句柄 + 命令行转义），
    以子进程 stdin/stdout 作为 Channel（无需 fd 传递协议）。
  - 自研而非库：消息很简单，引入 gRPC/nng 等会带来大型依赖；协议
    设计上保持"载荷自己编解码"，IPC 层只保证字节帧的完整与上界。
- **Renderer 协议**（`neko::browser::renderer_protocol`）：版本化二进制
  载荷，`LoadRequest{url, viewport_w, viewport_h}` →
  `LoadResult{status, rgba 位图, width, height, dom, title, error}`。
  编解码全部带边界检查（恶意的另一端是威胁模型的一部分）。

### 里程碑

- **M1（本 ADR 首次交付）**：IPC 基础设施 + Renderer 子进程跑完整页面管线
  （fetch→parse→style→layout→rasterize），经 IPC 返回位图 + DOM 文本；
  子进程入口是同一二进制 `--renderer-child`（Chromium 也是独立二进制，
  此处复用 CLI 二进制作为过渡，后续拆分独立 `neko_renderer` 可执行文件）；
  CLI `--renderer-process` 走 RendererHost 加载 + 截图/dump-dom；单元测试
  （帧协议、管道往返、协议编解码）+ 端到端集成测试（真实子进程加载
  本地夹具页面并回传位图）。GUI 尚未接入（M2）。
- **M2（已实现，2026-09）**：**渲染器会话**（持久子进程 + 交互协议）+ GUI 接入，
  见下节“M2 设计”。
- **M3a（已实现，2026-10）**：**Network 进程首切片**——`NetworkSession` /
  `RunNetworkChild`（`--network-child`）：DNS/TCP/TLS/HTTP 在子进程；Cookie
  罐留在浏览器侧，子进程在每个跳（含重定向）经 `kCookieLookup` 回问、浏览器
  以 CookieReply 应答（HttpOnly 值从不进入子进程）；顶层文档抓取走子进程
  （CLI `--network-process`、GUI `--network-process` 、`BrowserController`
  的 `NetworkOptions`）；子进程崩溃 = 单次抓取失败并按需重启，不带走浏览器。
  **M3b（尚未）**：子资源/脚本/fetch() 也经 Network 进程（现仍走进程内栈）、
  流式响应与 HTTP cache、渲染器↔网络进程的授权/直连模型。
- **M4**：GPU 进程（SoftwareCompositor 的 GPU 实现 + 共享内存传输
  ——IPC 帧协议对位图足够，大帧走共享内存是 M4 的优化）。
- **M5**：沙箱（Linux seccomp/namespace、Windows AppContainer、macOS
  sandbox-exec）+ 站点隔离。

### 诚实边界（M1）

- M1 的 Renderer 子进程仍链接全引擎（含网络栈），隔离是**进程级
  崩溃隔离 + 架构缝就位**，不是安全沙箱（沙箱是 M5）；文档、矩阵、
  README 均如实标注。
- 每页新建子进程（无会话复用），开销换取简单与正确；复用是 M2。
- Windows 上 `fork` 不存在，Subprocess 用 CreateProcess 实现（与现有
  winsock2 路径同策略，MSVC 编译 CI 验证）。

### M2 设计（已实现）

与 M1 的“一次加载一个子进程”不同，M2 的**会话**在整个页面生命周期内存活：

```text
浏览器（BrowserController + WebView）                 渲染器子进程（--renderer-session）
  ├─ 抓取顶层文档（带 Cookie）        ── kLoad{字节, Content-Type, URL, 视口} ──▶
  │                                    ◀── 状态{URL, 标题, 内容高度, changed} ──
  ├─ 用户输入转发                    ── kClick/kHover/kWheel/kKey/kScroll ──▶
  ├─ 帧按需请求                      ── kSnapshot{视口, 滚动偏移} ──▶
  │                                    ◀── 视口位图（RGBA8888）+ 状态 ──
  ├─ 页面导航回传（浏览器重跑）      ◀── redirect_url ──
  └─ 定时器泵                        ── kPump ──▶（子进程内 setTimeout/动画）
```

- **协议**（`renderer_protocol.h` 的 `SessionOp`/`RendererSessionRequest`/
  `RendererSessionReply`，版本号与 M1 载荷分开）：操作与回复都是版本化二进制
  载荷，编解码全部带边界检查、字段有尺寸上限（文档 32 MiB、文本字段 1 MiB、
  位图 512 MiB 且必须与宽高一致），越界即拒绝。
- **子进程宿主**（`renderer_session_host.cpp`）：一个 `BrowserController` +
  单 tab，文档字节经 IPC 到达（子进程**不抓取顶层文档**）；页面内的链接点击 /
  `location` 赋值会让子进程的控制器自己导航——宿主把它回传为 `redirect_url`，
  由浏览器用它自己的网络栈（带 Cookie）重跑一遍。页面存储指向一个每会话的临时
  目录，退出时删除。`--renderer-session` 是子进程入口。
- **浏览器侧**（`renderer_session.h/cpp`）：`Spawn`/各操作 RPC/`Shutdown`，
  同步阻塞（控制器本来就是单线程同步模型）。`BrowserController::RendererOptions`
  开启后，HTML 文档经会话渲染；**每个 tab 绑定一个站点会话**（同站导航复用，
  跨站或崩溃后重建），非 HTML 内容（图片/PDF/音频/文本）仍走进程内查看器。
- **GUI 接入**：WebView 绘制子进程帧（视口尺寸由 GUI 上报，子进程据此布局），
  交互（点击/悬停/滚轮/键盘/滚动）经既有的 worker 动作转发；滚动条范围取
  子进程报告的内容高度；指针手型来自子进程报告的 hover 链接；脚本发起的滚动
  沿用既有的 latch 机制。`neko_browser_gui --renderer-process` 与
  `neko_gui_screenshot --renderer-process` 启用。
- **帧按需**：回复中的 `changed` 标志（文档/布局版本变化）决定是否拉帧；
  滚动引起的拉帧按 40 ms 节流合并，避免拖动滚动条时打爆管道。

### 诚实边界（M2）

- 隔离仍是**进程级崩溃隔离 + 架构缝就位**，不是安全沙箱（沙箱/站点隔离是 M5）：
  子进程仍能访问文件系统与网络。
- **网络拆分**：顶层文档由浏览器抓取（Cookie 在浏览器侧计算并注入）；子进程内的
  页面导航会在子进程里先自己抓取一次（无 Cookie，可能失败），随后浏览器重跑
  （带 Cookie）——即一次多余的抓取，M3 的 Network 进程消除。子资源（图片/CSS/
  字体/脚本）仍在子进程内直连抓取，不带 Cookie。
- **存储**：子进程里的页面存储（localStorage/IndexedDB/Cookie 写入）指向每会话
  临时目录，不持久化、与浏览器 profile 不共享（M3 的存储服务解决）。
- **DevTools**：隔离模式下 DOM/Computed/Network/Console 面板为空（数据在子进程，
  协议尚未回传）；浏览器侧只记录会话级别的失败。
- **帧传输**：位图经 IPC 拷贝（1100×800 视口约 3.5 MB/帧），没有损坏矩形与共享
  内存（M4）；重脚本动画页面（如 cctv.com）每帧成本由引擎自身决定，两种模式
  相同，但隔离模式不阻塞 GUI 线程。
- **会话粒度**：每 tab 一个会话（同站点复用），不是跨 tab 共享的站点实例。
- 文本光标（caret）在隔离模式下不闪烁（子进程帧不含光标图层）。

## M3a 设计（已实现，2026-10）

```text
浏览器（BrowserController）                       网络子进程（--network-child）
  │  kFetch{url, cookie_header} ──────────────▶  DNS → TCP → TLS → HTTP/1.1
  │  ◀── kCookieLookup{hop_url} ──────────── HttpGet 的 HeaderProvider 逐跳回调
  │  ── CookieReply{cookie} ──────────────▶
  │  ◀── kResponse{status, headers, body} / kError{message}
```

- **协议**（`network_protocol.h`）：版本化二进制载荷（fetch/shutdown；
  response/error/cookie-lookup；cookie-reply），字段有尺寸上限、响应体
  48 MiB 帧上限，全部解码边界检查（对端是威胁模型的一部分）。
- **会话**（`network_session.h`）：`Fetch` 同步阻塞（与控制器单线程模型一致），
  收到 `kCookieLookup` 时调用浏览器侧回调并把结果回给子进程；子进程死亡时
  通道关闭 → 本次抓取失败，`BrowserController` 在下一次抓取时惰性重启
  （`EnsureNetworkSession`），并在重启失败时回退进程内网络栈。
- **子进程**（`network_host.h`）：单线程循环；`HttpGet` 的 HeaderProvider
  把每跳 URL 回传浏览器；DNS/TLS 信任库/重定向/压缩全在子进程执行
  （复用现有 `neko::network` 实现，不重复造轮子）。
- **GUI/CLI 接入**：`neko_browser_gui --network-process`、
  `neko_browser --network-process --url ...`（非 renderer-process 路径）；
  CLI 无 Cookie 罐时子进程仍然回问，浏览器回空串（行为与进程内一致）。
- **测试**：13 个协议单元测试（含随机垃圾不崩溃、超限拒绝）+ 8 个会话/
  端到端测试（本地回环 HTTP 服务器；重定向逐跳 Cookie 回调；连接失败
  不致命；进程无法启动报错；控制器级 Cookie 罐跨导航回送）。

**诚实边界（M3a）**：子资源（图片/CSS/字体/脚本）仍在渲染器子进程内直连抓取
（不带 Cookie）；无流式传输/缓存/连接复用；子进程仍能访问文件系统与网络
（安全沙箱是 M5）；`--network-process` 与 `--renderer-process` 不叠加
（后者由渲染器子进程自行抓取，CLI 会告警提示）。

## 备选方案

- **线程隔离**：不隔离地址空间，崩溃仍带崩整个浏览器；安全模型
  不成立。
- **外部 IPC 库（gRPC/Cap'n Proto/nng）**：消息模型过重、依赖过大；
  帧协议自研（约百行）即可，需要结构化 IDL 时再评估（M3 网络协议
  可能引入，届时另立 ADR）。
- **一次性跳到完整 Chromium 式多进程**：违反增量原则；M1 的价值是
  用最小的真实闭环（真实子进程、真实 IPC、真实渲染回传）把架构缝
  落地并测试，后续里程碑各自独立可验证。

## 后果

- 优点：进程级崩溃隔离从 M1 起真实存在（Renderer 崩溃不带走浏览器）；
  IPC/进程抽象跨平台且有测试；后续 Network/GPU 进程复用同一 Channel/
  Subprocess 与帧协议。
- 缺点：M1 每次加载 spawn 子进程有 ~10-20ms 进程启动开销（headless 与
  测试场景可忽略；M2 会话复用解决）；位图回传有 IPC 拷贝（M4 共享
  内存解决）；子进程仍共享引擎二进制（体积无变化，拆分是后续工作）。
