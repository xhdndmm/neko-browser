# release.yml：Windows Qt6 安装。
#
# Qt 官方 MSVC 预编译包（aqtinstall 从 download.qt.io 下载，不依赖第三方
# action；vcpkg 从源码编译 Qt 代价过高）。arm64 目标用 Qt 的交叉编译包
# （只含目标库与目标 cmake 配置），宿主工具来自同版本 x64 包，配置时用
# QT_HOST_PATH 指定。
param(
  [Parameter(Mandatory)][string]$QtArch,
  [Parameter(Mandatory)][string]$QtDir,
  [Parameter(Mandatory)][string]$QtHostArch,
  [Parameter(Mandatory)][string]$QtHostDir
)
$ErrorActionPreference = 'Stop'

python -m pip install --disable-pip-version-check --quiet aqtinstall
if ($LASTEXITCODE -ne 0) { throw "aqtinstall 安装失败（exit $LASTEXITCODE）" }

$qtRoot = "$env:GITHUB_WORKSPACE\.qt"
python -m aqt install-qt windows desktop 6.8.3 $QtArch --outputdir $qtRoot --archives qtbase
if ($LASTEXITCODE -ne 0) { throw "Qt 目标包安装失败（exit $LASTEXITCODE）" }
if ($QtArch -ne $QtHostArch) {
  python -m aqt install-qt windows desktop 6.8.3 $QtHostArch --outputdir $qtRoot --archives qtbase
  if ($LASTEXITCODE -ne 0) { throw "Qt 宿主工具包安装失败（exit $LASTEXITCODE）" }
}

$qtPrefix = "$qtRoot\6.8.3\$QtDir"
$qtHostPrefix = "$qtRoot\6.8.3\$QtHostDir"
if (-not (Test-Path "$qtPrefix\lib\cmake\Qt6\Qt6Config.cmake")) {
  throw "Qt 目标安装不完整：$qtPrefix"
}
if (-not (Test-Path "$qtHostPrefix\lib\cmake\Qt6CoreTools\Qt6CoreToolsConfig.cmake")) {
  throw "Qt 宿主工具安装不完整：$qtHostPrefix"
}
# 记录实际布局（原生包与交叉编译包的 lib/ 内容不同）：链接失败时
# 这段日志能直接回答“Qt 的导入库到底在不在”。
Get-ChildItem "$qtPrefix\lib" -Filter 'Qt6*.lib' -ErrorAction SilentlyContinue |
  Select-Object -First 40 | ForEach-Object { Write-Host "qt-lib: $($_.FullName)" }
# 交叉编译时把目标前缀排在宿主前缀之前：目标模块（Widgets/Gui/Core）
# 取 arm64，Qt*Tools（moc/rcc/uic）只存在于 x64 前缀。
$prefixPath = $qtPrefix
if ($qtHostPrefix -ne $qtPrefix) { $prefixPath = "$qtPrefix;$qtHostPrefix" }
"QT_PREFIX=$qtPrefix" | Out-File -FilePath $env:GITHUB_ENV -Append
"QT_HOST_PREFIX=$qtHostPrefix" | Out-File -FilePath $env:GITHUB_ENV -Append
"CMAKE_PREFIX_PATH=$prefixPath" | Out-File -FilePath $env:GITHUB_ENV -Append
"$qtHostPrefix\bin" | Out-File -FilePath $env:GITHUB_PATH -Append
