# Networking 模块

> 状态：**Not Started**（计划 Phase 2）

## 职责

网络栈分层：

```text
URL → HTTP → TLS → TCP → Socket
```

- Socket：TCP/UDP/DNS
- HTTP/1.1：请求/响应/头/体、chunked、keep-alive、重定向、压缩
- HTTPS：TLS 抽象层（封装 OpenSSL/mbedTLS/BoringSSL），证书校验
- 后续：HTTP/2、HTTP/3/QUIC

## 约束

- 第三方网络库必须封装在项目自有接口之后，核心引擎不得直接依赖。
- 所有响应数据视为不可信输入。
