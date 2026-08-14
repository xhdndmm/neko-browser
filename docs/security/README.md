# Security 模块

> 状态：**Partial**（Phase 10 M1：Origin / 同源判定已落地；SOP 实施、CORS、CSP
> 未开始）。

## 现状（M1）

- `neko::security::Origin`（`src/security`）：scheme + host + effective port
  三元组，`IsSameOrigin` 同源判定，不透明 origin（data: 等）永不与任何
  origin 同源；`Serialize()` 输出规范形式（默认端口省略，不透明为 `"null"`）。
- 浏览器接入：每个标签页加载后记录页面 origin（`Tab::origin` /
  `TabSnapshot::origin`），GUI 可经快照读取。
- 测试：8 个单元测试 + 1 个浏览器集成测试。

## 未实现（诚实标注）

- SOP 在网络读取上的实施（fetch/XHR 读取需 CORS）—— 后续里程碑。
- CORS 头解析与预检、CSP、secure context、权限系统 —— 后续里程碑。
- 经典 `<script>`/`<img>` 跨源加载在浏览器中无需 CORS（本引擎目前允许，
  与规范一致）。

## 参考

- HTML Standard — Origin / same origin
- WhatWG Fetch — CORS 模型（后续实施时遵循）
- docs/security/security-model.md（威胁模型总纲）
