# 模糊测试（Fuzzing）

> 状态：**已建立**。覆盖 AGENTS.md §32 指定的四个不可信输入解析器：
> URL、HTTP 响应、HTML（tokenizer + tree builder）、CSS（tokenizer + 解析器）。
> WebSocket 帧解析目前与活动 socket 绑定、未暴露独立入口，**暂不 fuzz**（如实标注）。

## 组成

| 组件 | 位置 | 作用 |
| --- | --- | --- |
| 共享 harness | `tests/fuzz/fuzz_targets.{h,cpp}` | 每个解析器一个入口函数 `neko::fuzz::{Url,Http,Html,Css}` |
| libFuzzer 驱动 | `tests/fuzz/fuzz_{url,http,html,css}.cpp` | 长时间 fuzz 运行（Clang，opt-in） |
| 语料库 | `tests/fuzz/corpus/{url,http,html,css}/` | 种子 + **崩溃复现样本（永久回归）** |
| 确定性回归 | `tests/unit/fuzz/fuzz_regression_test.cpp` | 每次构建回放全部语料 + 固定对抗样本，所有工具链 |

## 设计原则

1. **崩溃样本必须落成语料文件**。libFuzzer 发现的每个可复现崩溃，
   最小化后提交到 `tests/fuzz/corpus/<area>/`，从此就是 CI 上的永久回归测试
   （AGENTS.md §17/§32）。确定性回归套件会自动拾取目录中的新文件。
2. **回归套件是下限，不是替代**。`tests/unit/fuzz/` 在 GCC/Clang/MSVC 上
   都会重放语料并跑固定对抗样本（深嵌套、超大 token、畸形 UTF-8、
   帧长度整数溢出、病态重复），保证没有 libFuzzer 的工具链也有覆盖。
3. **harness 自身不能制造假阳性**。DOM 遍历用显式栈（不用递归），
   避免深输入时 harness 自己爆栈，掩盖或伪造解析器发现。
4. **超时 = 发现**。harness 不加人为迭代上限；解析器不终止就是 bug，
   由 libFuzzer `-timeout` 与 CI 超时暴露。

## 运行

构建 libFuzzer 目标（需要 Clang）：

```bash
cmake --preset clang -DNEKO_BUILD_FUZZERS=ON   # 或任意 Clang 配置
cmake --build build/clang --target neko_fuzz_html neko_fuzz_css neko_fuzz_url neko_fuzz_http
```

**语料卫生**：fuzz 运行会把覆盖率增长的输入写进传入的语料目录。
不要直接对仓库目录跑——先复制到工作副本，只把**崩溃最小化样本**带回仓库：

```bash
cp -r tests/fuzz/corpus/html /tmp/html-corpus     # 工作副本
./build/clang/bin/neko_fuzz_html /tmp/html-corpus -max_total_time=60 -timeout=10
./build/clang/bin/neko_fuzz_html crash-<hash>        # 复现 / 最小化单个输入
```

常用参数：`-timeout=N`（单输入秒数上限）、`-rss_limit_mb=2048`、
`-max_len=4096`、`-minimize_crash=1`。

确定性回归（每次 `ctest` 都跑）：

```bash
ctest -R FuzzRegression --output-on-failure
```

## 崩溃处理流程

```text
fuzz 发现 crash
→ ./neko_fuzz_<area> crash-<hash> 复现
→ -minimize_crash=1 最小化
→ 诊断根因（禁止只在 harness 里吞掉症状）
→ 修复 + 把最小输入存入 tests/fuzz/corpus/<area>/
→ 确定性回归套件自动覆盖，防止回归
```

## 覆盖范围与限制

- **URL**：绝对解析、序列化往返、Origin、半输入作 base 的相对解析。
- **HTTP**：`ParseHttpResponse` 状态行/头/Content-Length/chunked 帧。
- **HTML**：tokenizer 必须在 EOF 终止；tree builder 产出的 DOM 必须有限、
  无环（harness 全树遍历验证）。
- **CSS**：tokenizer、`ParseStyleSheet`、`ParseDeclarationBlock`。

**未覆盖**（待后续）：WebSocket 帧解码（需 socket 缝）、图片解码器
（PNG/GIF/SVG 自研解码器应优先加入，输入同为不可信字节）、
JS 绑定层 DOM API 序列化、PDF 解析器。图像解码器已有资源上限
（128 MiB 画布 + inflate 上限），但仍是 fuzz 的高价值目标。
