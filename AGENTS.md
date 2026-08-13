# Neko-Browser AGENT.md

## 项目概述

本项目是一个浏览器引擎（Browser Engine），负责实现从网络资源获取到网页渲染、脚本执行以及用户交互的完整流程。

核心目标：

* 实现标准化的 HTML / CSS / JavaScript 处理能力
* 实现 DOM、CSSOM、Layout、Paint、Compositing 等浏览器核心机制
* 提供网络资源加载能力
* 提供可扩展的 JavaScript Runtime
* 保持模块之间低耦合
* 优先保证正确性、可调试性和可测试性
* 在正确性稳定后再进行性能优化

除非明确要求，否则不要为了性能牺牲代码可读性、正确性或可维护性。

---

## Agent 工作原则

### 1. 先理解，再修改

修改代码前必须：

1. 阅读相关模块的代码
2. 查找调用者和被调用者
3. 理解数据流
4. 确认现有测试
5. 判断修改是否会影响其他模块

不要只根据函数名猜测代码行为。

对于大型修改，应先给出简短的实现计划，再开始修改。

### 2. 优先小范围修改

除非任务明确要求重构，否则：

* 不要修改无关代码
* 不要顺手重命名大量 API
* 不要改变公共接口
* 不要引入不必要的依赖
* 不要重新组织整个项目结构
* 不要删除看起来“没用”的代码，除非确认其确实没有使用

### 3. 不要伪造实现

如果某个 Web 标准、协议或算法尚未实现：

* 不要假装已经完整支持
* 不要用静态结果欺骗测试
* 不要简单返回固定值绕过逻辑
* 应明确标记 TODO / FIXME
* 必要时增加对应的测试用例

---

# 架构

浏览器引擎原则上按照以下层次组织：

```text
Application
    │
    ▼
Browser / Page
    │
    ├── Network
    │
    ├── HTML Parser
    │       │
    │       ▼
    │      DOM
    │
    ├── CSS Parser
    │       │
    │       ▼
    │      CSSOM
    │
    ├── Style System
    │       │
    │       ▼
    │   Computed Style
    │
    ├── Layout
    │       │
    │       ▼
    │   Layout Tree
    │
    ├── Paint
    │       │
    │       ▼
    │   Display List
    │
    ├── Compositor
    │       │
    │       ▼
    │    Graphics
    │
    └── JavaScript Runtime
            │
            ├── DOM Bindings
            ├── Web APIs
            └── Event Loop
```

实际目录结构可能不同，但新增代码应尽量保持类似的依赖方向。

---

# 模块职责

## Network

负责：

* DNS
* TCP / TLS
* HTTP / HTTPS
* 请求与响应
* Headers
* Cookies
* Redirect
* Cache
* Resource Loading

Network 层不应该直接操作 DOM 或 Layout。

---

## HTML Parser

负责：

* Tokenization
* HTML Parsing
* Error Recovery
* Element 创建
* Attribute 解析
* Text Node 创建
* DOM Tree 构建

HTML Parser 不负责：

* CSS Layout
* Paint
* JavaScript 执行
* GPU 绘制

如果脚本执行会影响解析流程，应通过明确的 Parser / Script 协调机制处理，而不是直接产生跨模块依赖。

---

## DOM

DOM 是 HTML 文档的结构表示。

典型对象包括：

```text
Node
 ├── Document
 ├── Element
 ├── Text
 ├── Comment
 └── DocumentFragment
```

DOM 应负责：

* Tree Structure
* Parent / Child 关系
* Attributes
* Event Target
* Mutation

DOM 不应该直接负责 Layout 或 Paint。

---

## CSS

CSS 系统主要负责：

```text
CSS Source
    ↓
Tokenizer
    ↓
Parser
    ↓
Stylesheet
    ↓
Selector Matching
    ↓
Cascade
    ↓
Computed Style
```

不要把 CSS Parser、Selector Matching、Cascade 和 Layout 混合在一起。

---

## Style System

Style System 将 DOM 和 CSSOM 联系起来。

输入：

```text
DOM
CSS Rules
User Agent Rules
Inline Style
```

输出：

```text
Computed Style
```

需要注意：

* Cascade
* Specificity
* Inheritance
* Initial Values
* Relative Units
* CSS Variables

如果某个 CSS 属性尚未实现，应保持明确的 fallback 行为。

---

## Layout

Layout 根据 DOM 和 Computed Style 计算几何信息。

主要输入：

```text
DOM Tree
Computed Style
Viewport
```

主要输出：

```text
Layout Tree
```

Layout 负责：

* Width
* Height
* Position
* Margin
* Padding
* Border
* Box Model
* Text Layout
* Block Layout
* Inline Layout
* Flex Layout
* Grid Layout（如果已实现）

不要在 Layout 阶段直接进行最终 GPU 绘制。

---

## Paint

Paint 将 Layout 结果转换成可绘制指令。

例如：

```text
DrawRect
DrawBorder
DrawText
DrawImage
Clip
Transform
```

推荐使用 Display List 或类似中间表示。

Paint 不应该重新计算完整 Layout。

---

## Compositor

Compositor 负责将多个绘制层组合成最终画面。

典型流程：

```text
Display List
    ↓
Paint Layers
    ↓
Compositing
    ↓
GPU / Software Renderer
    ↓
Framebuffer
```

如果项目支持 GPU：

* 不要在业务层直接调用底层 GPU API
* GPU 资源生命周期必须明确
* 避免跨线程访问 GPU 对象
* 注意纹理、Buffer、Surface 的所有权

---

# JavaScript Runtime

JavaScript Runtime 与浏览器引擎其他部分通过明确的 API Boundary 连接。

主要组成：

```text
JavaScript Engine
    │
    ├── Parser
    ├── AST / Bytecode
    ├── Interpreter
    ├── GC
    └── Runtime
            │
            ▼
        Web APIs
```

Web API 不应该直接依赖 JavaScript Parser 或 Interpreter 的内部实现。

例如：

```text
DOM API
Fetch API
Timer API
Console API
Event API
```

应通过稳定的 Runtime Interface 暴露。

---

# Event Loop

浏览器中的异步操作必须遵循明确的任务调度模型。

概念结构：

```text
Task Queue
    │
    ▼
Event Loop
    │
    ├── Task
    ├── Microtask
    └── Rendering
```

需要区分：

* Task
* Microtask
* Timer
* Network Event
* DOM Event
* Rendering Update

不要通过随意创建线程来解决异步问题。

---

# 多线程

如果项目使用多线程，必须明确线程职责。

例如：

```text
Main Thread
    ├── DOM
    ├── JavaScript
    └── Event Loop

Network Thread
    └── Network I/O

Compositor Thread
    └── Compositing

Raster Thread
    └── Rasterization
```

线程之间不要直接共享可变状态。

优先使用：

* Message Passing
* Immutable Data
* Ownership Transfer
* 明确的 Lock

避免：

* 全局锁
* 全局可变状态
* 隐式线程同步
* 随处增加 mutex

---

# 内存管理

浏览器引擎通常拥有大量长生命周期对象。

开发时必须明确对象生命周期。

特别注意：

* DOM Tree
* Layout Tree
* CSS Rules
* Images
* Network Buffers
* GPU Resources
* JavaScript Objects

如果使用手动内存管理，应明确：

```text
create
    ↓
owner
    ↓
borrow
    ↓
release
```

如果使用 GC，则注意：

* Root
* Reachability
* Mark
* Sweep
* Finalization

不要因为“方便”而无限制缓存对象。

---

# 错误处理

错误应该尽可能在正确的层级处理。

推荐：

```text
Low-level Error
    ↓
Contextual Error
    ↓
Public Error
```

错误信息应包含足够的上下文。

例如：

```text
failed to parse CSS selector:
selector=".foo >"
position=8
reason=unexpected end of selector
```

不要：

```text
error
failed
something went wrong
```

对于 Web 内容中的错误，应尽可能遵循浏览器的容错原则。

网页写错不应该轻易导致整个浏览器崩溃。

---

# 标准实现

实现 Web 标准时：

1. 优先确认标准定义
2. 查找已有测试
3. 理解边界条件
4. 实现最小正确行为
5. 添加测试
6. 再考虑优化

不要只根据某个浏览器的行为猜测标准。

如果需要参考实现，可以研究：

* Chromium
* Firefox / Gecko
* WebKit

但不要直接复制大型实现而不理解其设计。

---

# 测试

任何影响核心行为的修改都应该增加测试。

测试类型包括：

```text
Unit Test
Integration Test
Parser Test
Layout Test
Rendering Test
JavaScript Test
Web Platform Test
Regression Test
```

优先测试边界情况。

例如 HTML Parser：

```html
<div>
<div>
</div>
```

CSS：

```css
.foo {
    width: calc(100% - 10px);
}
```

Layout：

```text
width: auto
height: auto
overflow
percentage
nested block
```

不要只测试正常输入。

---

# 回归测试

修复 Bug 时：

```text
Bug
 ↓
最小复现
 ↓
Regression Test
 ↓
Fix
 ↓
Full Test
```

不要只修改代码然后确认“看起来正常”。

如果 Bug 可以稳定复现，应将其加入测试集，避免未来重新出现。

---

# Debugging

遇到 Bug 时优先使用以下顺序：

1. 最小化问题
2. 确认输入
3. 确认输出
4. 检查数据流
5. 检查状态变化
6. 检查生命周期
7. 检查线程问题
8. 检查内存问题
9. 检查标准行为

不要一开始就大范围修改代码。

推荐增加临时日志：

```text
[Network]
[Parser]
[DOM]
[CSS]
[Style]
[Layout]
[Paint]
[Compositor]
[JS]
[EventLoop]
```

日志应该能够通过模块名称快速定位来源。

---

# 性能优化

性能优化必须建立在 profiling 基础上。

禁止：

```text
我觉得这里慢
↓
直接重写
```

推荐：

```text
Benchmark
    ↓
Profile
    ↓
Identify Hot Path
    ↓
Optimize
    ↓
Benchmark Again
```

优化后必须确认：

* 性能是否真的提高
* 内存是否增加
* 正确性是否受到影响
* 是否增加复杂度

---

# API 设计

公共 API 应：

* 简单
* 一致
* 可预测
* 有明确生命周期
* 避免暴露内部实现

不要把内部数据结构直接作为公共 API。

例如不推荐：

```text
GetInternalLayoutNode()
```

更推荐：

```text
GetBoundingClientRect()
```

---

# 依赖管理

新增第三方依赖前必须考虑：

* 是否真的需要
* 项目是否已经存在类似功能
* License
* 维护状态
* 安全性
* 编译体积
* 跨平台支持

能使用标准库解决的问题，不要为了少量代码引入大型依赖。

---

# Git

Commit 应保持单一目的。

推荐：

```text
parser: fix malformed tag handling
css: implement selector specificity
layout: fix percentage width calculation
network: handle HTTP redirects
paint: add border radius rendering
```

不要：

```text
fix stuff
update code
changes
```

不要把格式化、重构和功能修改混在一个 Commit 中，除非确实无法分离。

---

# 修改前检查清单

* [ ] 已阅读相关代码
* [ ] 已确认模块职责
* [ ] 已确认调用关系
* [ ] 已检查已有测试
* [ ] 已确认是否涉及公共 API
* [ ] 已确认线程安全问题
* [ ] 已确认生命周期问题
* [ ] 已考虑错误处理
* [ ] 已考虑回归测试

---

# 修改后检查清单

* [ ] 项目可以编译
* [ ] 相关测试通过
* [ ] 没有明显新增 Warning
* [ ] 没有无关代码修改
* [ ] 没有遗留 Debug Code
* [ ] 没有未解释的 TODO
* [ ] 没有明显内存泄漏
* [ ] 没有明显线程安全问题
* [ ] 新增行为有测试覆盖

---

# Agent 禁止事项

除非用户明确要求，否则不要：

* 删除测试
* 删除错误处理
* 删除日志系统
* 修改公共 API
* 大规模重构
* 引入大型第三方依赖
* 为了通过测试写死结果
* 修改与任务无关的模块
* 修改构建系统的核心配置
* 禁用编译器 Warning
* 禁用 Sanitizer
* 忽略崩溃
* 使用未定义行为作为优化手段

---

# 优先级

当多个目标发生冲突时，按照以下优先级处理：

```text
Correctness
    >
Security
    >
Memory Safety
    >
Maintainability
    >
Performance
    >
Code Size
```

性能优化不能破坏正确性。

---

# 实现策略

对于尚未实现的浏览器功能，采用渐进式实现：

```text
Specification
    ↓
Minimal Implementation
    ↓
Unit Test
    ↓
Integration Test
    ↓
Edge Cases
    ↓
Optimization
```

不要试图一次实现完整 Web 标准。

每次提交应尽可能保持：

```text
Small
Correct
Testable
Reviewable
```

---

# 最终原则

浏览器引擎是一个高度复杂的系统。

Agent 在修改代码时应始终优先考虑：

> **正确的数据流、清晰的模块边界、明确的生命周期、可重复的测试，以及可调试性。**

如果不确定某项行为，不要猜测。

先检查：

```text
代码
测试
标准
现有架构
```

然后再修改。
