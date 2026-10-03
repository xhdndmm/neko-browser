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
`{"wall_ms"` 开头的那行即可）。**推荐用仓库里的读取器**，它直接从管道挑出 JSON 行、
支持多轮采样与中位数，并会提示确定性的计数器是否在样本间漂移：

```bash
./build/release/bin/neko_browser --headless --url <URL> --dump-dom --perf-report 2>/dev/null \
  | python3 tools/perf_report.py                 # 单次
# 多轮（推荐：3 次取中位数）
for i in 1 2 3; do
  ./build/release/bin/neko_browser --headless --url <URL> --dump-dom --perf-report 2>/dev/null \
    | grep -o '{"wall_ms".*}' >> /tmp/perf.jsonl
done
python3 tools/perf_report.py --unit s /tmp/perf.jsonl
```

原始行（供脚本消费）：

```json
{"wall_ms":17993,"requests":181,"connections_opened":50,"connections_reused":131,
 "connections_discarded":27,"bytes":18781086,
 "phases":{"document":1108,"stylesheets1":601,"fonts1":1112,
           "scripts":8518,"scripts_fetch":985,"scripts_exec":51,
           "scripts_modules":7346,"scripts_events":0,"scripts_restyle":129,
           "stylesheets2":390,"fonts2":0,"images":1036,"videos":0,
           "pump":4459,"late_images":720,"post_load":5179},
 "pump_turns":16,"pump_over_50ms":1,"pump_total_ms":4016,
 "pump_p50":0,"pump_p95":3992,"pump_max":3992,
 "pump_breakdown":{"timers_ms":69,"timers":103,"jobs_ms":11,"job_rounds":103,
                   "xhr_ms":0,"xhr":9,"raf_ms":3929,"raf":38,
                   "ports_ms":0,"ports":0,"websockets_ms":0,"websockets":0},
 "frames":0,"frame_over_50ms":0,"frame_production_p50":0,"frame_production_p95":0,
 "frame_production_max":0,"frame_interval_p50":0,"frame_interval_p95":0,
 "frame_interval_max":0}
```

> 上面是**一次真实运行**（release，bilibili）的完整一行，所有字段互相对得上：
> 脚本五段之和 8511 ms 对 `scripts = 8518 ms`（残差 7 ms），
> `post_load = pump + late_images`（4459 + 720 = 5179）。**不要把不同运行的字段拼在一起**：
> 这段曾经就是把拆分后的键粘进拆分前的 `scripts` 基线，凭空造出 540 ms 的"缺口"。

**`scripts_*` 把脚本阶段拆开**（此前只有一个 `scripts`，无法判断该优化什么）：
`scripts_fetch`＝阻塞等经典脚本体；`scripts_exec`＝`Evaluate`（解析+编译+执行）；
`scripts_modules`＝模块脚本（`EvaluateModule` 及其 import 抓取）；`scripts_events`＝
`DOMContentLoaded`/`load` 处理器；`scripts_restyle`＝脚本后引擎重跑的全量样式级联。
五段在 `RunPageScripts` 内部计时，而 `scripts` 是在它外层测的，所以两者之差就是**残差**：
DOM 里收集 `<script>`、import-map 解析、并行预取的提交与去重集合、以及三个执行 pass 的
循环开销（模块 body 本身在 `fetch()` 内产出、在模块计时器**之内**，不是残差）。计时内部按**纳秒累加、出口一次
换算**，因此残差不是六段各自截断的堆积（上一次的实现是逐段截断，"未归属 8–11 ms"不足以
证明闭合，现已修正）。实测残差 7–11 ms。

**`pump_breakdown` 说明那些轮里到底跑了什么**（没有它，一个 2.7 s 的轮子毫无可操作性）：
`timers`/`timers_ms`（到期 `setTimeout`/`setInterval`）、
`job_rounds`/`jobs_ms`（promise 续体）、`xhr`/`xhr_ms`（排队的 XHR 完成投递）、
`raf`/`raf_ms`（`requestAnimationFrame` 回调）、`ports`、`websockets`。
六段与一轮互斥穷尽（各段耗时之和 ≈ 该轮耗时）。

**`pump_*` 是流畅性的主指标**：一次「泵动作」= `DomBinder::RunPendingTimers()` 的一次调用，
也就是到期定时器 + 排队中的 XHR 完成回调真正执行 JS 的那一轮——用户感知到的"长任务"
就是这一轮。轮与轮之间的**等待不计入**（那是空闲，不是工作）。
`pump_over_50ms` 是 ≥50 ms 的轮数（**纯信息，不作门禁**）。

**`frames` 只在存在帧管线时非零**：headless `--dump-dom` 不产生帧，因此恒为 0；
GUI 路径用下面的工具测（帧产出耗时 + 帧间隔）。

- **计数器**（`requests`/`connections_*`/`bytes`）在同一页面内容下可重复：本次两次
  连续运行完全相同。页面内容轮换（bilibili 会个性化/轮播）会带来 ±1–2 请求、
  ±1 MB 的差异。
- **相位时间**是波动的（网络与机器负载），只作证据，**不作门禁**——见下"纪律"。
- 相位语义见 `src/browser/include/neko/browser/perf_report.h`。`wall_ms` 含截图
  光栅化（因此 `wall_ms − Σphases` 大致就是截图时间）。

## GUI 路径（帧与泵）

```bash
QT_QPA_PLATFORM=offscreen ./build/debug/bin/neko_gui_screenshot \
    https://www.bilibili.com /tmp/shot.png --perf-report
```

它打印与 CLI 相同的 JSON 行（`frames` 段才有意义；计数器来自 GUI 路径自己走的
`SharedHttpClient`）。**使用前先看这三点限制**：

1. 该工具只跑到页面"就绪"（内容 + 非 loading + 首帧）为止，**不是稳态 FPS 测量**；
   其就绪循环上限 200×50 ms = 10 s，重载页面可能超时——超时会向 stderr 打印
   `page did not settle within 10 s`，此时的报告描述的是**未完成的加载**（实测
   出现过 `requests=35`、`frames=0` 的这种样本，勿当结论）。
2. **`pump_turns` 在该工具里恒为 0**：它用 `processEvents()` 驱动，不像真 GUI 那样
   每 50 ms 触发 `PumpScriptTimers`。所以 GUI 的泵时长目前**测不到**——这是已知缺口，
   需要一个"驱动 N 秒泵并统计帧/泵分布"的稳态 harness（下一项工作）。
3. 帧数只覆盖加载窗口内的产出。

实测（debug，bilibili，就绪样本）：`frames 2–3`、帧间隔 p50 ≈ 0.8–1.7 s、
单帧产出最长 750–780 ms、`frame_over_50ms ≥ 1`——即**加载期间内容只在秒级粒度上更新**。

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
| **post_load** | ~4.3 s | **1.46 / 5.26 s** | **4.06 / 4.16 s** |
| ├ pump（定时器/事件泵） | （未拆分） | **1.23 / 4.60 s** | （未单独测） |
| └ late_images（晚到图片轮次） | （未拆分） | **0.23 / 0.67 s** | （未单独测） |
| 单次泵动作最长 | （未测量） | **0.65 / 4.01 s**（`pump_max`） | 4.73 s（debug） |
| XHR 类请求 | 33 个 / 12.9 s / 跨度 19.6 s / 每个 ~0.51 s | **32 个 / 3.6–5.3 s / 跨度 7.8 s / 中位 24–102 ms** | （含在 scripts + post_load） |

关键结论（写入 ADR 0022 修订与 Council memo）：

1. **P0 的连接复用直接把 XHR 变便宜**：31/32 个 XHR 骑在热连接上，单请求从
   ~0.51 s 降到中位 24–102 ms。**"可回收 12.9 s"这一说法在 HEAD 上已过期**，
   P1 异步化的收益必须重新表述（上限 = Σ(耗时) − 最长单个请求，实际未知）。
2. `scripts` 仍是最大单项（5.2–6.9 s）——剩余部分是脚本体抓取后的**执行**，不是传输。
3. **泵里的长任务归因（2026-10 实测，debug，两次运行）**：
   `pump_turns ≈ 12`、`pump_max ≈ 2.7–2.9 s`，而 breakdown 显示该轮是
   **`raf = 38` 个回调、`raf_ms ≈ 2.5–2.7 s`**；`timers = 95`（0.35–0.37 s）、
   `job_rounds = 95`（0.10 s）、**`xhr = 8` 且 `xhr_ms = 0`**、ports/websockets 为 0。
   即：**此前"32 个 XHR 完成回调挤在同一轮"的猜测被实测否证**——XHR 投递几乎不耗时，
   昂贵的是 rAF 回调。
   重要限定：headless CLI 的泵**没有帧节拍**，加载期间累积的 rAF 回调会在**同一轮被一次排空**
   （38 个一次跑完），因此这个"2.7 s 单轮"至少部分是 **headless 路径的产物**；真 GUI 每
   50 ms 泵一次、每轮只排空少量回调。真 GUI 的泵时长仍缺稳态 harness（见上文限制 2）。
   可操作方向：(a) 给 headless 泵也加"每轮 rAF 上限"（模仿帧预算，让轮与轮之间能推进），
   (b) 建 GUI 稳态 harness 后再判断交互路径的真实分布。
   （与第 5 条的脚本段是**两笔独立开销**：见上表，泵与脚本段在相位里分开计量。）
4. **`post_load` 已拆分，它的代价几乎全在泵里**：release 两次实测
   `pump = 1.23 / 4.60 s` 对 `late_images = 0.23 / 0.67 s`，而且泵内**存在单轮
   0.65 / 4.01 s 的动作**（`pump_max`，`pump_over_50ms = 2`）——一次 JS 回调链
   就把 worker 独占数秒。**这是目前测得的最大流畅性问题**，量级超过阻塞式
   XHR（32 次 × 中位 24–102 ms），而 P0 完全没碰它。注意它与墙钟的波动同源：
   两次运行的 `scripts` 是 2.6 s 与 10.0 s，说明慢的那部分在 JS 抓取/执行，
   而不是传输。
5. **脚本阶段的绝大部分在 `scripts_modules`**（拆分后实测；release 例：
   `scripts = 8518 ms`，其中 modules 7346、fetch 985、restyle 129、exec 51、events 0）——
   即瓶颈是那个约 1.6 MB 的 bundle，而不是经典脚本体或传输。
   **但必须说清该字段的口径**：`scripts_modules` 是**混合量**——模块入口抓取 +
   `EvaluateModule`（其中还会抓取该模块的 import），它**不区分网络与 CPU**。
   本页的 bundle 没有 import，因此这里几乎全是求值；但对 import 密集的页面，该字段会被
   I/O 抬高，**不能**用它单独支撑"贵在求值不在网络"的结论。真要断言，需要把
   `module_fetch`/`module_eval` 拆开——留到字节码缓存那一步一起做。
   候选优化仍是**字节码缓存**（首访后 `JS_WriteObject` 落盘、后续 `JS_ReadObject`
   跳过解析+编译）与降低绑定开销。
6. 差异说明：`Σ(XHR 耗时) = 3.6 s` 却占 **7.8 s 跨度** → 剩余时间**不在传输**
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
- **`phases.scripts_*` 只在 CLI 的进程内加载路径填写**（与 `phases` 同源）：GUI 帧报告、
  渲染子进程、网络子进程模式下它们恒为 0，此时 0 的含义是"该路径未测量"，不是"没耗时"。
- `bytes` 是**解码后**的响应体大小（含字体/图片/脚本），不含 HTTP 头。
- 流畅性指标现在**部分**覆盖：泵轮时长（CLI 可用）与帧产出/间隔（GUI 工具可用）；
  仍缺：稳态 FPS、输入→帧延迟、以及 GUI 泵时长（见上文限制 2）。
- 泵指标按"轮"聚合，**不区分**是定时器回调还是 XHR 完成回调；要归因到具体回调需要
  在 binder 里按任务类型分别计时（后续）。
- 尚无按资源类别拆分的字节统计（本页字体 108 文件 / 9.6 MB 这类数字仍来自临时
  插桩），拆分统计列为后续。
