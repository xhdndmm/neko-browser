# ADR 0018: 内置 DNS 解析器（DNS 优先，getaddrinfo 回退）

- 状态：Accepted
- 日期：2026-09
- 相关：`src/network/include/neko/network/dns.h`、`src/network/src/dns.cpp`、
  `Socket::Connect`

## 背景

`Socket::Connect` 一直直接用 `getaddrinfo`：

- 解析顺序、缓存、超时都交给系统 libc，引擎无法控制；
- 没有测试手段（单元测试无法注入解析结果）；
- 每次连接都要走一次系统解析，没有 TTL 缓存（真实站点一个页面几十个请求
  打到同一域名时开销明显）；
- DNS 是浏览器必须自己掌握的能力（Chrome/Firefox 都内置解析器）。

## 决策

在 `neko::network` 实现**项目自有的 DNS 客户端**（RFC 1035 报文、RFC 3596
AAAA）：

1. 手工实现报文编解码（与其它解析器一致：全部边界检查、压缩指针有跳数与
   访问界、坏报文返回错误而不是崩溃）。不引入 c-ares 等解析库。
2. 解析顺序：**数字地址直返 → /etc/hosts → 内置 DNS（UDP）→
   getaddrinfo 回退**。回退保证 NSS-only 环境（mDNS、LDAP、VPN 插件）仍然
   可用；内置 DNS 让引擎掌握缓存与超时。
3. 缓存：按主机名缓存 A/AAAA 结果，TTL 取应答中的最小值并夹取
   `[min_ttl, max_ttl]`（默认 1s–1h）；TTL=0 不缓存。**负缓存**：解析失败
   （NXDOMAIN/SERVFAIL/超时/畸形报文）记住 10s，避免每次请求都付超时，
   也是回退到 getaddrinfo 的路径不会重复付费的原因。
4. 防伪造：随机查询 id + 校验响应 id/QR 位 + 连接到服务器地址的 UDP
   socket（内核过滤其它源的报文）+ 问题段匹配语义。
5. CNAME：链式跟随（上限 8 跳，成环不会挂死）。
6. 配置：`DnsResolver::Options`（服务器列表、`/etc/resolv.conf` 路径、
   `/etc/hosts` 路径、端口、超时、重试、缓存上限、TTL 夹取、负缓存时长、
   可注入时钟）。默认实例 `DnsResolver::Default()` 供 `Socket::Connect`
   使用；测试用真实 UDP 服务器 + 临时 hosts 文件 + 注入时钟，不依赖外网。

## 备选方案

- **继续只用 getaddrinfo**：实现最省，但无法缓存/超时控制/测试，放弃。
- **c-ares 等第三方库**：成熟但引入依赖与回调式异步模型；当前需要的是一个
  小而可测的解析器 + 缓存，自研可控（并且报文解析是本项目一贯强项）。
- **DoH/DoT**：默认解析器当然可以是 HTTPS，但需要先有 DNS 才能连（引导
  问题），且对本机/企业 DNS 兼容性差 → 列为未来工作（`Options.servers`
  支持 `https://` 形式时可后接）。

## 影响

- `Socket::Connect` 现在先走解析器；数字地址不会重复走 getaddrinfo。
- 线程模型不变：`Resolve()` 线程安全（缓存加锁），每次调用自带 UDP
  事务，浏览器 fetch 池上的并发解析互不共享 socket。
- 已实现限制（诚实标注，写在头文件与兼容性矩阵）：仅 A/AAAA/CNAME，
  无 DNSSEC、无 EDNS0、截断应答不做 TCP 重试、resolv.conf 不区分网卡。
- Windows 与 POSIX 共用同一份 UDP 查询实现（Winsock/POSIX 差异集中在模块内
  `src/network/src/socket_platform.{h,cpp}`）；Windows 上 `/etc/resolv.conf` 与
  `/etc/hosts` 不存在，解析器回退到公共解析器列表，`Socket::Connect` 仍有
  getaddrinfo（系统解析器）回退。网络测试目前仍为 POSIX 守卫，Windows 路径由
  CI 编译验证、未在 CI 运行。
