# release.yml：断言配置期实际生效的目标平台与 vcpkg 三元组。
#
# “平台没有生效”在 CMake 里是静默的，失败会推迟到链接期变成数百行
# LNK2001；这里失败一次就给出可读原因，也保证产物架构与发布矩阵一致。
param(
  [Parameter(Mandatory)][string]$GeneratorPlatform,
  [Parameter(Mandatory)][string]$VcpkgTriplet
)
$ErrorActionPreference = 'Stop'

$cachePath = 'build/release/CMakeCache.txt'
if (-not (Test-Path $cachePath)) { throw "缺少 $cachePath" }
$cache = Get-Content $cachePath

$platformMatch = @($cache | Select-String '^CMAKE_GENERATOR_PLATFORM:[^=]+=(.+)$') |
  Select-Object -First 1
if (-not $platformMatch) {
  throw 'CMakeCache.txt 中没有 CMAKE_GENERATOR_PLATFORM：-A 未生效'
}
$platform = $platformMatch.Matches[0].Groups[1].Value.Trim()
Write-Host "CMAKE_GENERATOR_PLATFORM: $platform"
if ($platform -ine $GeneratorPlatform) {
  throw "目标平台为 '$platform'，期望 '$GeneratorPlatform'：产物架构会与发布矩阵不符"
}

$tripletMatch = @($cache | Select-String '^VCPKG_TARGET_TRIPLET:[^=]+=(.+)$') |
  Select-Object -First 1
$triplet = ''
if ($tripletMatch) { $triplet = $tripletMatch.Matches[0].Groups[1].Value.Trim() }
Write-Host "VCPKG_TARGET_TRIPLET: $triplet"
if ($triplet -ne $VcpkgTriplet) {
  throw "vcpkg 三元组为 '$triplet'，期望 '$VcpkgTriplet'"
}
