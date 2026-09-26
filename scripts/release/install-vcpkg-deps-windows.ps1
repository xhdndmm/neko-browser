# release.yml：Windows 依赖安装（vcpkg）。
#
# libavif 默认不启用任何 AV1 解码器（vcpkg 端口无 default-features），必须
# 显式选择 dav1d，否则 AVIF 解码在运行期返回 "No codec available"
# （2026-09 windows 发布的 AvifTest 失败原因）。
#
# 分两次安装：无 LGPL 问题的依赖（zlib/jpeg/webp/freetype/openssl/avif）
# 走静态三元组（环境变量 VCPKG_DEFAULT_TRIPLET），包里不再有对应 DLL；
# FFmpeg 单独装进动态三元组（LGPL 要求保持动态链接，见 ADR 0014/0019），
# DLL 在打包阶段随包分发。
#
# 环境（由 workflow 步骤设置）：VCPKG_DEFAULT_TRIPLET、VCPKG_BINARY_SOURCES
param(
  [Parameter(Mandatory)][string]$DynamicTriplet
)
$ErrorActionPreference = 'Stop'

vcpkg install zlib libjpeg-turbo libwebp freetype openssl 'libavif[dav1d]'
if ($LASTEXITCODE -ne 0) { throw "vcpkg install 失败（exit $LASTEXITCODE）" }
vcpkg install ffmpeg --triplet $DynamicTriplet
if ($LASTEXITCODE -ne 0) { throw "vcpkg install ffmpeg 失败（exit $LASTEXITCODE）" }

# ctest 在构建目录里跑（不是打包后的目录），FFmpeg DLL 必须在 PATH 上。
"$env:VCPKG_ROOT\installed\$DynamicTriplet\bin" | Out-File -FilePath $env:GITHUB_PATH -Append
