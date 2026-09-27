# release.yml（test-windows-arm64 任务，runs-on: windows-11-arm）：
# 在原生 ARM64 runner 上运行 windows-arm64 交叉编译产物的测试套件。
#
# 测试载荷由 stage-tests-windows-arm64.ps1 在 x64 构建任务里打包（artifact
# tests-windows-arm64）。ctest / GoogleTest 的生成文件与测试二进制（编译进
# NEKO_TEST_PAGES_DIR / NEKO_BROWSER_BIN）里写死了构建机的绝对路径，本脚本
# 负责让它们在本 runner 上成立：
#   1. 把构建机的工作区路径在本 runner 上还原出来（Windows 镜像的工作区盘符
#      不统一：x64 构建镜像在 D:，windows-11-arm 在 C:；只允许盘符不同），
#      再按相同相对布局把载荷还原到 build/release；
#   2. 把载荷携带的 CMake GoogleTest 模块放回生成文件里写死的绝对路径；
#      若写死的 cmake.exe 路径不存在（镜像的 CMake 安装方式/版本可能不同），
#      用本机 cmake 顶上；
#   3. 把运行库（Qt ARM64 / FFmpeg / MSVC CRT）加进 PATH、设置 Qt 插件路径
#      （都必须是绝对路径：测试进程的工作目录是生成文件里的 tests/unit）；
#   4. 运行 ctest（Release 配置、失败时输出、无测试即失败）。
#
# 环境：GITHUB_WORKSPACE（相对路径参数的基准；载荷里的 build_workspace 以它为准）。
param(
  [string]$ArtifactDir = 'build',
  [string]$StageDir = 'build/release'
)
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrEmpty($env:GITHUB_WORKSPACE)) { throw "GITHUB_WORKSPACE 未设置" }

# 相对路径参数以工作区为基准，脚本因此不依赖调用者的 cwd
# （约定见 docs/development/ci-scripts.md）。
function Resolve-WorkspacePath {
  param([string]$Path)
  if ([IO.Path]::IsPathRooted($Path)) { return $Path }
  return [IO.Path]::Combine($env:GITHUB_WORKSPACE, $Path)
}

# 让构建机的工作区路径在本 runner 上成立 -------------------------------------
# 载荷里写死的绝对路径都以构建机的 GITHUB_WORKSPACE 打头（生成文件里的
# add_test 命令与 CMake 模块路径，测试二进制里的 NEKO_TEST_PAGES_DIR /
# NEKO_BROWSER_BIN 等编译期常量），必须原样可解析。Windows 镜像的工作区盘符
# 并不统一：windows-2025（交叉编译任务）是 D:\a\<repo>\<repo>，
# windows-11-arm 是 C:\a\<repo>\<repo>（ARM64 镜像的第二块盘没有挂载，见
# actions/runner-images#14088），盘符以外的布局相同。于是把构建机的盘符别名
# 到本 runner 的盘符根——两边指向同一棵 checkout，写死的路径原样成立，既不改写
# 生成文件，也不给二进制打补丁。
function Initialize-BuildWorkspacePath {
  param([string]$Baked, [string]$Runner)

  if ($Baked -eq $Runner) { return }   # -eq 对字符串不区分大小写

  # 只允许盘符不同：盘符（及其后的分隔符）以外的布局必须逐字一致。
  $ok = $Baked.Length -ge 3 -and $Runner.Length -ge 3 -and
        $Baked.Substring(0, 2) -match '^[A-Za-z]:$' -and
        $Runner.Substring(0, 2) -match '^[A-Za-z]:$' -and
        $Baked.Substring(2) -eq $Runner.Substring(2)
  if (-not $ok) {
    throw ("载荷在构建机的工作区是 '$Baked'，本 runner 是 '$Runner'：两者只允许" +
      "盘符不同（Windows 镜像的工作区盘符不统一），盘符之后的布局必须一致，" +
      "否则写死的绝对路径无法还原（见脚本头注释）")
  }

  $bakedRoot = $Baked.Substring(0, 3)      # 'D:\'
  $runnerRoot = $Runner.Substring(0, 3)    # 'C:\'
  if (-not (Test-Path -LiteralPath $bakedRoot)) {
    # 盘符空闲：把构建机的盘符指到 runner 的盘符根。subst 的映射对本步骤
    # 启动的进程（ctest、发现脚本、测试可执行文件）都可见；PowerShell 的
    # PSDrive 只在自己进程内有效，不能用来还原测试二进制里的路径。
    $bakedDrive = $Baked.Substring(0, 2)
    $subst = [IO.Path]::Combine($env:SystemRoot, 'System32/subst.exe')
    & $subst $bakedDrive $runnerRoot
    if ($LASTEXITCODE -ne 0) { throw "subst $bakedDrive $runnerRoot 失败（exit $LASTEXITCODE）" }
    Write-Host "build workspace: $bakedDrive -> $runnerRoot (subst)"
  } elseif (-not (Test-Path -LiteralPath $Baked)) {
    # 盘符已被真实磁盘占用（例如镜像以后把第二块盘挂上）：改用目录联接。
    New-Item -ItemType Directory -Force (Split-Path $Baked -Parent) | Out-Null
    New-Item -ItemType Junction -Path $Baked -Target ([IO.Path]::GetFullPath($Runner)) | Out-Null
    Write-Host "build workspace: $Baked -> $Runner (junction)"
  }

  # 生成文件与测试二进制还会从构建机路径读源码树（夹具、生成脚本、presets），
  # 它们由本任务的 checkout 提供；别名没建对时在这里失败，而不是等到 ctest。
  foreach ($rel in 'tests/pages', 'tests/cmake', 'cmake', 'CMakePresets.json') {
    if (-not (Test-Path -LiteralPath ([IO.Path]::Combine($Baked, $rel)))) {
      throw "构建机的工作区路径 '$Baked' 未指向本 runner 的 checkout（缺 $rel）"
    }
  }
}

# --- 1. 定位并还原测试载荷 ---------------------------------------------------
$ArtifactDir = Resolve-WorkspacePath $ArtifactDir
$StageDir = Resolve-WorkspacePath $StageDir
$kitInfo = Get-ChildItem $ArtifactDir -Recurse -Filter 'kit-info.txt' -File -ErrorAction SilentlyContinue |
  Where-Object { Test-Path "$($_.Directory)/tests/CTestTestfile.cmake" } | Select-Object -First 1
if (-not $kitInfo) { throw "在 $ArtifactDir 下找不到测试载荷（kit-info.txt + tests/CTestTestfile.cmake）" }
$kit = $kitInfo.Directory.FullName

$info = @{}
Get-Content $kitInfo.FullName | ForEach-Object {
  if ($_ -match '^(?<key>[^=]+)=(?<value>.*)$') { $info[$Matches['key']] = $Matches['value'] }
}
if (-not $info.ContainsKey('build_workspace')) {
  throw "载荷的 kit-info.txt 缺少 build_workspace（无法还原写死的绝对路径）"
}
Initialize-BuildWorkspacePath -Baked $info['build_workspace'] -Runner $env:GITHUB_WORKSPACE

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
# 绝对路径：测试进程的 cwd 是生成文件里的 tests/unit（ctest 以 CTestTestfile
# 所在目录为默认工作目录），相对的 PATH / 插件路径会解析到别的地方。
$env:PATH = [IO.Path]::Combine($StageDir, 'runtime/bin') + [IO.Path]::PathSeparator + $env:PATH
$env:QT_QPA_PLATFORM_PLUGIN_PATH = [IO.Path]::Combine($StageDir, 'runtime/plugins/platforms')

# --- 4. ctest ----------------------------------------------------------------
# 与其它平台相同的套件与语义；--output-on-failure / --no-tests=error 对应
# CMakePresets 里 release testPreset 的 output/execution 设置。
& ctest --test-dir "$StageDir/tests" -C Release --output-on-failure --no-tests=error
if ($LASTEXITCODE -ne 0) { throw "ctest 失败（exit $LASTEXITCODE）" }
Write-Host "test-windows-arm64: OK"
