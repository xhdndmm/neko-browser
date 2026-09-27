# 架构决策记录 0010：HTTPS/TLS 采用 OpenSSL

- 状态：**Accepted**（2026-08）
- 决策者：架构组

## 背景

Phase 2 的 HTTP/1.1 客户端只支持 `http://`，`https://` 返回显式
NOT IMPLEMENTED（socket 层预留了传输缝）。真实网页几乎全部走 HTTPS，
TLS 是联网浏览器的基础能力。依赖政策 §5 明确：**TLS/加密必须用成熟实现**，
不得自研。

## 决策

- 使用 **OpenSSL 3.x**（`find_package(OpenSSL REQUIRED)`，与 zlib/libjpeg/
  FreeType/Qt 的"系统包"惯例一致；CI 三平台显式安装）。
- 新模块 `neko::network::TlsSocket`（`src/network/`）封装 OpenSSL：
  - `TlsSocket::Connect(host, port, TlsOptions)`：TCP 连接 + TLS 握手 +
    SNI + **证书与主机名校验**（系统信任库 + 可选附加信任锚）。
  - 只暴露 `Send` / `ReceiveAll` / `Close`，与 `Socket` 同构，HTTP 层通过
    模板统一处理 http/https 两种传输。
  - 强制 TLS ≥ 1.2；`SSL_CTX` 按连接创建（无全局可变状态）。
- `HttpGet` 支持 `https://`；`TlsOptions.extra_ca_cert_pem` 用于测试的
  本地自签名 CA（生产默认空，证书校验始终开启）。
- 证书校验失败（不受信 / 主机名不匹配 / 过期）返回 `ErrorCategory::kNetwork`，
  绝不静默降级为明文。

## 备选方案

- **mbedTLS**：更小、嵌入式友好，但 OpenSSL 是 CI 三平台最普遍的系统包，
  文档与工具链最成熟，且是依赖政策表里列出的首要候选。
- **BoringSSL / LibreSSL**：API 兼容但系统包覆盖面差，需 FetchContent 自建。
- **自研 TLS**：明确禁止（依赖政策 §5）。

## 许可证

- OpenSSL 采用 **Apache-2.0** 双许可（旧版 OpenSSL License + SSLeay）——
  宽松许可，与项目 Unlicense 兼容。

## 后果

- 优点：`https://` 端到端可用（example.com 手工验证）；证书校验默认开启；
  与既有 Socket 抽象同构，HTTP 层改动小。
- 缺点：新增一个系统包依赖；OpenSSL 状态机复杂，错误信息需要包装成可读
  文本；测试需要本地 TLS 服务器（自签名证书 + 附加信任锚）。

## 参考

- https://www.openssl.org/docs/man3.0/man3/SSL_connect.html
- https://www.openssl.org/docs/man3.0/man3/SSL_set1_host.html
- RFC 8446（TLS 1.3）/ RFC 5246（TLS 1.2）

## 修订 2026-09：信任库取自**运行主机**（rc2 发布故障）

- **症状**：rc2 的 Linux 产物（Ubuntu runner 打包）在用户机（Arch）上访问
  任何 `https://` 都失败：`TLS handshake / certificate verification failed for
  <host>: error:0A000086:SSL routines::certificate verify failed`。
- **根因**：产物捆绑了 runner 的 `libcrypto.so.3`，其编译期
  `OPENSSLDIR=/usr/lib/ssl`（Debian 系布局）。Arch 上只有 `/etc/ssl/certs`，
  于是 `SSL_CTX_set_default_verify_paths()` **返回 1（成功）而信任库里有 0 个
  锚点**（实测：宿主 libcrypto = 121 锚点；捆绑 libcrypto = 0 锚点且错误队列
  为空），握手必然校验失败。macOS/Windows 同理：Homebrew 的
  `/opt/homebrew/etc/openssl@3`、vcpkg 前缀在用户机上都不存在。
- **决策**：新增模块内 `system_trust_store`（`src/network/src/`），在
  **运行时**按运行主机的布局发现信任库（PEM 包 / hashed 目录 / 平台 store），
  逐个装进连接的 `X509_STORE`；**不再调用**
  `SSL_CTX_set_default_verify_paths()`。`SSL_CERT_FILE` / `SSL_CERT_DIR` 按
  OpenSSL 语义替换平台默认。锚点解析按候选列表记忆化（121 张证书解析约
  2.6 ms，复用后插入约 0.04 ms/连接），缓存不失效、重启生效——与浏览器对
  根证书的取舍一致。
- **备选**：
  - 启动脚本里设 `SSL_CERT_FILE`：把策略藏进环境变量、Windows 无效、用户
    自定义环境时行为不可预测。否决。
  - 随包自带 Mozilla CA 包（Chrome/Firefox 形态）：等于自建根证书计划
    （更新、审计、合规），留作后续工作。否决（暂）。
- **后果**：验证始终以**用户机器**的信任库为准；主机一个锚点都没有时明确
  失败关闭并提示安装 `ca-certificates` 或设置 `SSL_CERT_FILE`，绝不拿空信任库
  校验。测试：`tests/unit/network/network_test.cpp` 的 `TrustStoreTest.*`
  （PEM 包逐张装载、hashed 目录需真有条目、宿主信任库必须有锚点、空信任库
  失败关闭、环境变量信任库真的参与握手）。
- **已知边界**：macOS 只读 `/etc/ssl/cert.pem`，未接 Keychain；Windows 走
  `org.openssl.winstore://`（系统 ROOT 库），尚无运行期测试（CI 只编译）。
