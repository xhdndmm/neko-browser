# URL 设计

> 位置：`neko::url`（`src/url/`）

## 为什么存在

URL 是浏览器一切网络活动的地基。解析、规范化、相对解析、Origin 计算必须精确且
可测试。

## 语义（Phase 1 范围）

- WHATWG URL / RFC 3986 风格：
  - scheme（小写）、authority（userinfo@host:port）、path、query、fragment
  - host 小写、IPv6 字面量（`[::1]`）方括号、默认端口省略
  - 特殊 scheme（http/https/ws/wss/ftp）强制 authority；空 host 报错
  - 相对引用解析（RFC 3986 §5.2）+ dot-segment 移除（§5.2.4）
- 百分号编码/解码（unreserved 集：`A-Z a-z 0-9 -._~`）
- `Origin()`：scheme://host[:非默认端口]

## 未实现（Phase 1 范围外）

- IDNA（非 ASCII 域名）
- `file:` 特例语义、opaque path（非特殊 scheme 的完整规范化）
- 完整 IPv6 校验

## 测试

见 `tests/unit/url/url_test.cpp`（含 RFC 3986 §5.4.1 全部 Normal Examples）。
