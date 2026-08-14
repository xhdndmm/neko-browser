# 安全模型

> 原则：**所有外部输入不可信。**
> 本文档是威胁模型与安全设计的总纲，随实现阶段持续更新。

## 攻击面（按实现阶段展开）

| 输入面 | 实现阶段 | 风险 |
| --- | --- | --- |
| HTML | Phase 3 | parser bomb、深度嵌套、实体膨胀 |
| CSS | Phase 4 | 巨型选择器、递归 |
| URL | Phase 1 | 解析歧义、SSRF、凭据泄露 |
| HTTP 响应 | Phase 2 | 头注入、分块歧义、解压炸弹 |
| TLS | Phase 2 | 证书校验失败 |
| 图片/字体 | Phase 5–6 | 解码器漏洞 |
| PNG/JPEG | 已落地 | 长度/溢出/超大尺寸边界检查（有测试） |
| GIF | 已落地 | LZW 码宽/码表/输出长度/色表索引边界检查（有测试） |
| PDF | 已落地 (Partial) | 畸形 xref/对象/流（长度与溢出检查） |
| Cookie 存储 | 已落地 | 域/路径匹配、注入转义（百分号编码） |
| HTTP 压缩 | 已落地 | 64 MiB 解压输出上限（zip-bomb 防护） |
| TLS | 已落地 | 证书+主机名校验（默认全量，测试含不受信/主机名不匹配拒绝） |
| JavaScript runtime | 已落地 (Partial) | QuickJS 沙箱：无 std/os 模块、执行时限中断、内存上限（均有测试） |
| JavaScript | Phase 8+ | 沙箱逃逸、原型污染 |
| IPC | Phase 12 | 消息伪造、越权 |

## 长期安全子系统

- **Origin / Same-Origin Policy**：所有跨源交互的基石 —— **M1 已落地**（见下）
- **CORS / CSP**：内容与请求策略 —— 未开始
- **Cookie 安全**：Secure / HttpOnly / SameSite —— 部分（存储已实现，强制未做）
- **TLS 证书验证**：默认全量校验，禁止静默降级 —— 已落地
- **权限系统**：最小权限 —— 未开始
- **沙箱 / 进程隔离**：多进程阶段的纵深防御 —— 未开始
- **导航与下载安全**：拦截恶意下载与钓鱼导航 —— 未开始

## Origin / Same-Origin Policy（Phase 10 M1）

`neko::security::Origin`（`src/security`）实现了 origin 模型：

- **定义**：scheme + host + effective port（显式端口，缺省用 scheme 默认值）。
- **同源判定**：`Origin::IsSameOrigin` —— 三元组完全一致才算同源；
  不透明 origin（data: 等非特殊 scheme、file:）永不与任何 origin 同源
  （包括自身），序列化为 `"null"`。
- **接入**：每个标签页在加载后记录当前页面 origin（`Tab::origin` /
  `TabSnapshot::origin`），供后续 SOP 实施使用。
- **测试**：8 个单元测试（同源/跨 host/跨 scheme/显式端口/默认端口等价/
  序列化/不透明）+ 1 个浏览器集成测试（origin 记录与快照暴露）。
- **未实现（诚实标注）**：SOP 在网络读取上的实施（fetch/XHR 需 CORS）、
  CORS 头解析与预检、CSP、secure context、权限系统 —— 均为后续里程碑。
  经典 `<script>`/`<img>` 跨源加载在浏览器中是允许的（无需 CORS），
  本引擎目前同样允许，与规范一致。

## Parser 安全基线（每个 parser 落地即生效）

- 深度嵌套限制
- 超大 token / 文档限制
- 整数溢出防护
- 畸形 UTF-8 处理
- 内存分配上限（防 parser bomb）
- CPU 消耗上限（防膨胀算法）

## 开发者义务

- 新增解析/反序列化代码必须考虑恶意输入。
- 安全修复不得以"早期阶段"为由推迟。
- 代码评审中，安全是硬性检查项。

## 当前状态（Phases 0–8）

- Phase 0–6：URL/HTTP/HTML/CSS 等 parser 已按基线实现边界检查。
- 内容解析：PNG 解码器（chunk 长度/CRC/尺寸上限/位深组合校验）、
  PDF 解析器（xref/对象/流长度与溢出检查）均含畸形输入测试。
- Cookie 存储（RFC 6265 子集）：字段经百分号编码转义，防止注入；
  域/路径匹配已实现。**已知限制**：未做 PSL 校验与 SameSite 强制实施，
  跨域 Cookie 语义可能过宽 —— 已标注为限制，后续里程碑收紧。
- HTTP 内容编码（gzip/deflate）：解压输出设 64 MiB 上限，防解压炸弹；
  截断/损坏流返回解析错误而非损坏内容。
- Origin 模型（M1）：`neko::security::Origin` 提供三元组 origin 与同源判定，
  浏览器控制器在每次加载后记录页面 origin（见上方专节）。
- JavaScript：QuickJS 沙箱（无 std/os 模块、执行时限、内存上限）；
  页面脚本通过每页独立 runtime + DOM 绑定执行，脚本错误记录到 console。
- TLS（Phase 2）：OpenSSL 封装（ADR 0010），默认全量证书校验 + 主机名校验，
  拒绝不受信/主机名不匹配的服务器（有测试），不提供静默降级路径。
- JavaScript runtime（Phase 8 M1）：QuickJS 沙箱化 —— 不编译 `std`/`os`
  模块（无文件/进程/网络能力），仅自有 `console` 绑定；默认执行时限
  10 秒 + 内存上限 128 MiB（有中断与内存限制测试）。**已知限制**：
  尚无 DOM 绑定、无 Origin 隔离（每个 engine 独立全局域）。
- 未开始：Origin/SOP/CORS/CSP、沙箱、权限、进程隔离。
