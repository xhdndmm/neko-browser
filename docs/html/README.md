# HTML 模块

> 状态：**Implemented**（Phase 3，子集）

## 已实现

- 真实 tokenizer（非正则）：标签/属性（各引号风格）/注释/doctype/字符引用
  （数值 + 常用命名子集）/RAWTEXT(style)/RCDATA(title,textarea)/
  script data（含 escape、double-escape 状态）
- 树构建：initial → before html → before head → in head → after head →
  in body → text → after body；隐含 html/head/body、隐含 p/li/标题闭合、
  void 元素、游离结束标签、EOF 骨架
- 活动格式化元素列表（含 Noah's Ark 条款）+ adoption agency 算法
  （错嵌套格式化元素的领养/重建）
- 畸形 HTML 作为普通输入处理

## 未实现

- 完整 WHATWG 字符引用表（~2200 项，当前为常用子集）
- table 相关容错（foster parenting）——表格按普通块处理
- 活动格式化元素列表的 marker（进入 applet/object/marquee/template/td/th/caption
  时插入的标记）——依赖表格支持，尚未使用
- CDATA section 状态、processing instruction 状态（依赖 foreign content）

## 架构

```text
HTML Source → Tokenizer → Token Stream → Parser → DOM
```

参考：WHATWG HTML Standard。
