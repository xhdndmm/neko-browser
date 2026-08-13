# Style 引擎设计

> 位置：`neko::style`（`src/style/`）

## 为什么存在

把 CSS 规则与 DOM 结合成每个元素的"计算样式"，供布局消费。负责选择器匹配、
级联、继承与单位解析。

## 管线

```text
UA 样式表 + <style> 作者样式表 + style 属性
        ↓ 选择器匹配（neko::css）
级联：importance > specificity > order
        ↓
继承（color/font-*/line-height/text-align）
        ↓
单位解析（px/em/rem/%）
        ↓
ComputedStyle（px 值）
```

## 关键决策

- UA 样式表以字符串内嵌（`kUaStylesheet`），CSS parser 解析。
- 内联样式用合成特异性（a=10^6）参与级联，保证"内联 > 普通规则"且
  "作者 !important > 内联普通"。
- em/rem 在计算时解析：em 相对父元素 font-size，rem 相对根 font-size；
  百分比（宽/高/边距/内边距）留给布局阶段按包含块解析。

## 未实现

- flex/grid display（按 block 处理）、font 简写、复杂媒体查询求值。
