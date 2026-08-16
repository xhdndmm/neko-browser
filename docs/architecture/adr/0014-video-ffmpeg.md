# 架构决策记录 0014：视频解码采用 FFmpeg（LGPL）

- 状态：**Accepted**（2026-08）
- 决策者：架构组

## 背景

Web 视频需要容器解复用（MP4/WebM/…）与编解码器（H.264/VP8/VP9/…），
每个编解码器都是数千行的子系统（熵解码、逆变换、运动补偿、环路滤波）。
依赖政策 §2 允许"图形、压缩等通用基础设施"使用成熟三方库，且 §5 的
"不重造安全关键轮子"同样适用于编解码器：自研 H.264 解码器既不现实，
也没有架构价值（本项目目标是浏览器引擎，不是多媒体编解码库）。

## 决策

- 使用 **FFmpeg 6.x**（`libavformat` 解复用 + `libavcodec` 解码 +
  `libswscale` 像素格式转换 + `libavutil`），通过 `cmake/FindFFmpeg.cmake`
  （pkg-config）以系统包方式引入；CI 三平台显式安装 dev 包
  （Debian/Ubuntu: `libavformat-dev libavcodec-dev libavutil-dev
  libswscale-dev`）。
- 封装在 `neko::media` 之后，FFmpeg 头文件**不越过** `src/media/src/
  video.cpp`（同 QuickJS 的 SYSTEM PRIVATE 处理）：
  - `MediaSource::Open(data)`：内存字节直接进 FFmpeg（AVIOContext 读回调，
    零拷贝语义的输入副本 + `AV_INPUT_BUFFER_PADDING_SIZE` 尾零），解复用、
    定位视频流、打开解码器；`NextFrame()` 按呈现顺序给出
    `VideoFrame{pts, image::Image(RGBA, top-down)}`。
  - `DecodeVideo(data, max_frames, max_total_bytes)`：全量解码便利接口，
    带帧数/字节预算（恶意输入不会打爆内存）。
  - 音频轨道被忽略（不解码、不抽取）；解码帧统一转 8-bit RGBA。
- 错误统一包装为 `base::Error`（`av_strerror` 文本 + 解析失败归
  `kParse`）；FFmpeg 类型、时间基、`AVRational` 等不泄漏出模块。
- `<video>` 元素接入（fetch → MediaSource → 帧泵播放）是本决策的消费方，
  作为后续里程碑实现；本轮交付解码核心 + CLI（`--video-info/--video-out`）
  + 单元测试。

## 备选方案

- **GStreamer**：插件化架构更重（glib 依赖、管道概念），对"解码出帧"
  这一单一需求过于庞大；FFmpeg 的 C API 更直接。
- **libvpx/libx264 等单编解码库**：需要自己实现/组合容器解复用，
  覆盖格式越多代码越多；FFmpeg 一个依赖覆盖 MP4/WebM/Matroska 与
  H.264/VP8/VP9/AV1 等。
- **WebCodecs（多进程后由 renderer 委托）**：当前单进程架构无宿主；
  未来多进程渲染器可切换，MediaSource 缝保留迁移路径。
- **自研解码器**：明确不做（项目目标是浏览器引擎）。

## 许可证

- FFmpeg 为 **LGPL-2.1-or-later**（Ubuntu 发行版构建，`--enable-gpl`
  关闭的默认配置不含 GPL 组件）。LGPL 要求：本项目动态链接
  （`PkgConfig::FFmpeg` 默认链接共享库）且不修改 FFmpeg 源码，即可满足
  ——与 Unlicense 项目兼容；若未来需要静态链接发行，需提供重链接目标
  文件（LGPL §4）。禁止使用 `libx264`（GPL）等 GPL 编解码器构建。

## 后果

- 优点：一个依赖覆盖主流容器+编解码器；解码核心 ~350 行封装即完成；
  内存输入支持（无需落盘）；预算钳制防恶意输入。
- 缺点：新增系统包依赖（CI 三平台需安装 dev 包）；FFmpeg 6.x API 在
  主版本间有破坏性变更，升级需跟随适配；LGPL 动态链接约束（文档已记）。

## 参考

- https://ffmpeg.org/doxygen/6.1/group__lavf__decoding.html
- https://ffmpeg.org/doxygen/6.1/group__lavc__decoding.html
- https://ffmpeg.org/legal.html
