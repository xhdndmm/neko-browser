# CI（ci.yml）Windows Qt6 安装。
#
# 直接取 Qt 官方 MSVC 预编译包（aqtinstall 从 download.qt.io 下载，无需第三方
# action；vcpkg 从源码编译 Qt 对 CI 而言过慢）。qtbase 内含 Widgets / Gui /
# Core / Test 与 offscreen 平台插件（UI 测试用）。
#
# 发布构建的 Qt 安装（交叉编译 + 宿主工具）见
# scripts/release/install-qt-windows.ps1。
$ErrorActionPreference = 'Stop'

python -m pip install --disable-pip-version-check --quiet aqtinstall
if ($LASTEXITCODE -ne 0) { throw "aqtinstall 安装失败（exit $LASTEXITCODE）" }

$qtRoot = "$env:GITHUB_WORKSPACE\.qt"
python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 --outputdir $qtRoot --archives qtbase
$qtPrefix = "$qtRoot\6.8.3\msvc2022_64"
if (-not (Test-Path "$qtPrefix\lib\cmake\Qt6\Qt6Config.cmake")) {
  throw "Qt 安装不完整：$qtPrefix"
}
# 记录实际布局：链接失败时这段日志能直接回答“Qt 的导入库在不在”。
Get-ChildItem "$qtPrefix\lib" -Filter 'Qt6*.lib' -ErrorAction SilentlyContinue |
  Select-Object -First 40 | ForEach-Object { Write-Host "qt-lib: $($_.FullName)" }
# find_package(Qt6) 用 CMAKE_PREFIX_PATH；PATH 供运行测试时加载 Qt6*.dll。
"CMAKE_PREFIX_PATH=$qtPrefix" | Out-File -FilePath $env:GITHUB_ENV -Append
"$qtPrefix\bin" | Out-File -FilePath $env:GITHUB_PATH -Append
