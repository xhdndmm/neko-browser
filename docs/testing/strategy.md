# 测试策略

## 目标

每个重要行为必须有验证策略。测试不是项目最后才补的工作。

## 分层

| 层级 | 位置 | 内容 | 运行时机 |
| --- | --- | --- | --- |
| 单元测试 | `tests/unit/` | 单模块、快速、无外部服务 | 每次 CI |
| 集成测试 | `tests/integration/` | 跨模块流程（Phase 2+） | 每次 CI |
| 网络测试 | `tests/network/` | 本地 HTTP 服务器 | 每次 CI |
| 渲染测试 | `tests/rendering/` | HTML+CSS → 截图 → 像素对比 | 每次 CI |
| 模糊测试 | `tests/fuzz/` | URL/HTML/CSS/HTTP parser | CI 冒烟 + 定期 |
| Web Platform Tests | `tests/web-platform/` | WPT 子集 | Phase 13+ |

## 单元测试规范

- 每个测试只验证一个行为点。
- 断言必须有意义（禁止为覆盖率写无意义测试）。
- 边界条件必须覆盖：空输入、极长输入、畸形输入、非法 UTF-8、整数边界。
- 测试命名：`<Module>Test.<Behavior>`（如 `UrlTest.ParseHost`）。

## 渲染测试

```
HTML + CSS → Browser Engine → Screenshot → Pixel Comparison
```

必须处理平台差异：字体、抗锯齿、DPI。禁止在不同后端间要求逐字节一致。

## 模糊测试

重点目标：URL、HTML tokenizer/parser、CSS tokenizer/parser、HTTP parser。
规则：

- 每个可复现的崩溃 → 转为回归测试
- 模糊输入视为不可信输入

## 覆盖率

- 关注核心 parser、URL、CSS、DOM、Layout、Network。
- 覆盖率不是唯一指标；代码质量优先于数字。
- CI 中生成覆盖率报告并上传 artifact。

## 失败处理流程

```text
测试失败 → 判定类别 → 修复根因 → 添加回归测试 → 复跑
```

禁止：删除测试、改期望掩盖问题、关 sanitizer、注释失败代码。

## 当前状态

- `tests/unit/base/`：status / string_util / logging 测试（单元）
- `tests/unit/browser/`：CLI 选项解析测试（单元）
- 可执行文件冒烟测试（`--version` / `--help`）
- 共 54 个测试，全绿。
