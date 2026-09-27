# release.yml（test-windows-arm64 任务，runs-on: windows-11-arm）：
# 在原生 ARM64 runner 上运行 windows-arm64 交叉编译产物的测试套件。
#
# 测试载荷由 stage-tests-windows-arm64.ps1 在 x64 构建任务里打包（artifact
# tests-windows-arm64）。ctest / GoogleTest 的生成文件里写死了构建机绝对
# 路径，本脚本负责让它们在本 runner 上成立：
#   1. 校验本 runner 的工作区与构建机一致（GitHub 的 Windows runner 固定为
#      D:\a\<repo>\<repo>），把载荷按相同相对布局还原到 build/release；
#   2. 把载荷携带的 CMake GoogleTest 模块放回生成文件里写死的绝对路径；
#      若写死的 cmake.exe 路径不存在（镜像的 CMake 安装方式/版本可能不同），
#      用本机 cmake 顶上；
#   3. 把运行库（Qt ARM64 / FFmpeg / MSVC CRT）加进 PATH、设置 Qt 插件路径；
#   4. 运行 ctest（Release 配置、失败时输出、无测试即失败）。
#
# 环境：GITHUB_WORKSPACE（与构建机比对用）。
param(
  [string]$ArtifactDir = 'build',
  [string]$StageDir = 'build/release'
)
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrEmpty($env:GITHUB_WORKSPACE)) { throw "GITHUB_WORKSPACE 未设置" }

# --- 1. 定位并还原测试载荷 ---------------------------------------------------
$kitInfo = Get-ChildItem $ArtifactDir -Recurse -Filter 'kit-info.txt' -File -ErrorAction SilentlyContinue |
  Where-Object { Test-Path "$($_.Directory)/tests/CTestTestfile.cmake" } | Select-Object -First 1
if (-not $kitInfo) { throw "在 $ArtifactDir 下找不到测试载荷（kit-info.txt + tests/CTestTestfile.cmake）" }
$kit = $kitInfo.Directory.FullName

$info = @{}
Get-Content $kitInfo.FullName | ForEach-Object {
  if ($_ -match '^(?<key>[^=]+)=(?<value>.*)$') { $info[$Matches['key']] = $Matches['value'] }
}
if ($info['build_workspace'] -ne $env:GITHUB_WORKSPACE) {
  throw ("载荷在构建机的工作区是 '$($info['build_workspace'])'，本 runner 是 " +
    "'$env:GITHUB_WORKSPACE'：ctest 生成文件里的绝对路径无法成立，两个 runner 的" +
    "工作区路径必须一致（见脚本头注释）")
}
foreach ($part in 'bin', 'tests', 'runtime') {
  if (-not (Test-Path "$kit/$part")) { throw "载荷不完整：缺少 $kit/$part" }
}

Remove-Item -Recurse -Force $StageDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $StageDir | Out-Null
foreach ($part in 'bin', 'tests', 'runtime') { Move-Item "$kit/$part" "$StageDir/$part" }

# --- 2. CMake 模块与 cmake.exe 补位 ------------------------------------------
# 生成文件里 include/调用的是构建机 CMAKE_ROOT 下的模块（模块目录带版本号）
# 与构建机的 cmake.exe 路径；镜像与本机安装不同，按需补位，保证发现脚本和
# build_system 用例可执行。只补缺失的路径，绝不改写生成文件。
$modules = @{}
Get-ChildItem "$kit/cmake-modules" -Filter '*.cmake' -File | ForEach-Object { $modules[$_.Name] = $_.FullName }

$neededModules = @{}   # 写死的绝对路径 -> 模块文件名
$bakedTools = @{}      # 写死的 cmake.exe 路径
foreach ($file in Get-ChildItem "$StageDir/tests" -Recurse -Filter '*.cmake' -File) {
  $content = Get-Content $file.FullName -Raw
  foreach ($match in [regex]::Matches($content, '[A-Za-z]:[\\/][^"\r\n\[\]]*?[\\/]Modules[\\/]([^"\[\]\r\n]+\.cmake)')) {
    # 4.x 的模块在 Modules/GoogleTest/ 子目录下，取文件名匹配载荷里的副本。
    $name = Split-Path $match.Groups[1].Value -Leaf
    if ($name -match '^(GoogleTest|DiscoverTests|LaunchTest).*\.cmake$') {
      $neededModules[$match.Value] = $name
    }
  }
  foreach ($match in [regex]::Matches($content, '[A-Za-z]:[\\/][^"\r\n\[\]]*?[\\/]cmake\.exe')) {
    $bakedTools[$match.Value] = $true
  }
}

$patched = 0
foreach ($baked in $neededModules.Keys) {
  $target = [IO.Path]::GetFullPath($baked)
  if (Test-Path -LiteralPath $target) { continue }
  $name = $neededModules[$baked]
  if (-not $modules.ContainsKey($name)) { throw "载荷缺少 CMake 模块 $name（生成文件需要 $baked）" }
  New-Item -ItemType Directory -Force (Split-Path $target -Parent) | Out-Null
  Copy-Item -LiteralPath $modules[$name] -Destination $target
  Write-Host "cmake module: $name -> $target"
  $patched++
}
if ($bakedTools.Count -gt 0) {
  $localCmake = (Get-Command cmake).Source
  foreach ($baked in $bakedTools.Keys) {
    $target = [IO.Path]::GetFullPath($baked)
    if (Test-Path -LiteralPath $target) { continue }
    New-Item -ItemType Directory -Force (Split-Path $target -Parent) | Out-Null
    Copy-Item -LiteralPath $localCmake -Destination $target
    Write-Host "cmake.exe: $localCmake -> $target"
    $patched++
  }
}
Write-Host "test-windows-arm64: patched $patched path(s) expected by generated ctest files"

# --- 3. 运行环境 ------------------------------------------------------------
if (-not (Get-Command ctest -ErrorAction SilentlyContinue)) { throw "找不到 ctest（runner 需要 CMake）" }
$env:PATH = "$StageDir/runtime/bin" + [IO.Path]::PathSeparator + $env:PATH
$env:QT_QPA_PLATFORM_PLUGIN_PATH = "$StageDir/runtime/plugins/platforms"

# --- 4. ctest ----------------------------------------------------------------
# 与其它平台相同的套件与语义；--output-on-failure / --no-tests=error 对应
# CMakePresets 里 release testPreset 的 output/execution 设置。
& ctest --test-dir "$StageDir/tests" -C Release --output-on-failure --no-tests=error
if ($LASTEXITCODE -ne 0) { throw "ctest 失败（exit $LASTEXITCODE）" }
Write-Host "test-windows-arm64: OK"
