# CONTRIBUTING

欢迎为 neko-browser 贡献代码。请先阅读：

- [AGENTS.md](AGENTS.md) —— 项目的工程规则与架构约束（最重要）
- [ARCHITECTURE.md](docs/architecture/architecture.md)
- [开发路线图](docs/development/roadmap.md)
- [编码风格](docs/development/coding-style.md)

## 工作流程

1. **先讨论，后实现**。对于架构级改动，先在 issue 中说明方案。
2. 从 `main` 创建分支，使用清晰的提交信息（见下）。
3. 保持提交**小而原子**，一个提交只做一件事。
4. 每个功能/修复都必须带测试。
5. 提交前运行本地检查（见下）。
6. 发起 Pull Request，CI 全绿后由维护者合并。

## 提交信息

使用 Conventional Commits 风格：

```text
feat:     新功能
fix:      修复
refactor: 重构（无行为变化）
test:     测试
docs:     文档
build:    构建系统
ci:       CI 配置
perf:     性能
security: 安全
```

## 本地检查（提交前必须）

```bash
./tools/check.sh debug          # 格式检查 + 配置 + 构建 + 测试
```

如果装了 clang-format：

```bash
./tools/format.sh               # 自动格式化
```

CI 会强制：

- 格式检查（clang-format，阻塞）
- 编译警告即错误（`-Werror`，阻塞）
- 全部单元测试通过（阻塞）
- ASan/UBSan（阻塞）
- clang-tidy（实验性，非阻塞）

## 代码规范要点

- 遵循 [coding-style.md](docs/development/coding-style.md)。
- 新依赖必须符合 [dependency-policy.md](docs/development/dependency-policy.md)。
- 任何 `TODO` 必须指向 issue 并有明确描述。
- 禁止伪实现（空函数、硬编码、返回固定字符串充当实现）。
- 未实现的功能必须明确标注 `NOT IMPLEMENTED` / `PARTIALLY IMPLEMENTED`。

## 文档

改动架构、公共 API、模块职责时，必须同步更新对应文档。

## 行为准则

见 [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md)。
