# SECURITY

## 安全立场

**所有外部输入都是不可信的。** 浏览器引擎处理的 HTML、CSS、JavaScript、URL、
HTTP 响应、图片、字体、网络包、IPC 消息都可能来自恶意来源。

安全必须从架构早期就考虑，而不是等项目完成后再补。

## 报告安全漏洞

**请勿公开披露漏洞**。请发送邮件至项目维护者（联系信息见仓库主页），或通过
GitHub 私密的安全报告通道（Security → Report a vulnerability）。

请提供：

- 受影响的版本 / 提交
- 漏洞类型与影响
- 复现步骤（尽量最小化）
- 建议的修复方案（可选）

## 已知安全相关工作（路线）

- Origin / Same-Origin Policy（Phase 10）
- CORS / CSP（Phase 10）
- Cookie 安全（Secure / HttpOnly / SameSite）（Phase 10）
- TLS 证书校验（Phase 2，HTTPS）
- 权限系统与沙箱（Phase 10–12）
- 进程隔离（Phase 12）
- Parser 安全（持续）：深度嵌套、超大 token、UTF-8 畸形输入、整数溢出、
  内存耗尽（parser bomb）—— 从每个 parser 落地的第一天起就要防御。

## 开发者注意事项

- 所有 parser 必须有资源限制与畸形输入防御（见 AGENTS.md §67）。
- 所有解析错误按常规输入处理，不当作异常。
- 内存错误、数据竞争、UB 视为严重缺陷，必须修复（sanitizer 在 CI 中强制）。
- 不允许提交任何密钥、口令、token、证书私钥。
- 新增依赖必须经过 [dependency-policy.md](docs/development/dependency-policy.md)
  的安全评估（许可证、维护状况、历史漏洞）。

## 威胁模型（当前阶段）

Phase 0 尚无可执行的外部输入面（引擎未实现）。每个新模块落地时必须同步更新
本威胁模型文档。
