# 架构决策记录 0019：发布产物运行时打包（静态链接 + 运行库捆绑）

- 状态：**Accepted**（2026-09）
- 决策者：构建/发布工程

## 背景

0.1.0 的发布产物是纯动态链接：Linux/macOS 用户必须先安装 Qt6、FFmpeg、
OpenSSL 等运行库才能启动 GUI，Windows 产物只捆绑了 Qt，连 vcpkg 的 DLL
（zlib1.dll、libcrypto、avcodec 等）与 VC++ 运行库都没有随包，干净机器上
CLI 也无法运行。发布产物的目标是**下载即用（零安装）**。

约束（决定了方案边界）：

- Qt6 官方二进制（apt / Homebrew / Qt 官方预编译包）**只提供动态库**；
  静态 Qt 需要从源码构建，或属于商业授权构建。
- **macOS 不存在完全静态**：系统框架与 libSystem 必须动态链接。
- **Linux 全静态不可行**：glibc 的 NSS（DNS 解析、系统证书等）依赖运行期
  动态加载，静态 glibc 会破坏浏览器最核心的网络行为。
- **FFmpeg 保持动态**：LGPL 静态链接附带重链接义务，且发行版/Homebrew
  构建可能包含 GPL 组件（见 ADR 0014 与依赖政策）。

## 决策

按平台采用“能静态则静态，其余随包捆绑”，并在打包阶段强制校验。

### Windows（除 Qt/FFmpeg 外全部静态链接）

- vcpkg 使用 `x64-windows-static-md` / `arm64-windows-static-md`
  （静态库 + **动态 CRT**）：zlib、libjpeg-turbo、libwebp、FreeType、
  OpenSSL、libavif（含 dav1d/libyuv）全部静态链接进可执行文件。
  选择 `static-md` 而非 `static`（/MT）是因为官方 Qt DLL 为 /MD；
  跨 CRT 混用在 Windows 上属于不受支持区域，而动态 CRT 的运行库
  （vcruntime140/msvcp140）随包分发同样满足零安装。
- **FFmpeg 是唯一例外**：保持动态链接（LGPL 重链接义务，ADR 0014），
  从动态三元组（`x64-windows` / `arm64-windows`）单独安装，DLL 随包
  分发；CMake 侧通过 `FFMPEG_ROOT` 指向该三元组的安装前缀
  （`cmake/FindFFmpeg.cmake`）。
- Qt 保持官方动态库，`windeployqt`（原生）或固定清单（ARM64 交叉包没有
  部署工具）随包；MSVC 运行库从 VS 的 `Redist/MSVC/*/<arch>/Microsoft.VC*.CRT`
  拷贝，不要求用户安装 Redistributable。
- 打包后校验（`release.yml` → `scripts/release/verify-package-windows.ps1`）：
  导入表只允许引用包内文件或系统 DLL；
  被静态链接的依赖（zlib1/libcrypto/avif/webp/freetype/...）不得再以 DLL
  形式出现；Qt/FFmpeg/CRT 必须在包内。

### Linux（捆绑全部非系统库 + RPATH 改写）

`tools/package_runtime_linux.sh`：递归收集两个可执行文件与 Qt 插件的
共享库闭包，复制到 `lib/`，并把 RPATH 改写为 `$ORIGIN` 相对路径
（`bin -> $ORIGIN/../lib`、`lib -> $ORIGIN`、`plugins -> $ORIGIN/../../lib`）；
Qt 插件（platforms/imageformats）复制到 `plugins/`，`bin/qt.conf` 指向它。

**不捆绑**：glibc 家族（`ld-linux*`、`libc`、`libm`、`libpthread`、NSS
相关）与 GPU/驱动栈（`libGL/libEGL/libdrm/...`）——前者是主机 ABI 的一部分
（DNS、系统证书），后者必须与目标机内核驱动匹配。

### macOS（GUI 打成 .app + macdeployqt；CLI 改写 install name）

`tools/package_runtime_macos.sh`：

- GUI 组装为 `neko_browser_gui.app`（脚本写 `Info.plist`），由
  `macdeployqt` 部署 Qt framework、Qt 插件，**以及非 Qt 的 dylib**
  （FFmpeg/OpenSSL/...）；脚本再补一个 offscreen 平台
  插件供 headless 冒烟。`dylibbundler` 之类工具明确跳过 `.framework`，
  无法处理 Homebrew 的 Qt，故选官方工具链。
- CI 只安装 Homebrew 的 `qtbase`（GUI 仅用 Qt6 Widgets，测试用 Qt6 Test）。
  聚合包 `qt` 会把 qtwebengine（QtPdf）、qtvirtualkeyboard、qtsvg、
  qtdeclarative 等模块的插件放进共享插件目录，而 `macdeployqt` 会**无条件**
  部署 `iconengines`/`platforminputcontexts`/`imageformats` 中的插件、却
  解析不到分散在其他 keg 的 framework → “Cannot resolve rpath”，产物依赖
  校验失败（2026-09 rc2 macOS 发布失败复盘）。
- ad-hoc 签名由脚本自己完成：`macdeployqt` 自带的签名在含 Homebrew 依赖树
  的 bundle 上会半途失败，且从不封 `.app` 本身。脚本在部署完成后关闭它
  （`-no-codesign`，旧版本无此选项则忽略其结果），从内到外重签 bundle 内的
  所有 Mach-O，再签 bundle 根并用 `codesign --verify --deep` 校验。
- CLI 不含 Qt：脚本自行解析 `otool -L` 闭包，把非系统 dylib 复制到 `lib/`，
  引用改写为 `@executable_path/../lib/...`，重签并且清理指向 Homebrew
  的 LC_RPATH。
- 打包后校验按 dyld 语义解析每个非系统依赖（`@rpath` 查 LC_RPATH，
  `@loader_path`/`@executable_path` 按文件位置展开），要求解析结果落在包内；
  系统库（`/usr/lib`、`/System`）永不复制：macOS 上它们无法静态且必须与
  主机一致。

### 通用

- 打包后立即用**打包产物**跑冒烟测试（CLI `--dump-dom` 本地页面；GUI 以
  `QT_QPA_PLATFORM=offscreen` 存活一段时间且无平台插件加载失败），并清空
  `LD_LIBRARY_PATH`/`DYLD_LIBRARY_PATH`，防止借用到构建机上已安装的库。
- 本地开发构建（`--preset debug` 等）不受影响，仍使用系统动态库；
  静态三元组只用于发布流水线与 `FFMPEG_ROOT` 显式指定的场景。

## 备选方案

- **全静态（含 Qt）**：需要 CI 从源码静态构建 Qt（单平台 +30~90 分钟，
  失败风险高），且要处理 LGPL 静态合规；macOS 系统框架与 Linux glibc/NSS
  的限制依旧存在。否决。
- **Linux AppImage / macOS .dmg 等打包格式**：需要引入额外打包工具链；
  现有“tarball + rpath 改写”已能用自带脚本验证，后续有需要再演进。否决（暂）。
- **Windows 保持动态 + 捆绑全部 vcpkg DLL**：用户同样零安装，但 DLL 数量多
  （FFmpeg 及其依赖链条），漏拷一个就运行失败；静态链接把这类错误提前到
  链接期，是更稳的形态。否决。
- **仅文档要求用户安装依赖**：等于不解决用户痛点。否决。

## 后果

- 正向：三平台发布产物下载即用；Windows 包内除 Qt/FFmpeg/CRT 外不再有
  第三方 DLL；Linux/macOS 的依赖解析在打包时被脚本校验（失败即不产出
  产物）。
- 代价：包体积显著增大（Linux/macOS 含 Qt 与 FFmpeg 运行库，tar.gz 数十
  MB）；发布 CI 增加少量打包/冒烟时间；Linux 产物的 glibc 基线等于构建
  runner（Ubuntu 24.04）。
- **已知合规注意事项**：Linux/macOS 捆绑的 FFmpeg 运行库来自发行版/Homebrew
  构建，可能包含发行版启用的 GPL 组件；后续工作是为发布构建自有 LGPL
  运行时（FFmpeg 最小特性集），并同步更新依赖政策。
- 未覆盖（后续工作）：代码签名/公证、独立调试符号包、Windows 静态 CRT
  （/MT）形态。
