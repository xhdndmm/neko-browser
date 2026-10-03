# 架构决策记录 0022：HTTP 连接复用（keep-alive 池）+ 脚本并行预取

- 状态：**Accepted**（2026-10）
- 决策者：架构组
- 关联：ADR 0010（TLS/OpenSSL）、ADR 0018（DNS）、ADR 0020（DOM 归 worker
  线程独占）、AGENTS.md §10/§25/§34/§56/§65

## 背景

用户报告「访问 bilibili.com 很慢」。先建可复现回路再动手，测量结果如下
（release + LTO，`neko_browser --headless --url https://www.bilibili.com/
--dump-dom`，同机同网络）：

| 指标 | 实测 |
| --- | --- |
| 墙钟 | 16.9 / 24.2 / 25.4 / 26.1 / 28.6 s（抖动大） |
| CPU | `user 3.5s + sys 0.4s` —— **84% 时间在等 I/O** |
| HTTP 请求数 | 183（字体 108 / 9.6 MB、图片 32 / 7.9 MB、JS 7 / 2.3 MB） |
| 逐请求耗时之和 | 77.2 s（其中 `tls_connect` 41.5 s、`transfer` 35.6 s） |
| 单次新建连接成本 | `curl` 对照：TCP 1.4 ms，**TLS 握手 0.28–0.40 s** |

阶段分解（插桩）：文档 0.4–1.1 s、CSS 1.3 s、字体 2.2 s、**脚本 11.1–19.7 s**、
图片 0.6 s、加载后泵 4.3 s。

三个结构性问题互相放大：

1. **每个请求都重新握手**：`http.cpp` 硬编码 `Connection: close`，没有连接池、
   没有 TLS 会话复用，`src/network/` 下**没有 HTTP 缓存**。183 个请求 =
   183 次完整 TCP+TLS 握手（握手占逐请求总耗时的 54%）。
2. **经典 `<script>` 是「取一个→执行一个」**：`page_scripts.cpp` 在文档序循环里
   内联 `fetch()`，没有预扫描/并行预取。7 个脚本 = 7 次串行往返（≈4.1 s）。
3. **XHR 传输是同步阻塞的**（`xhr_binding.cpp`，已文档化的近似）：bilibili 首页
   用 XHR 拉 33 个图标/接口，实测**间隔恰好 ≈0.51 s、完全无重叠**（27 个请求的
   「与上一个的间隔 == 自身耗时」），单这一段就占 19.6 s 跨度中的 12.9 s。

决定性对照：同一主机 `i0.hdslb.com` 的同类 JSON，**经线程池并行发出时每个
32–99 ms（24 个共 0.5 s）**，**经同步 XHR 发出时每个 ≈510 ms 且严格串行**。
差别只在「引擎是否重叠它们」，不在服务器。

本 ADR 只处理 1 和 2（P0）。第 3 项（真异步 XHR/fetch）是 P1，见下文「未解决」。

## 决策

### 1. `network::HttpClient`：按 origin 复用的 keep-alive 连接池

新增 `HttpClient`（`http_client.h`，实现在 `http.cpp`，与线协议代码同处一个 TU）：

- **连接池**：`{scheme, host, port, timeout, proxy, 信任锚哈希}` → 空闲连接栈。
  信任锚/代理/超时参与键，避免「测试的额外 CA 被未请求它的请求静默复用」以及
  「复用了握手的超时」。每 origin 最多保留 8 条空闲连接（`kMaxIdlePerOrigin`），
  空闲 60 s 后不再复用。
- **复用条件**：响应 framing 是有界的（Content-Length / chunked / 无体），
  且没有 `Connection: close`；连接上无残留字节（本客户端从不流水线，残留字节
  说明对端 framing 不可信）。
- **健康探测**：复用前用非消费式 `HasPendingInput()`（TLS 版先看 `SSL_pending`）
  判断对端是否已关闭，避免把一次请求花在死连接上。
- **失败重试**：只有在「**复用的**连接、且在收到**任何一个响应字节之前**失败」
  时，才对新建连接重发一次（GET 幂等）。收到一半响应、或新建连接失败，都是
  真实错误，不重试。
- **进程级入口**：`SharedHttpClient()`（函数内静态）供 `HttpGet()` 使用，因此
  所有既有调用方（样式表/脚本/图片/字体/fetch/XHR）**无需改动**即可获得复用；
  需要隔离池的嵌入方可自建 `HttpClient`。
- **连接不再主动关闭**：请求不再发送 `Connection: close`。

### 2. `Socket::ReadSome` / `TlsSocket::ReadSome`：单次读取原语

实现连接池时发现一个既有缺陷：`Receive(max_bytes, timeout)` 的语义是
「**读到 max_bytes 或超时**」，即对端不关闭就一定等到超时。既有代码能与
`Connection: close` 配合工作，正是因为对端会关闭。改成常连后，每次响应读取
都会等满 10 s（新增测试首次运行：3 个请求 30 s）。

因此新增单次读取原语 `ReadSome()`（有数据立即返回，不等待填满 `max_bytes`），
HTTP 的 `ResponseReader` 改用它：framing 感知的读取方**绝不能等待属于下一条
消息的字节**。这也是持久连接可用的前提。

### 3. `RunPageScripts`：脚本体并行预取、执行仍按文档序

`page_scripts.cpp` 在三个执行 pass 之前增加预取阶段：

- 在**调用线程**上遍历 `<script>`、解析绝对 URL、读取内联体（DOM 归 worker
  线程独占，ADR 0020；**只有网络工作进池**）。
- 把每个唯一 URL 提交到调用方传入的 `base::ThreadPool&`（新增必填参数——
  线程归属显式化，AGENTS.md §10）；同一 URL 只取一次（去重）。
- 三个 pass 的**执行顺序、失败语义、错误文案完全不变**；模块脚本仍走 ES
  module loader（不预取），拿不到预取结果的路径回退为内联 fetch。

## 备选方案

- **只在 `Socket` 里加连接池**：层次错误。复用是 HTTP 语义（framing/header
  决定连接是否可留），TLS/TCP 层看不到这些信息。
- **立刻上 HTTP/2 多路复用**：收益更大但代价也大（帧层、HPACK、流控、服务端
  协商），且不影响 keep-alive 池的结论；作为后续独立演进项。
- **把整个「取脚本 + 执行」都丢进池线程**：违反 ADR 0020（DOM 线程独占），
  直接引入数据竞争。
- **加 per-origin 并发上限 + 排队**：更接近真实浏览器（每 origin 6 条），但需要
  条件变量队列，且与嵌套请求（重定向、子资源）组合有死锁风险。本轮不做，
  留作后续（见「未解决」）。
- **只做脚本预取、不做连接池**：收益受限，因为每次预取仍是新握手（本机
  0.3–0.5 s）。

## 后果

同机同网络复测（release）：

| 指标 | 之前 | 之后 |
| --- | --- | --- |
| HTTP 请求数 | 183 | 183 |
| 新建 TCP+TLS 连接 | **183** | **54**（−70%） |
| 走热连接的请求 | 0 | **129** |
| 墙钟（中位数，各 9 次采样） | ≈18.3 s | ≈11.4 s |

- 优点：握手成本从「每请求一次」降为「每段并发一批」；脚本体并发抓取；
  既有调用方零改动；`HttpClient::Stats` 为后续性能工作提供可测口径。
- 缺点/代价：连接生命周期变长（进程内最多 8×origin 条空闲连接，60 s 后
  回收）；对端关闭空闲连接时多一次「探测 + 重试」往返；`Connection: close`
  的移除使「无 framing 且不关闭」的服务器依赖读超时兜底（规范要求这类响应
  必须靠关闭定界，风险与真实浏览器一致）。
- **未解决（本轮范围外，P1）**：
  - **同步 XHR 传输**（真异步化）——这是现在最大的单项等待（诊断中 12.9 s 量级）；
  - **HTTP 缓存**（内存/磁盘）——重复访问仍全量重下；
  - **TLS 会话复用**——有了连接池后收益已很小，未做；
  - **per-origin 并发上限 + 排队**——并发突发（如 108 个字体子集）仍会开较多连接；
  - **`@font-face` 未按 `unicode-range` 过滤**——bilibili 的 108 个子集 9.6 MB
    会被全量下载（并行，约 2.2 s，但纯属浪费）；
  - **`discarded=27`**：其中包含「响应要求 close」「lane 满」与少量过期/死亡
    连接，尚未细分统计。

## 验证

- `tests/unit/network/http_client_test.cpp`（8 个用例，本地 keep-alive 服务器 +
  连接计数）：复用（3 请求 1 连接）、chunked 复用、`Connection: close` 不复用、
  对端静默关闭时重试一次且请求只发一次、残留字节不复用、`CloseIdleConnections`、
  不同客户端不共享池、非法 scheme。
  **红性验证**：把「可复用」判定强制为 false 后，
  `ReusesOneConnectionForSequentialRequests` 与
  `ReusesConnectionForChunkedResponses` 失败（其余 6 个不受影响）。
- `tests/unit/browser/page_scripts_test.cpp`（4 个用例）：三个脚本体同时 in-flight
  且按文档序执行、同一 URL 只取一次但执行两次、抓取失败仍报告错误且后续脚本
  照常运行、内联脚本不触发抓取。**红性验证**：禁用预取后并发断言读到 1（串行）。
- 既有套件：`ctest --preset debug` 全绿（1435 个用例），包含既有 HTTP/TLS/
  代理用例。其中 `TestConnectProxy` 的析构原本依赖「客户端会关闭连接」——
  连接池上线后该假设不成立（测试挂死），已改为析构时 `shutdown()` 活跃连接：
  **测试服务器不得依赖客户端主动断开**。

## 修订 1（2026-10）：排查 `www.baidu.com` 握手失败时补的三点

用户报 `http://baidu.com` 失败并显示 `TLS handshake / certificate verification
failed for www.baidu.com: ... unexpected eof while reading`。复现结论：**这条报错 =
握手期间 TCP 对端关闭**，与证书无关——用一个「收到 CONNECT 后立刻关闭」的本地代理
可一字不差复现该 OpenSSL 错误码（`0A000126`），而「连 CONNECT 都不回」则报
`recv failed`。本机的 `http://baidu.com`（经 302 → https）、`https://www.baidu.com`、
`--network-process`、`--renderer-process`、GUI（`neko_gui_screenshot`）全部正常，
`tls_socket.cpp` 的改动是 +76/−0（握手/校验/SNI 一行未动），故该报错由网络路径
（代理/VPN/中间盒）造成，而非本引擎。由此补了三处：

1. **错误文案不再误导**：`SSL_R_UNEXPECTED_EOF_WHILE_READING` 单独分支，明确
   「未收到证书、请检查网络路径或 HTTPS 代理」，不再归入证书校验失败。
   回归用例：`TlsTest.PeerClosingDuringTheHandshakeIsReportedAsAPeerClose`。
2. **连接身份包含「路径」**：池键改用 `TlsOptionsWithEnvironmentProxy` 解析后的
   *生效* 代理，而不是只看显式 `options.proxy_host`。否则「直连建立的连接会被
   代理请求复用」，而 NO_PROXY 主机也可能悄悄走代理。回归用例：
   `TlsTest.DoesNotReuseADirectConnectionWhenAProxyAppears`（用 keep-alive 的本地
   TLS 服务器 + 单次 CONNECT 代理，断言第二个请求真的走了代理且新建了连接）。
3. **SIGPIPE 由网络模块接管默认处置**：`EnsureInitialized()` 在 SIGPIPE 仍为
   `SIG_DFL` 时置为 `SIG_IGN`（嵌入方自己装的 handler 不动），`SendBytes` 另加
   `MSG_NOSIGNAL`。原因：TLS 经 OpenSSL 的 socket BIO 写入（`write(2)`，**无法**
   逐调用屏蔽信号），而连接池让「写到对端已关闭的连接」从边角情形变成常态。
   此前只有 `browser/main.cpp` / `ui/main.cpp` 自行 `signal(SIGPIPE, SIG_IGN)`，
   因此测试二进制与嵌入方会被 SIGPIPE **直接杀掉**（新用例首次运行即如此）。
   回归用例：`SocketTest.NetworkIoDoesNotLeaveSigpipeAtItsDefaultDisposition`。

   顺带发现：`TestTlsServer` 原来一次只服务一条连接，无法表达「两个连接同时存在」，
   已改为每连接一线程并在析构时 shutdown 活跃连接（与 `TestConnectProxy` 一致）：
   **测试服务器不得依赖客户端主动断开，也不得假设同时只有一条连接。**
