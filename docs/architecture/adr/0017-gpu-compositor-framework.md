# 架构决策记录 0017：GPU 合成框架（设备抽象 + 回退）

- 状态：**Accepted**（2026-09）
- 决策者：架构组

## 背景

ADR 0015 建立了 `Compositor` 缝并明确"GPU 实现可以复用同一接口"，
AGENTS.md §23 要求长期支持 OpenGL/Vulkan/Metal/Direct3D 并"使用图形
抽象层"。经过 ADR 0015 之后，合成层的接口已稳定，但 GPU 侧仍然只有一句
承诺：没有设备抽象、没有能力探测位置、没有"无 GPU 时怎么办"的成文决策。

当前开发环境（Ubuntu 虚拟机）没有 GPU，但架构必须现在就把 GPU 路径的
形状定义清楚，否则未来接入时会把设备代码散落到 UI/合成器内部。

## 决策

- 新增两条 GPU 侧边界（`neko::compositor`，与软件合成器同模块）：
  1. **`GpuContext`**：设备抽象，刻意限定为合成器实际需要的最小操作集——
     `ResizeSurface`、`CreateTexture/UploadTexture(子矩形)/DestroyTexture`、
     `BeginFrame(rgba)/Draw(GpuDrawCall)/EndFrame`、`Readback`（无头截图与
     逐像素对比用）。纹理是不透明的 `uintptr_t` 句柄；不暴露任何具体图形
     API 的类型。线程约束：创建它的线程独占使用。
  2. **`GpuCompositor`**：`Compositor` 接口的 GPU 前端的**真实实现**——
     图层簿记、脏矩形、滚动暴露带、CPU 镜像像素（保证输出在各种情况下
     都正确）全部实现；每帧对脏图层执行上传、对每个可见图层发出一次
     textured-quad draw（`GpuDrawCall`）。
- **能力探测集中在 `ProbeGpuCapabilities()`**：返回 `available/backend/
  renderer/max_texture_size/hardware_accelerated` 等。**当前实现恒返回
  `available = false`**：没有平台后端时谎报可用，会让合成器走上一条
  无法呈现帧的路径，比软件回退更糟。
- **回退是显式决策而非降级**：`GpuCompositor::Create()` 在无可用设备时
  返回 `SoftwareCompositor`（ADR 0015 的真实实现）。`force_software` 参数
  供测试固定行为。`NullGpuContext` 用于"有设备句柄但无设备"的中间态：
  接口完整、设备操作返回 **NOT IMPLEMENTED**（诚实而非静默 no-op）。
- **可测试性是设计的一部分**：`RecordingGpuContext` 记录纹理内容与 draw
  call 账本，使"上传了哪些像素、在什么位置画了几次"能在**没有 GPU 的 CI**
  上被断言；`GpuPathOutputMatchesSoftwareReference` 断言 GPU 路径的呈现
  输出与软件合成器**逐像素一致**（加速不是行为改变）。
- **明确未实现**：平台后端（GL 3.3/Vulkan 1.1/Metal/D3D11）、swapchain
  呈现、共享内存回读路径。这些不会伪装：探测恒 false，`NullGpuContext`
  的失败路径有单元测试锁定（PASS/FAIL 语义清晰）。

## 后果

- 正面：
  - GPU 接入点唯一（`GpuContext` 工厂 + 探测函数），未来实现不需要触碰
    合成器、UI 或光栅化器。
  - 无 GPU 环境（本机、CI）行为完全确定，且 GPU 路径的簿记逻辑已被
    9 个单元测试覆盖。
  - "加速不改变像素"被测试强制，避免未来 GPU 后端引入视觉回归。
- 负面 / 限制：
  - 设备路径未经真实硬件验证（本环境无 GPU），`GpuDrawCall` 的形状
    （纹理句柄 + 目标矩形 + 不透明度）可能在真实后端接入时调整。
  - 脏矩形上传目前是整层上传（`texture_dirty` 粒度到层），子矩形上传
    原语已在 `GpuContext` 中预留。
- 中立：
  - 合成器输出仍是 CPU `Surface`（UI 走 QImage blit 呈现）；"GPU 呈现到
    窗口"属于后端接入时的后续决策（需要把输出表面换成 swapchain 镜像
    共享内存）。

## 相关

- ADR 0015（软件合成器抽象层）：`Compositor` 接口与图层模型。
- ADR 0016（多进程架构）：将来 GPU 进程化时，本 ADR 的 `GpuContext` 就是
  进程内客户端接口的自然拆分点。
- `docs/compatibility/compatibility-matrix.md`：GPU 合成行（Partial，
  平台后端未实现）。
- `src/compositor/include/neko/compositor/gpu_compositor.h`、
  `gpu_context.h`。
