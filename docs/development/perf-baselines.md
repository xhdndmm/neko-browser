# 性能基线（Performance baselines）

> 状态：**In Progress**（Landing 0：度量面 + 首批基线）

这里记录**可复现**的加载性能数字。存在的理由很直接：之前那些数字来自临时插桩与
临时脚本（`build/timing/`，gitignore），log 一删就**没人能复核**（Council 里
reviewer 的原话：*"none of the packet's numbers are reproducible by a later reader"*）。
现在数字由仓库内的一条命令产出。

## 如何复现

```bash
cmake --preset release && cmake --build --preset release --parallel
./build/release/bin/neko_browser --headless --url https://www.bilibili.com/ --dump-dom --perf-report
```

`--perf-report` 在 stdout 追加**一行 JSON**（`--dump-dom` 的 DOM 也在 stdout，取以
`{"wall_ms"` 开头的那行即可）：

```json
{"wall_ms":13961,"requests":181,"connections_opened":49,"connections_reused":132,
 "connections_discarded":27,"bytes":19233480,
 "phases":{"document":202,"stylesheets1":975,"fonts1":899,"scripts":6442,
           "stylesheets2":617,"fonts2":0,"images":559,"videos":0,"post_load":4162}}
```

- **计数器**（`requests`/`connections_*`/`bytes`）在同一页面内容下可重复：本次两次
  连续运行完全相同。页面内容轮换（bilibili 会个性化/轮播）会带来 ±1–2 请求、
  ±1 MB 的差异。
- **相位时间**是波动的（网络与机器负载），只作证据，**不作门禁**——见下"纪律"。
- 相位语义见 `src/browser/include/neko/browser/perf_report.h`。`wall_ms` 含截图
  光栅化（因此 `wall_ms − Σphases` 大致就是截图时间）。

## 实测（2026-10-03，本机 Linux x86_64，同机同网，`--dump-dom`）

| 指标 | P0 之前 | **HEAD（`2484fdc`，release）** | HEAD（debug，`--perf-report`） |
| --- | --- | --- | --- |
| 墙钟 | 16.9 / 24.2 / 25.4 / 26.1 / 28.6 s | **12.9 / 11.3 s** | 14.0 / 15.3 s |
| 请求数 | 183 | 182 | 181 |
| 新建 TCP+TLS | **183** | 49–54 | 49 |
| 走热连接 | 0 | **132–133** | 132 |
| 载荷 | 20.8 MB | 16.0 / 16.9 MB | 18.2 / 19.2 MB |
| document | 0.4–1.1 s | （相位内）| 0.20 / 0.99 s |
| stylesheets1 | ~1.3 s | 0.62 / 0.73 s | 0.62 / 0.98 s |
| fonts1 | ~2.2 s | 1.29 / 2.05 s | 0.90 / 1.20 s |
| **scripts** | **11.1–19.7 s** | **5.21 / 5.82 s** | 6.44 / 6.88 s |
| stylesheets2 | （含在 scripts 内） | 0.33 / 0.39 s | 0.62 / 0.82 s |
| images | ~0.6 s | 0.41 / 0.62 s | 0.56 / 0.58 s |
| **post_load** | ~4.3 s | （未单独测） | **4.06 / 4.16 s** |
| XHR 类请求 | 33 个 / 12.9 s / 跨度 19.6 s / 每个 ~0.51 s | **32 个 / 3.6–5.3 s / 跨度 7.8 s / 中位 24–102 ms** | （含在 scripts + post_load） |

关键结论（写入 ADR 0022 修订与 Council memo）：

1. **P0 的连接复用直接把 XHR 变便宜**：31/32 个 XHR 骑在热连接上，单请求从
   ~0.51 s 降到中位 24–102 ms。**"可回收 12.9 s"这一说法在 HEAD 上已过期**，
   P1 异步化的收益必须重新表述（上限 = Σ(耗时) − 最长单个请求，实际未知）。
2. `scripts` 仍是最大单项（5.2–6.9 s）——剩余部分是脚本体抓取后的**执行**，不是传输。
3. **`post_load` ≈ 4.1 s 是本轮新量化的未处理大项**（定时器泵 + 晚到图片轮次），
   P0 完全没碰它；它和阻塞式 XHR 一样会让帧产出停摆。
4. 差异说明：`Σ(XHR 耗时) = 3.6 s` 却占 **7.8 s 跨度** → 剩余时间**不在传输**
   （JS 计算，或页面自己在等依赖）。这正需要 fixture A/B 与 send() 站点插桩来判定。

## 纪律（Council 收敛项）

- **不得用墙钟或 per-pump ms 阈值作 CI 门禁**：共享 runner 上必然 flake，且
  `TESTING.md`（"时间相关测试"）与 AGENTS.md §44 明文禁止拿时间窗口赌断言。
  CI 只门禁**确定性量**：计数器与行为（如 `tests/unit/network/http_client_test.cpp`
  的连接复用计数、注入时钟下的泵轮次）。
- **每次落地刷新本文件的数字**（含机器/提交/日期），否则报告型基线会腐烂。
  这是明文纪律，不是建议。
- 本机**没有** `perf`/`strace`/`ltrace`，归因只能靠进程内相位计时 + 连接/字节
  计数；需要 CPU 级剖析时另行说明，不新增依赖。

## 已知限制（诚实标注）

- `--renderer-process` 下抓取发生在子进程，报告里的连接计数器描述的是**未被使用**的
  进程内 client；相位对渲染子进程模式亦不适用。
- `bytes` 是**解码后**的响应体大小（含字体/图片/脚本），不含 HTTP 头。
- 只覆盖**加载**：仓库目前仍**没有任何帧时间/jank/交互延迟指标**（Council 双方
  独立确认），流畅性指标是下一步工作。
- 尚无按资源类别拆分的字节统计（本页字体 108 文件 / 9.6 MB 这类数字仍来自临时
  插桩），拆分统计列为后续。
