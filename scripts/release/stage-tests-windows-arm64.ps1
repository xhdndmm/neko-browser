# release.yml（windows-arm64 任务）：打包「Windows ARM64 测试载荷」。
#
# Windows ARM64 由 x64 runner 用 MSVC 交叉编译完成，产物无法在 x64 上执行，
# 因此把运行 ctest 所需的部分连同运行库（全部为 ARM64 二进制）打成一个
# 自包含载荷上传 artifact，由 test-windows-arm64 任务在原生 ARM64 runner
# （windows-11-vs2026-arm）上还原并运行；还原逻辑见 test-windows-arm64.ps1。
#
# 为什么载荷里必须保留构建树的相对布局：ctest 的 GoogleTest PRE_TEST 发现
# 脚本与 add_test 命令把构建机绝对路径写进了生成文件（测试可执行文件、工作
# 目录、源目录 tests/pages、CMake 的 GoogleTest 模块），测试二进制里还编译进
# 了 NEKO_TEST_PAGES_DIR / NEKO_BROWSER_BIN。构建机（windows-2025）的工作区是
# D:\a\<repo>\<repo>，ARM64 runner（windows-11-vs2026-arm）是 C:\a\<repo>\<repo>
# ——除盘符外布局相同，还原脚本会把构建机的盘符别名到本 runner 的盘符根
# （见 test-windows-arm64.ps1），这些路径因此原样成立；CMake 模块另行携带一份，
# 由还原脚本放回它被写死的绝对路径。
#
# 用法：stage-tests-windows-arm64.ps1 -VcpkgDynamicTriplet <triplet>
#
# 环境（由 workflow 前面的步骤写入）：GITHUB_WORKSPACE、QT_PREFIX（ARM64
# 目标前缀，install-qt-windows.ps1 写入）、VCPKG_ROOT（镜像自带）。
param(
  [Parameter(Mandatory)][string]$VcpkgDynamicTriplet,
  [string]$OutDir = 'build/testkit-windows-arm64'
)
$ErrorActionPreference = 'Stop'

foreach ($name in 'GITHUB_WORKSPACE', 'QT_PREFIX', 'VCPKG_ROOT') {
  if ([string]::IsNullOrEmpty([Environment]::GetEnvironmentVariable($name))) {
    throw "$name 未设置（由 workflow 前面的步骤写入）"
  }
}

$binDir = 'build/release/bin/Release'
$testsDir = 'build/release/tests'
if (-not (Test-Path "$binDir/neko_browser.exe")) { throw "找不到 $binDir/neko_browser.exe（请先构建）" }
if (-not (Test-Path "$testsDir/CTestTestfile.cmake")) { throw "找不到 $testsDir/CTestTestfile.cmake（NEKO_BUILD_TESTS=ON 的 release 构建会生成）" }

Remove-Item -Recurse -Force $OutDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$OutDir/bin/Release", "$OutDir/tests", `
  "$OutDir/runtime/bin", "$OutDir/runtime/plugins/platforms", "$OutDir/cmake-modules" | Out-Null

# --- 构建产物 ---------------------------------------------------------------
# bin/Release 里有全部测试可执行文件与 CLI：浏览器/UI 测试会通过
# NEKO_BROWSER_BIN 启动 CLI，渲染器子进程复用同一个二进制。
Copy-Item "$binDir/*" "$OutDir/bin/Release" -Recurse
Remove-Item "$OutDir/bin/Release/*.pdb" -ErrorAction SilentlyContinue # 测试不需要调试符号
# tests/ 只需要 ctest 元数据（CTestTestfile.cmake + GoogleTest 发现脚本；
# 测试二进制在 bin/ 下）。
Copy-Item "$testsDir/*" "$OutDir/tests" -Recurse
Remove-Item -Recurse -Force "$OutDir/tests/Testing" -ErrorAction SilentlyContinue

$testExeCount = (Get-ChildItem "$OutDir/bin/Release" -Filter 'neko_*_tests.exe').Count
if ($testExeCount -eq 0) { throw "载荷里没有任何 neko_*_tests.exe（$binDir）" }

# --- CMake 的 GoogleTest 发现模块 --------------------------------------------
# 生成文件 include 的是构建机 CMAKE_ROOT 下的模块（3.31 风格：
# GoogleTestAddTests.cmake；4.x 风格：DiscoverTests/LaunchTest.cmake）。
# 镜像自带的 CMake 版本不同、模块目录带版本号，所以整份携带、由还原脚本按
# 文件名放回被写死的位置。
$systemInfo = & cmake --system-information
if ($LASTEXITCODE -ne 0) { throw "cmake --system-information 失败（exit $LASTEXITCODE）" }
$cmakeRoot = $null
foreach ($line in $systemInfo) {
  if ($line -match '^CMAKE_ROOT "(.*)"$') { $cmakeRoot = $Matches[1]; break }
}
if (-not $cmakeRoot) { throw "无法从 cmake --system-information 解析 CMAKE_ROOT" }
foreach ($module in 'GoogleTest.cmake', 'GoogleTestAddTests.cmake') {
  $source = Join-Path $cmakeRoot "Modules/$module"
  if (Test-Path $source) { Copy-Item $source "$OutDir/cmake-modules/" }
}
# 4.x 把发现脚本放在 Modules/GoogleTest/ 子目录（DiscoverTests/LaunchTest）。
Get-ChildItem (Join-Path $cmakeRoot 'Modules/GoogleTest') -Filter '*.cmake' -File -ErrorAction SilentlyContinue |
  Copy-Item -Destination "$OutDir/cmake-modules/"
if (-not (Get-ChildItem "$OutDir/cmake-modules" -Filter '*.cmake' -File)) {
  throw "未找到 GoogleTest 发现模块（$cmakeRoot/Modules）"
}

# --- 运行库（全部为 ARM64）---------------------------------------------------
# Qt（官方包是动态库；官方 ARM64 交叉包不含 windeployqt，按清单部署）。
$qtBinCount = 0
foreach ($pattern in 'Qt6*.dll', 'icu*.dll', 'd3dcompiler_47.dll', 'opengl32sw.dll') {
  Get-ChildItem "$env:QT_PREFIX/bin" -Filter $pattern -File -ErrorAction SilentlyContinue |
    ForEach-Object { Copy-Item $_.FullName "$OutDir/runtime/bin/"; $qtBinCount++ }
}
Get-ChildItem "$env:QT_PREFIX/plugins/platforms" -Filter '*.dll' -File -ErrorAction SilentlyContinue |
  Copy-Item -Destination "$OutDir/runtime/plugins/platforms/"
if ($qtBinCount -eq 0) { throw "Qt 运行库为空：$env:QT_PREFIX/bin（Qt 目标包是否安装？）" }
if (-not (Test-Path "$OutDir/runtime/plugins/platforms/qoffscreen.dll")) {
  throw "缺少 qoffscreen.dll（$env:QT_PREFIX/plugins/platforms；UI 测试用 offscreen 平台）"
}
# FFmpeg（LGPL 保持动态）：整个 DLL 家族，理由见 package-windows.ps1。
$ffmpegCount = 0
foreach ($pattern in 'av*.dll', 'sw*.dll', 'postproc*.dll') {
  Get-ChildItem "$env:VCPKG_ROOT/installed/$VcpkgDynamicTriplet/bin" -Filter $pattern -File -ErrorAction SilentlyContinue |
    ForEach-Object { Copy-Item $_.FullName "$OutDir/runtime/bin/"; $ffmpegCount++ }
}
if ($ffmpegCount -eq 0) { throw "FFmpeg 运行库为空：$env:VCPKG_ROOT/installed/$VcpkgDynamicTriplet/bin" }
# MSVC CRT（与零安装打包同一来源）。
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($vs)) { throw "vswhere 失败：找不到 Visual Studio" }
$crt = Get-ChildItem "$vs/VC/Redist/MSVC/*/*/Microsoft.VC*.CRT" -Directory |
  Where-Object { $_.FullName -match '[\\/]arm64[\\/]' } |
  Sort-Object FullName | Select-Object -Last 1
if (-not $crt) { throw "找不到 arm64 的 MSVC CRT 目录（$vs）" }
Copy-Item "$($crt.FullName)/*.dll" "$OutDir/runtime/bin/"

# --- 元信息 -----------------------------------------------------------------
# 还原脚本用它把构建机的工作区路径在本 runner 上还原（写死的绝对路径依赖这个）。
@(
  "build_workspace=$env:GITHUB_WORKSPACE"
  "cmake_root=$cmakeRoot"
  "qt_prefix=$env:QT_PREFIX"
  "vcpkg_installed=$env:VCPKG_ROOT/installed/$VcpkgDynamicTriplet"
) | Out-File "$OutDir/kit-info.txt" -Encoding ascii

$size = (Get-ChildItem $OutDir -Recurse -File | Measure-Object Length -Sum).Sum / 1MB
Write-Host ("test payload: {0} test binaries, {1:N1} MB -> {2}" -f $testExeCount, $size, $OutDir)
