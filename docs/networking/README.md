# Networking 模块

> 状态：**Implemented**（Phase 2 核心 + HTTPS + 压缩）

## 已实现

- TCP Socket 抽象（POSIX；getaddrinfo 解析、连接超时、完整收发）
- HTTP/1.1 GET：请求构建、响应解析（状态行/头/体）、Content-Length、
  chunked 传输、重定向跟随（301/302/303/307/308）
- **HTTPS/TLS**：`TlsSocket` 封装 OpenSSL（ADR 0010）——证书+主机名校验、
  SNI、TLS≥1.2；`HttpGet` 对 https:// 自动启用
- **gzip/deflate**：`compression` 封装 zlib，RFC 7231 内容编码解码
  （链式编码、raw deflate 兼容、64 MiB 输出上限）
- **`data:` URL（RFC 2397）**：`HttpGet` 在打开 socket 之前就地解码
  （`DecodeDataUrl`）——元数据段（含 `;base64` 标记，大小写不敏感）与逗号后的
  负载、负载百分号解码、缺省 `text/plain;charset=US-ASCII`、无 padding 与
  空白容忍、非法字符/尾部置位拒绝；所有子资源（样式表、脚本、@font-face、
  图片、`fetch()`）都走同一入口，因此内联 base64 资源不需要网络

## 未实现

- keep-alive 连接复用、HTTP/2、HTTP/3、brotli
- 超时/取消的完整生命周期管理

## 分层

```text
URL → HTTP → TLS → TCP → Socket
```

第三方网络库（OpenSSL/zlib）必须封装在自有接口之后；响应数据视为不可信输入。
