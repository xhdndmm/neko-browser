# HTML 模块

> 状态：**Implemented**（Phase 3，子集）

## 已实现

- 真实 tokenizer（非正则）：标签/属性（各引号风格）/注释/doctype/字符引用
  （数值 + **完整 WHATWG 命名字符引用表 2125 项**，含双码点序列，二分查找；
  旧名无分号集合按 WHATWG entities.json 生成）/RAWTEXT(style,xmp,iframe,
  noembed)/RCDATA(title,textarea)/PLAINTEXT(plaintext)/script data（含
  escape、double-escape 状态）
- 输入预处理：CRLF/CR → LF 归一化（§13.2.3.5）；EOF-in-tag 丢弃未闭合标签
- DOCTYPE 完整状态机：public/system identifier（单/双引号）、bogus doctype、
  force-quirks 标志（EOF/缺失标识符等错误置位）；属性值上下文按 §13.2.5.78
  规则处理（legacy 无分号名后接 `=`/alnum 时按字面输出）
- 树构建：initial → before html → before head → in head → after head →
  in body → text → after body，外加完整表格模式链：
  in table → in table text → in caption → in column group → in table body →
  in row → in cell；隐含 html/head/body、隐含 p/li/标题闭合、void 元素、
  hr/center 关闭 p、dd/dt 互闭、游离结束标签、EOF 骨架
- **表格容错（foster parenting）**：表格内误置的文本/元素被「寄养」到表格之前
  （§13.2.6.1、13.2.6.4.9-4.15），含 pending table character tokens 缓冲与
  清空栈至 table/table body/table row 上下文的算法
- **活动格式化元素列表的 marker**（td/th/caption 进入时插入，清栈回 table
  上下文时清除）+ Noah's Ark 条款 + adoption agency 算法（错嵌套格式化
  元素的领养/重建）
- **列表项闭合**：`<li>` 按 WHATWG 13.2.6.4.7 规则闭合——逐节点向上找打开的
  li，遇 special 元素（非 address/div/p，如嵌套 `<ul>/<ol>`）即停止，因此嵌套
  列表的 li 不会错误关闭外层 li；`</li>` 结束标签按 list item scope 判定
- after head 模式下 base/link/meta/style/script 等仍按 in head 规则追加到
  head 元素（head element pointer）
- 作用域判定：默认作用域 + button 作用域 + table 作用域 + list item 作用域
- 畸形 HTML 作为普通输入处理
- 字符集：解析前由 `neko::base::encoding` 完成检测与转码（BOM > HTTP
  Content-Type > meta 预扫描 > UTF-8 默认；见 ADR 0012），解析器只接收
  已转码的 UTF-8

## 未实现

- CDATA section 状态、processing instruction 状态（依赖 foreign content）
- in template / in frameset / in head noscript 模式（依赖 template/frameset
  支持）
- quirks mode 尚未接线到 CSS/布局（force-quirks 标志已由 tokenizer 计算，
  但 Document 的渲染模式仍为 no-quirks）

## 命名字符引用（生成代码）

`src/html/src/entities_generated.inc` 由 `tools/gen_html_entities.py` 从
`tools/html/entities.json`（WHATWG HTML Standard §13.2.5.73 官方实体表，
https://html.spec.whatwg.org/entities.json）生成，**请勿手工编辑**。重新生成：

```bash
python3 tools/gen_html_entities.py
```

生成内容：2125 个 `&name;` 实体（按名称排序，二分查找；少数为双码点序列）
+ 106 个可省略分号的旧名（legacy set，由 entities.json 中无分号键推导）。

## 架构

```text
HTML Source → Tokenizer → Token Stream → Parser → DOM
```

参考：WHATWG HTML Standard。
