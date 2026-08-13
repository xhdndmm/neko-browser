# 技术文档索引

neko-browser 的文档体系。文档是项目的一等公民 —— 改动架构、公共 API、模块职责
时必须同步更新对应文档。

## 架构

- [总体架构](architecture/architecture.md) —— 模块划分、依赖方向、数据流
- [架构决策记录（ADR）](architecture/adr/)
  - [0001：使用 CMake 构建系统](architecture/adr/0001-cmake-build-system.md)
  - [0002：C++20 标准基线](architecture/adr/0002-cpp20-baseline.md)
  - [0003：GoogleTest 经 FetchContent 固定版本](architecture/adr/0003-googletest-fetchcontent.md)
  - [0004：自研日志与 Error/Result 模型](architecture/adr/0004-own-logging-and-result.md)

## 开发

- [开发路线图](development/roadmap.md) —— 分阶段计划与里程碑
- [依赖策略](development/dependency-policy.md)
- [编码风格](development/coding-style.md)
- [Sanitizer 使用](development/sanitizers.md)

## 测试与安全

- [测试策略](testing/strategy.md)
- [安全模型](security/security-model.md)
- [兼容性矩阵](compatibility/compatibility-matrix.md)

## 模块文档（按阶段逐步填充）

| 模块 | 状态 | 文档 |
| --- | --- | --- |
| Network | Not Started | [networking](networking/README.md) |
| HTML | Not Started | [html](html/README.md) |
| CSS | Not Started | [css](css/README.md) |
| Layout | Not Started | [layout](layout/README.md) |
| Rendering | Not Started | [rendering](rendering/README.md) |
| JavaScript | Not Started | [javascript](javascript/README.md) |

## 发布

- [发布说明](releases/README.md)
