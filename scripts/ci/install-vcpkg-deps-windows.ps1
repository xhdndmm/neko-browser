# CI（ci.yml）Windows vcpkg 依赖。
#
# libavif 默认不启用任何 AV1 解码器（vcpkg 端口没有 default-features），
# 必须显式选择 dav1d，否则 AVIF 解码在运行期返回 "No codec available"。
$ErrorActionPreference = 'Stop'

vcpkg install zlib libjpeg-turbo libwebp freetype openssl ffmpeg 'libavif[dav1d]'
if ($LASTEXITCODE -ne 0) { throw "vcpkg install 失败（exit $LASTEXITCODE）" }
