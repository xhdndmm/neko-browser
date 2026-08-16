# 架构决策记录 0016：多进程架构（进程模型 + IPC 设计 + 迁移路线）

- 状态：**Accepted**（2026-08，M1 已实现；M2+ 路线见下）
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

- **M1（本 ADR 交付）**：IPC 基础设施 + Renderer 子进程跑完整页面管线
  （fetch→parse→style→layout→rasterize），经 IPC 返回位图 + DOM 文本；
  子进程入口是同一二进制 `--renderer-child`（Chromium 也是独立二进制，
  此处复用 CLI 二进制作为过渡，后续拆分独立 `neko_renderer` 可执行文件）；
  CLI `--renderer-process` 走 RendererHost 加载 + 截图/dump-dom；单元测试
  （帧协议、管道往返、协议编解码）+ 端到端集成测试（真实子进程加载
  本地夹具页面并回传位图）。GUI 尚未接入（M2）。
- **M2**：GUI/BrowserController 经 RendererHost 加载页面（每站点子进程），
  崩溃隔离与重建、渲染帧按需请求。
- **M3**：Network 进程（HTTP/TLS/DNS 搬出 Browser，Renderer 与 Network
  经 Browser 中转或直连）；cookie 裁决留在 Browser。
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
