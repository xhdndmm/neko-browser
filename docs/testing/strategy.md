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

> **现状（诚实标注）**：当前只建立了 `tests/unit/`（按模块）、`tests/cmake/`（CMake
> 预设与三方 include 过滤校验）、`tests/pages/`（端到端页面与媒体夹具）三个目录。
> 表中 `tests/integration/`、`tests/network/`、`tests/rendering/`、`tests/fuzz/`、
> `tests/web-platform/` **尚未建立**（见 roadmap 后续项）。集成/端到端性质的测试
> 目前放在 `tests/unit/browser/`、`tests/unit/renderer/`、`tests/unit/ui/` 内，
> 待目录拆分后迁移。

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

- `tests/unit/<module>/`：按模块划分的 GoogleTest 套件（base / url / network /
  html / dom / css / style / layout / paint / renderer / storage / image /
  media / pdf / javascript / browser / ui / ipc / security / compositor /
  graphics）。
- `tests/cmake/`：CMake 预设与三方 include 过滤校验（由 ctest 驱动）。
- `tests/pages/`：端到端测试页面与媒体夹具。
- 可执行文件冒烟测试（`--version` / `--help`）。
- 全部 ctest 用例通过（含 ASan/UBSan）。**本文件不写死测试数量**——数量随代码
  增长，以 CI 实际运行为准，避免再次出现文档漂移。
- 尚未建立：`tests/integration/`、`tests/network/`、`tests/rendering/`（像素对比）、
  `tests/fuzz/`、`tests/web-platform/`。
