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
{"wall_ms":5276,"requests":181,"connections_opened":50,"connections_reused":131,
 "connections_discarded":27,"bytes":20340849,
 "phases":{"document":875,"stylesheets1":176,"fonts1":600,
           "scripts":2119,"scripts_fetch":562,"scripts_exec":52,
           "scripts_modules":1372,"scripts_modules_wait":723,"scripts_modules_eval":649,
           "scripts_events":0,"scripts_restyle":131,"sync_fetch_calls":26,
           "stylesheets2":141,"fonts2":0,"images":148,"videos":0,
           "pump":959,"late_images":213,"post_load":1172},
 "pump_turns":4,"pump_over_50ms":1,"pump_total_ms":761,
 "pump_p50":0,"pump_p95":737,"pump_max":737,
 "pump_breakdown":{"timers_ms":71,"timers":71,"jobs_ms":3,"job_rounds":71,
                   "xhr_ms":0,"xhr":6,"raf_ms":682,"raf":38,
                   "ports_ms":0,"ports":0,"websockets_ms":0,"websockets":0},
 "frames":0,"frame_over_50ms":0,"frame_production_p50":0,"frame_production_p95":0,
 "frame_production_max":0,"frame_interval_p50":0,"frame_interval_p95":0,
 "frame_interval_max":0}
```

> 上面是**一次真实运行**（release，bilibili）的完整一行，字段互相对得上：
> 脚本五段之和 2117 ms 对 `scripts = 2119 ms`（残差 2 ms），
> `post_load = pump + late_images`（959 + 213 = 1172）。
> **不要把不同运行的字段拼在一起**：这段曾经就是把拆分后的键粘进拆分前的 `scripts`
> 基线，凭空造出 540 ms 的"缺口"。


> 上面是**一次真实运行**（release，bilibili）的完整一行，字段互相对得上：
> 脚本五段之和 3373 ms 对 `scripts = 3375 ms`（残差 2 ms），
> `post_load = pump + late_images`（1244 + 222 = 1467）。
> **不要把不同运行的字段拼在一起**：这段曾经就是把拆分后的键粘进拆分前的 `scripts`
> 基线，凭空造出 540 ms 的"缺口"。

> 上面是**一次真实运行**（release，bilibili）的完整一行，所有字段互相对得上：
> 脚本五段之和 8511 ms 对 `scripts = 8518 ms`（残差 7 ms），
> `post_load = pump + late_images`（4459 + 720 = 5179）。**不要把不同运行的字段拼在一起**：
> 这段曾经就是把拆分后的键粘进拆分前的 `scripts` 基线，凭空造出 540 ms 的"缺口"。

**`scripts_*` 把脚本阶段拆开**（此前只有一个 `scripts`，无法判断该优化什么）：
`scripts_fetch`＝阻塞等经典脚本体；`scripts_exec`＝`Evaluate`（解析+编译+执行）；
`scripts_modules`＝模块脚本（`EvaluateModule` 及其 import 抓取）；`scripts_events`＝
`DOMContentLoaded`/`load` 处理器；`scripts_restyle`＝脚本后引擎重跑的全量样式级联。
`scripts_modules` 拆成 `scripts_modules_wait` 与 `scripts_modules_eval`。**口径要说准**：
`scripts_modules_wait` 是**模块段里 JS 线程阻塞在 `apis.fetch` 的总时间**——既包含模块加载器的
import，也包含页面自己在这个阶段发出的 `fetch()`（两者都在 JS 线程上同步等网络，这才是代价）；
`sync_fetch_calls` 是这类请求的次数。把它叫作"等模块源码"是不准确的，而实测正好暴露了这点
（见下文"模块预取的负面结果"）。

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

（`--raf-per-turn` 与泵统计对**两条路径都生效**：CLI 走浏览器层的泵函数，GUI 走
`BrowserController::PumpScriptTimers`，它直接调用 `RunPendingTimers`——两处都接上了。）

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
   每 50 ms 触发 `PumpScriptTimers`。要看 GUI 的泵与帧分布请用下面的**稳态 harness**。
3. 帧数只覆盖加载窗口内的产出。

实测（debug，bilibili，就绪样本）：`frames 2–3`、帧间隔 p50 ≈ 0.8–1.7 s、
单帧产出最长 750–780 ms、`frame_over_50ms ≥ 1`——即**加载期间内容只在秒级粒度上更新**。

## GUI 稳态 harness（`neko_gui_perf`）

```bash
./build/release/bin/neko_gui_perf <url> [--seconds N] [--interval-ms N] \
    [--raf-per-turn N] [--profile <dir>] [--quiet]
```

它**先有界等待页面安定**（默认 15 s，未安定会向 stderr 告警），再按 GUI 自己的节拍
（每 50 ms 泵一次 + 处理 Qt 事件）驱动 N 秒，最后打印与 `--perf-report` **同一行 JSON**
（外加一行人类可读摘要）。它不创建窗口：`NewTab` 就够给控制器一个活动标签，而没有
MainWindow 就没有隐藏的 50 ms 定时器与本 harness 抢节拍——所以 `pump_turns` 精确等于
本工具驱动的轮数。

读什么：
- `frames` / `frame_production_p50|max`：出帧数量与**单帧成本**；
- `frame_interval_p50|p95|max`：**帧节奏**（交互感受的主信号）；
- `pump_turns` / `pump_max` / `pump_breakdown`：哪一轮阻塞、那一轮跑了什么。

实测（release，bilibili）：安定耗时 8.7 s；随后 4 s 窗口 **22 帧**、单帧 p50 5 ms /
最长 180 ms、**帧间隔 p50 188 ms / max 231 ms**、泵 24 轮、最长一轮 **898 ms**。
即该页稳态内容更新约 5 帧/秒，且存在接近 1 秒的单轮停顿。

### 用它做 rAF 预算 A/B：第一次结论是"不确定"

3 档 × 2 轮（release，bilibili，4 s 窗口）：

| budget | frames | interval max | 最长泵轮 | pump_total |
| --- | --- | --- | --- | --- |
| 0（默认） | 24 / **0** | 311 / — | 1028 / **3873** | 1063 / 3873 |
| 2 | 17 / 13 | 251 / 1141 | **64** / 1016 | 360 / 2013 |
| 8 | 3 / 4 | 784 / 1039 | 1033 / 1068 | 1769 / 2069 |

同一档内两次运行差异极大（真实站点内容轮换 + 懒加载），**2 轮采样不足以判定**：budget=2
一次给出 64 ms 的最长轮，另一次 1016 ms。要得到结论需要：(a) **本地固定夹具**（已知数量与
已知工作量的 rAF 回调）而非真实站点；(b) 每档 ≥8 次采样；(c) 把判定落在**确定性计数**
（每轮回调数、总回调数）而不是帧计时上。因此 rAF 预算**保持默认关闭**，本 harness 是把
它变成可判定问题的工具。

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
5. **脚本阶段拆到底：模块段 ≈ 同步网络等待 + 编译执行**（release，例：`scripts = 2119 ms`
   → `scripts_modules = 1372 ms` = wait 723 + eval 649，
   `sync_fetch_calls = 26`；两次运行里 wait 占模块段 25%–53%）。
   三处曾被说错的地方现在都定了：
   - **不是**"瓶颈全在 QuickJS 求值"：纯 QuickJS 解析+编译同一个 1.57 MB bundle 只要 **235 ms**
     （`qjsc`），所以 eval 的一半主要是**执行**框架代码，不是编译。
   - **不是**"全在等网络"：那是 gdb 采样恰好落在抓取窗口里。
   - `scripts_modules_wait` **也不只是模块源码**：它含页面自己的 `fetch()`。
6. **模块并行预取：试过，是负面结果**（记录以免后人重走）。
   做法与 P0 给经典脚本的预取同源：求值前并行爬取模块图（入口 → 扫描静态 `import` → 并行取 → 递归，
   有界），加载器优先吃缓存。实测**只爬到 1 个源，而该阶段实际有 26 次同步 fetch**：bilibili 的
   主 bundle 里 **`import(` 只出现 1 次且是计算式**（`import("_"...)`），**没有任何静态 import 字面量**
   ——现代打包器（Vite/Rollup 风格）用计算式动态 import，静态扫描按定义扫不到。
   结论：预取对这类页面无效，已撤销该实现（不保留"默认关闭的旋钮"这种半成品）。
   **真正能动这块的是让 `apis.fetch`/XHR 的传输不再阻塞 JS 线程**（即一直记录在案的同步传输近似，
   见兼容矩阵的 XMLHttpRequest 行）——这是下一步，而不是继续在预取上做文章。
   附带收获：这条实验暴露了一个真实缺陷——`apis.fetch` 这个回调**活得比 `RunPageScripts` 长**
   （页面加载后的 `fetch()`/XHR 仍会调用它），而它按引用捕获了函数内的栈变量，属于悬垂引用
   （UB）。已改为共享所有权，并加了"加载后仍可 fetch"的回归测试。
7. 差异说明：6. 差异说明：`Σ(XHR 耗时) = 3.6 s` 却占 **7.8 s 跨度** → 剩余时间**不在传输**
   （JS 计算，或页面自己在等依赖）。这正需要 fixture A/B 与 send() 站点插桩来判定。

## 每轮 rAF 预算的 A/B（机制已实现，默认关闭）

同一页面交错三次、每档三采样（release，`--raf-per-turn`，0 = 旧行为）：

| budget | pump 轮数 | `pump_max` 中位 | `pump_total_ms` | wall |
| --- | --- | --- | --- | --- |
| **0（默认）** | 6–8 | **1128 ms** | 1062–1703 | 8.7–10.0 s |
| 8 | 8–12 | 912 ms | 984–3325 | 6.9–15.9 s |
| 2 | 21–28 | **793 ms** | 1072–1350 | 8.1–15.5 s |

读法：38 个回调在三种配置下**各只执行一次**（`raf=38` 恒定），所以这是纯粹的**交错**差异。
预算把最长一轮压掉约 30%，但**总轮时可能翻倍**——同一批回调被摊开后，页面在批次之间反复
重算布局/样式。墙钟没有改善（方差内甚至更差）。因此**默认保持 0（无限）**，`--raf-per-turn`
作为实验旋钮保留；是否值得开启要看"帧是否真的推进"，这正是 GUI 稳态 harness 要回答的问题。

## HiDPI：文字模糊的原因与修法（2026-10）

现象：HiDPI 屏幕上整页发虚，文字最明显。原因**不是**字体光栅化，而是**整帧被放大**：

- 全仓此前**没有任何 `devicePixelRatio` 处理**；
- `WebView` 用**逻辑像素**（`viewport()->width()`）作为视口 → 帧按逻辑尺寸光栅化；
- HTML 帧又是 1:1 画进窗口 → Qt 只能把 1100×800 的帧拉成 2200×1600 的设备像素。

修法：引入**设备缩放**这一与 zoom 独立的比例，并让所有换算只走一个访问器
（`Page::frame_scale() = zoom × device_scale`）：

- `Page::SetDeviceScaleFactor()`：帧 = CSS × zoom × 设备缩放，而 **CSS 视口不变**
  （`viewport_css_width()` 除回去）；
- GUI 上报**物理像素**视口（逻辑 × DPR），并给 `QImage` 设 `devicePixelRatio`，
  于是 Qt 1:1 绘制而不是放大；滚动范围/鼠标/光标/查找高亮按同一比例换算；
- 渲染进程标签（`--renderer-process`）**仍是 1×**：子进程渲染尺寸由会话协议决定，
  转发 DPR 需要改协议，列为后续。

排除项（不是原因）：字形位置已取整到整数像素（更锐，不是糊的原因）；页面 zoom 在
**布局期**生效，不是缩放位图；HTML 帧没有被我们缩放。

验证：`PageTest.DeviceScaleFactorScalesTheFrameButNotTheCssViewport` 断言"帧随设备缩放、
CSS 视口不变、命中测试一致"（确定性）；`QT_SCALE_FACTOR=2` 下实测 GUI 上报
816×716 逻辑 → **1632×1432 帧**（修复前是 816×716 再被 Qt 放大），文字区"核心黑/中间灰"
从 1.39 升到 1.70。**诚实边界**：offscreen 的 `grab()` 自身也会缩放窗口渲染，所以端到端的
锐度需要在一块真实 HiDPI 屏幕上最终确认（在 GUI 里看文字边缘即可）。

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
