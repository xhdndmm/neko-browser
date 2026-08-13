# 兼容性矩阵

> 本文档诚实记录每个特性的支持状态。**禁止**把"接口存在"写成"已实现"。
> 状态取值：Not Started / Planned / In Progress / Partial / Implemented / Tested。
> 最后更新：2026-08（Phases 1–6 达成）。

| 特性 | 状态 | 测试证据 | 备注 |
| --- | --- | --- | --- |
| URL 解析 | Tested | 19 单元测试 | RFC 3986 相对解析样例 |
| HTTP/1.1 | Tested | 8 单元测试 | GET、chunked、重定向、Content-Length |
| HTTPS / TLS | Not Started | — | Socket 层已预留 |
| gzip/deflate | Not Started | — | 返回显式 NOT IMPLEMENTED |
| HTML tokenizer | Tested | HTML 套件 | 字符引用子集、RAWTEXT/RCDATA |
| HTML parser | Tested | HTML 套件 | 插入模式子集（无 table 容错） |
| DOM | Tested | DOM 套件 | 树操作、querySelector 子集 |
| CSS tokenizer/parser | Tested | CSS 套件 | 规则、声明、!important、@media |
| 选择器匹配 | Tested | CSS 套件 | 属性/伪类/组合器子集 |
| 级联 / 计算样式 | Tested | Style 套件 | 特异性、继承、内联样式 |
| Block layout | Tested | Layout 套件 | 盒模型、堆叠、宽度填充 |
| Inline layout | Tested | Layout 套件 | 文字换行、行盒 |
| Flexbox | Not Started | — | display:flex 按 block 处理 |
| Grid | Not Started | — | display:grid 按 block 处理 |
| position absolute/fixed | Not Started | — | 按 static 处理 |
| 文本（位图字体） | Partial | Paint 套件 | 仅 ASCII，无整形/回退 |
| 文本（FreeType/HarfBuzz） | Not Started | — | 计划中 |
| 图像解码 | Not Started | — | — |
| 绘制 / 光栅化 | Tested | Paint 套件 | 纯色、边框、文字、PPM |
| 合成器 | Not Started | — | — |
| JavaScript | Not Started | — | Phase 8 |
| Fetch（浏览器 API） | Not Started | — | — |
| Cookie | Not Started | — | — |
| LocalStorage | Not Started | — | — |
| 多进程 | Not Started | — | Phase 12 |
| 日志系统 | Tested | 单元测试 | — |
| Error/Result 模型 | Tested | 单元测试 | — |
| CLI 参数解析 | Tested | 单元测试 | — |
| 真实网页渲染 | Tested | 端到端手工验证 | http://example.com/ 截图 |

更新规则：任何特性状态变化必须同步更新本矩阵与对应模块文档。
