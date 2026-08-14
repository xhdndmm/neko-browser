# ADR 0011: security 子系统起步 —— Origin / Same-Origin Policy 基础

- 状态：Accepted
- 日期：2026-08
- 关联：docs/security/security-model.md、docs/security/README.md

## 背景

浏览器安全的核心是**源（origin）**概念：所有跨源交互（fetch 读取、Cookie
作用域、存储分区）都建立在"两个资源是否同源"的判定之上。此前仓库没有
security 子系统；URL 模块已有 `Url::Origin()` 字符串形式，但缺少结构化的、
可比较的 origin 类型，也没有与浏览器生命周期（标签页）的接线。

## 决策

1. 新增 `src/security` 模块（`neko::security`），M1 实现 `Origin` 类型：
   - 三元组 = scheme + host + effective port（显式端口，缺省用 scheme 默认值）；
   - `IsSameOrigin` 按三元组比较；不透明 origin（data: 等非特殊 scheme，
     file: 暂未支持）永不与任何 origin 同源，包括自身；
   - `Serialize()` 输出规范形式（默认端口省略，不透明为 `"null"`）。
2. 浏览器控制器在每个标签页加载后记录页面 origin（`Tab::origin` /
   `TabSnapshot::origin`），经快照暴露给 GUI。
3. **不做**（本里程碑范围之外，诚实标注）：SOP 在网络读取上的实施、CORS
   头解析与预检、CSP、secure context、权限系统。经典 `<script>`/`<img>`
   跨源加载按规范允许（无需 CORS），不强制拦截。

## 理由

- Origin 是 SOP/CORS/CSP/Cookie 安全等所有安全特性的共同基石，先行建立
  结构化模型可避免后续各子系统各自实现、口径不一。
- 结构化 `Origin`（而非字符串）让比较、序列化、存储分区复用同一实现，
  且与 WHATWG HTML 的 "same origin" 定义对齐。
- 接入标签页让 origin 成为页面生命周期的一部分，GUI/DevTools 可直接观察。

## 后果

- 正面：SOP 实施（fetch/XHR）有了现成的判定基础；后续可在此模块扩展
  CORS/CSP 类型。
- 代价/限制：SOP 尚未在网络读取上强制；file:/data: 等 origin 语义
  （不透明）已按规范建模，但相关 scheme 的 URL 支持本身仍是后续工作。
