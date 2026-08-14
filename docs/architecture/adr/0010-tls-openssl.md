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
