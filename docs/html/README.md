# HTML 模块

> 状态：**Implemented**（Phase 3，子集）

## 已实现

- 真实 tokenizer（非正则）：标签/属性（各引号风格）/注释/doctype/字符引用
  （数值 + **完整 WHATWG 命名字符引用表 2125 项**，含双码点序列，二分查找；
  旧名无分号集合按 WHATWG entities.json 生成）/RAWTEXT(style)/
  RCDATA(title,textarea)/script data（含 escape、double-escape 状态）
- 树构建：initial → before html → before head → in head → after head →
  in body → text → after body；隐含 html/head/body、隐含 p/li/标题闭合、
  void 元素、游离结束标签、EOF 骨架
- 作用域判定：默认作用域 + button 作用域（「关闭 p 元素」步骤按 button 作用域
  判定，<button> 内部的 <p> 不会被后续块级元素（如 <div>）强行闭合）
- 活动格式化元素列表（含 Noah's Ark 条款）+ adoption agency 算法
  （错嵌套格式化元素的领养/重建）
- 畸形 HTML 作为普通输入处理

## 未实现

- table 相关容错（foster parenting）——表格按普通块处理
- 活动格式化元素列表的 marker（进入 applet/object/marquee/template/td/th/caption
  时插入的标记）——依赖表格支持，尚未使用
- CDATA section 状态、processing instruction 状态（依赖 foreign content）

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
