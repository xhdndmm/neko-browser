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

## 未实现

- keep-alive 连接复用、HTTP/2、HTTP/3、brotli
- 超时/取消的完整生命周期管理

## 分层

```text
URL → HTTP → TLS → TCP → Socket
```

第三方网络库（OpenSSL/zlib）必须封装在自有接口之后；响应数据视为不可信输入。
