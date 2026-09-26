# release.yml：Windows 配置（VS 生成器 + vcpkg 静态三元组 + 动态 FFmpeg）。
#
# 目标平台必须用命令行 -A 指定：CMAKE_GENERATOR_PLATFORM 环境变量只有在
# CMAKE_GENERATOR 环境变量同时设置时才被应用（CMake 文档明文），单独设置会被
# 静默忽略、VS 生成器回退到宿主默认平台（x64）——windows-arm64 任务曾因此
# 实际按 x64 构建，链接 arm64 的 vcpkg/Qt 导入库时爆出整墙 LNK2001（x64 任务
# 因默认平台恰好正确而未受影响）。release preset 未固定 generator /
# architecture，-A 与 --preset 可并用（已在 CMake 4.4 上验证该组合被接受并生效）。
#
# FFmpeg 不在静态三元组里：用 FFMPEG_ROOT 指向动态三元组的安装前缀，
# FindFFmpeg 会优先在那里搜索头文件与导入库（其余依赖自动来自 vcpkg 的
# 静态前缀）。
#
# 环境（由前面的步骤写入）：VCPKG_ROOT、QT_HOST_PREFIX（arm64 交叉编译时需要）
param(
  [Parameter(Mandatory)][string]$Arch,
  [Parameter(Mandatory)][string]$GeneratorPlatform,
  [Parameter(Mandatory)][string]$VcpkgTriplet,
  [Parameter(Mandatory)][string]$VcpkgDynamicTriplet
)
$ErrorActionPreference = 'Stop'

# Git Bash 下 cygpath -m 的等价物：CMake 同样接受 C:/... 形式。
function ConvertTo-CMakePath([string]$Path) { return $Path -replace '\\', '/' }

$vcpkgRoot = ConvertTo-CMakePath $env:VCPKG_ROOT
$cmakeArgs = @(
  '--preset', 'release',
  '-A', $GeneratorPlatform,
  '-DNEKO_WARNINGS_AS_ERRORS=ON',
  "-DCMAKE_TOOLCHAIN_FILE=$vcpkgRoot/scripts/buildsystems/vcpkg.cmake",
  "-DVCPKG_TARGET_TRIPLET=$VcpkgTriplet",
  "-DFFMPEG_ROOT=$(ConvertTo-CMakePath "$env:VCPKG_ROOT/installed/$VcpkgDynamicTriplet")"
)
if ($Arch -eq 'arm64') {
  $cmakeArgs += "-DQT_HOST_PATH=$(ConvertTo-CMakePath $env:QT_HOST_PREFIX)"
}

cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "cmake 配置失败（exit $LASTEXITCODE）" }
