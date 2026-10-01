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
| TLS | 已落地 | 证书+主机名校验（默认全量，测试含不受信/主机名不匹配拒绝）；信任锚取自**运行主机**的 CA 库，空信任库失败关闭 |
| JavaScript runtime | 已落地 (Partial) | QuickJS 沙箱：无 std/os 模块、执行时限中断、内存上限（均有测试） |
| JavaScript | Phase 8+ | 沙箱逃逸、原型污染 |
| IPC | Phase 12 | 消息伪造、越权 |

## 长期安全子系统

- **Origin / Same-Origin Policy**：所有跨源交互的基石 —— **M1 已落地**（见下）
- **CORS / CSP**：内容与请求策略 —— 未开始
- **Cookie 安全**：Secure / HttpOnly / SameSite —— 部分（存储已实现，强制未做）
- **TLS 证书验证**：默认全量校验，禁止静默降级；信任库在运行时按运行主机
  发现（ADR 0010 修订），发现不到任何锚点时失败关闭 —— 已落地
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

## 密码管理威胁模型（2026-10）

已保存的登录凭据存放在 `<profile>/logins.dat`：magic + 12 字节随机
nonce + 16 字节 GCM tag + AES-256-GCM 密文，明文（行式、字段百分号
编码）从不落盘。密钥是 `<profile>/login_key.bin`（32 随机字节，POSIX
上 0600；Windows 无等价保护）。

**能防**：单独拷贝 `logins.dat` 的泄露路径 —— 备份/云同步目录、以及
无权读取密钥文件的其它本机账户（读到的只是密文）。

**不能防**（如实记录）：

- 以该用户身份运行的本机恶意软件（密钥就在旁边，无主密码 —— 本
  里程碑刻意不引入主密码，避免半吊子的"伪保护"）；
- 有权读取整个 profile 目录的任何进程/用户；
- 篡改：GCM tag 保证完整性校验，篡改会以解密失败暴露（Load 报错且
  拒绝覆盖），但密钥一旦被替换，旧数据将不可读（此时拒绝 Save，
  保护可恢复数据）；
- 内存转储、键盘记录等本地攻击。

**行为约束**：凭据只按精确 origin 保存/填充（opaque origin 如
file:/data: 永不参与）；不做跨 origin 建议、不做表单嵌套域的宽松
匹配；UI 只显示 origin/username，从不回显密码。后续里程碑可把密钥
移入操作系统钥匙串（libsecret / DPAPI / Keychain）。

## 当前状态（Phases 0–12）

- Phase 0–9：URL/HTTP/HTML/CSS 等 parser 已按基线实现边界检查。
- 内容解析：PNG 解码器（chunk 长度/CRC/尺寸上限/位深组合校验）、
  PDF 解析器（xref/对象/流长度与溢出检查）均含畸形输入测试。
- Cookie 存储（RFC 6265 子集）：字段经百分号编码转义，防止注入；
  域/路径匹配已实现。**已知限制**：未做 PSL 校验与 SameSite 强制实施，
  跨域 Cookie 语义可能过宽 —— 已标注为限制，后续里程碑收紧。
- HTTP 内容编码（gzip/deflate）：解压输出设 64 MiB 上限，防解压炸弹；
  截断/损坏流返回解析错误而非损坏内容。
- Origin 模型（M1）：`neko::security::Origin` 提供三元组 origin 与同源判定，
  浏览器控制器在每次加载后记录页面 origin（见上方专节）。
- 密码管理（Phase 10 预览）：`storage::PasswordStore`（AES-256-GCM + 机器
  本地密钥文件）保存登录凭据；提交含非空密码的表单时捕获为 pending，
  用户在提示条确认后保存；页面加载时按精确 origin 自动填充第一个密码
  表单（不覆盖已有值，不自动提交）。威胁模型与限制见上方专节。
  **未实现**：主密码、OS 钥匙串、renderer 进程模式下的填充（DOM 在
  子进程）、跨设备同步。
- JavaScript：QuickJS 沙箱（无 std/os 模块、执行时限、内存上限）；
  页面脚本通过每页独立 runtime + DOM 绑定执行，脚本错误记录到 console。
- TLS（Phase 2）：OpenSSL 封装（ADR 0010），默认全量证书校验 + 主机名校验，
  拒绝不受信/主机名不匹配的服务器（有测试），不提供静默降级路径。
- JavaScript runtime（Phase 8 M1）：QuickJS 沙箱化 —— 不编译 `std`/`os`
  模块（无文件/进程/网络能力），仅自有 `console` 绑定；默认执行时限
  10 秒 + 内存上限 128 MiB（有中断与内存限制测试）。**已知限制**：
  无 Origin 隔离（每个 engine 独立全局域）。
- 多进程（Phase 12 M1+M2）：Renderer 子进程在独立地址空间运行页面管线，
  子进程崩溃不带走浏览器；**尚无沙箱与站点隔离**，子进程仍具备完整进程权限。
- 未开始：SOP 实施（fetch/XHR 需 CORS）、CORS/CSP、SameSite/PSL 强制、
  沙箱、权限系统、Network/GPU 进程隔离。
