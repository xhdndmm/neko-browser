# Networking 模块

> 状态：**Implemented**（Phase 2 核心 + HTTPS + 压缩）

## 已实现

- TCP Socket 抽象（POSIX + Windows/Winsock；平台差异集中在模块内
  `src/socket_platform.{h,cpp}`；连接超时、完整收发）
- **DNS 客户端（ADR 0018）**：自研 RFC 1035 报文编解码（A/AAAA/CNAME、
  压缩指针、边界检查）、TTL 缓存 + 负缓存、`/etc/hosts`、
  `/etc/resolv.conf` 服务器列表、随机 id 防伪造、CNAME 链跟随；
  `Socket::Connect` 顺序 = 数字地址 → hosts → 内置 DNS → getaddrinfo 回退
- HTTP/1.1 GET：请求构建、响应解析（状态行/头/体）、Content-Length、
  chunked 传输、重定向跟随（301/302/303/307/308）
- **HTTPS/TLS**：`TlsSocket` 封装 OpenSSL（ADR 0010）——证书+主机名校验、
  SNI、TLS≥1.2；`HttpGet` 对 https:// 自动启用
- **信任库发现**（`src/system_trust_store.{h,cpp}`）：按**运行主机**的 CA 布局
  定位信任锚——PEM 包（Debian/Ubuntu/Arch 的 `/etc/ssl/certs/ca-certificates.crt`、
  Fedora/RHEL 的 `/etc/pki/...`、openSUSE/Alpine/BSD 的 `/etc/ssl/*.pem`、
  macOS 的 `/etc/ssl/cert.pem`）、hashed 目录（仅在确实含 `c_rehash` 条目时
  计入）、平台 store（Windows `org.openssl.winstore://` 系统 ROOT 库）；
  `SSL_CERT_FILE` / `SSL_CERT_DIR` 按 OpenSSL 语义**替换**平台默认。一个锚点
  都没有时**失败关闭**并给出可操作报错（安装 `ca-certificates` 或设置
  `SSL_CERT_FILE`），绝不拿空信任库去校验。**不用**
  `SSL_CTX_set_default_verify_paths()`：它读的是 libcrypto **编译期**路径
  （打包产物 = 构建 runner 的布局），且路径不存在时仍然返回成功
- **gzip/deflate**：`compression` 封装 zlib，RFC 7231 内容编码解码
  （链式编码、raw deflate 兼容、64 MiB 输出上限）
- **`data:` URL（RFC 2397）**：`HttpGet` 在打开 socket 之前就地解码
  （`DecodeDataUrl`）——元数据段（含 `;base64` 标记，大小写不敏感）与逗号后的
  负载、负载百分号解码、缺省 `text/plain;charset=US-ASCII`、无 padding 与
  空白容忍、非法字符/尾部置位拒绝；所有子资源（样式表、脚本、@font-face、
  图片、`fetch()`）都走同一入口，因此内联 base64 资源不需要网络
- **keep-alive 连接复用（ADR 0022）**：`network::HttpClient` 按 origin
  （scheme+host+port+超时+代理+信任锚）持有空闲连接，`HttpGet()` 经进程级
  `SharedHttpClient()` 自动复用（每 origin 最多 8 条空闲、60 s 过期）；
  复用条件是响应 framing 有界且无 `Connection: close`、连接上无残留字节；
  复用前用非消费式 `HasPendingInput()` 确认对端未关闭；失败重发仅限
  「复用的连接、收到任何响应字节之前」（GET 幂等）。实现细节：
  `Socket/TlsSocket::ReadSome()` 单次读取原语（有数据立即返回，
  不等待填满缓冲区）——framing 感知的读取方绝不能等待属于下一条消息的字节。
  实测 bilibili.com：183 个请求从 183 次新建 TCP+TLS 降到 54 次，129 个走热连接。
  另外脚本体现在**并行预取、按文档序执行**（`RunPageScripts`）

### 连接身份与 SIGPIPE（ADR 0022 修订 1）

- 连接身份 = origin + 超时 + **生效的**代理（显式或 `HTTPS_PROXY`/`NO_PROXY` 解析
  结果）+ 信任锚。只按 origin 分池会让「直连建立的连接被代理请求复用」（反之亦然）。
- SIGPIPE：`platform::EnsureInitialized()` 在 SIGPIPE 仍为 `SIG_DFL` 时置为
  `SIG_IGN`（不覆盖嵌入方 handler），`SendBytes` 另加 `MSG_NOSIGNAL`。TLS 经
  OpenSSL socket BIO（`write(2)`）无法逐调用屏蔽，而连接池让「写对端已关闭的连接」
  成为常态；没有这条保证，测试/嵌入方会被 SIGPIPE 杀掉。
- 握手期对端关闭（代理不再转发、中间盒重置）单独报错：**未收到证书**，提示检查
  网络路径或 HTTPS 代理——不再误报为证书校验失败。

## 未实现

- HTTP/2、HTTP/3、brotli
- per-origin 并发上限与排队（并发突发仍会一次开多条连接）、TLS 会话复用
- HTTP 缓存（内存/磁盘）：重复访问仍全量重下
- 超时/取消的完整生命周期管理
- 自有根证书计划（root store）：信任锚完全来自运行主机，macOS 未接 Keychain
  （只读 `/etc/ssl/cert.pem`），Windows 的 ROOT 库尚无运行期测试
- Windows 网络路径由 CI 编译验证，但网络测试仍是 POSIX 守卫（未在 Windows 运行）；
  DNS 服务器列表在 Windows 上回退到公共解析器（`/etc/resolv.conf` 不存在）

## 分层

```text
URL → HTTP → TLS → TCP → Socket
```

第三方网络库（OpenSSL/zlib）必须封装在自有接口之后；响应数据视为不可信输入。
