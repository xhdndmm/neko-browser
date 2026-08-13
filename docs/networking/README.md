# Networking 模块

> 状态：**Partial**（Phase 2）

## 已实现

- TCP Socket 抽象（POSIX；getaddrinfo 解析、连接超时、完整收发）
- HTTP/1.1 GET：请求构建、响应解析（状态行/头/体）、Content-Length、
  chunked 传输、重定向跟随（301/302/303/307/308）
- 显式错误：https（TLS 未实现）、未知 content-encoding

## 未实现

- HTTPS/TLS（架构已预留：Socket::Connect 为传输接缝）
- keep-alive 连接复用、gzip/deflate 压缩、HTTP/2、HTTP/3
- 超时/取消的完整生命周期管理

## 分层

```text
URL → HTTP → TLS(计划) → TCP → Socket
```

第三方网络库必须封装在自有接口之后；响应数据视为不可信输入。
