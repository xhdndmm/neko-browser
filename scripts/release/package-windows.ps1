# release.yml：Windows 零安装打包（ADR 0019）。
#
# 除 Qt（官方只提供动态库，与当前配置一致）与 FFmpeg（LGPL 要求保持动态
# 链接，ADR 0014）外全部静态链接；Qt / FFmpeg / VC 运行库全部随包分发。
#
# 用法：package-windows.ps1 -Arch <x86_64|arm64> -VcpkgDynamicTriplet <triplet>
#                        -QtArch <aqt arch> -QtHostArch <aqt host arch>
#
# 环境（由前面的步骤写入）：PACKAGE、QT_PREFIX、QT_HOST_PREFIX、VCPKG_ROOT
param(
  [Parameter(Mandatory)][string]$Arch,
  [Parameter(Mandatory)][string]$VcpkgDynamicTriplet,
  [Parameter(Mandatory)][string]$QtArch,
  [Parameter(Mandatory)][string]$QtHostArch
)
$ErrorActionPreference = 'Stop'

Remove-Item -Recurse -Force $env:PACKAGE -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$env:PACKAGE/bin" | Out-Null

# VS 是多配置生成器：产物落在 bin/Release/ 下。
Copy-Item 'build/release/bin/Release/neko_browser.exe' "$env:PACKAGE/bin/"
$gui = 'build/release/bin/Release/neko_browser_gui.exe'
if (Test-Path $gui) { Copy-Item $gui "$env:PACKAGE/bin/" }
Copy-Item 'LICENSE', 'README.md' "$env:PACKAGE/"

# Qt6 运行库随 GUI 一起打包（官方预编译包是动态库）。
if (Test-Path $gui) {
  if ($QtArch -eq $QtHostArch) {
    # 原生 x64：windeployqt 部署 Qt DLL 与平台插件。
    & "$env:QT_HOST_PREFIX\bin\windeployqt.exe" --release --no-translations `
      --no-compiler-runtime --no-system-d3d-compiler --no-opengl-sw `
      --dir "$env:PACKAGE/bin" "$env:PACKAGE/bin/neko_browser_gui.exe"
    if ($LASTEXITCODE -ne 0) { throw "windeployqt 失败（exit $LASTEXITCODE）" }
  } else {
    # ARM64 交叉编译包不含 windeployqt（部署工具只有宿主版本，无法为
    # 目标架构部署），按已知依赖手工部署：3 个运行库 + 平台/样式/图像格式插件。
    $bin = "$env:PACKAGE/bin"
    foreach ($dll in @('Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll')) {
      Copy-Item "$env:QT_PREFIX/bin/$dll" $bin
    }
    foreach ($plugin in @('platforms/qwindows.dll',
                          'styles/qmodernwindowsstyle.dll',
                          'imageformats/qgif.dll',
                          'imageformats/qico.dll',
                          'imageformats/qjpeg.dll')) {
      $source = "$env:QT_PREFIX/plugins/$plugin"
      if (Test-Path $source) {
        $target = Join-Path $bin (Split-Path $plugin -Parent)
        New-Item -ItemType Directory -Force $target | Out-Null
        Copy-Item $source $target
      }
    }
  }
}

# FFmpeg 运行库（LGPL：不静态链接，随包分发动态库）。模式必须覆盖整个
# FFmpeg DLL 家族：av*（avcodec/avformat/avutil/avdevice/avfilter）、
# sw*（swscale/swresample）、postproc*。只写 `av*.dll` 会漏掉
# swscale-*.dll——两个可执行文件都导入它，零安装校验因此失败。
foreach ($pattern in @('av*.dll', 'sw*.dll', 'postproc*.dll')) {
  Get-ChildItem "$env:VCPKG_ROOT\installed\$VcpkgDynamicTriplet\bin\$pattern" |
    Copy-Item -Destination "$env:PACKAGE/bin/"
}

# MSVC 运行库：静态三元组用的仍是动态 CRT（与官方 Qt 的 /MD 一致），
# windeployqt 不负责跨架构部署，统一从 VS 的 Redist 目录拷贝。
$crtArch = @{ 'x86_64' = 'x64'; 'arm64' = 'arm64' }[$Arch]
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
  -latest -property installationPath
$crt = Get-ChildItem "$vs\VC\Redist\MSVC\*\*\Microsoft.VC*.CRT" -Directory |
  Where-Object { $_.FullName -match "\\$crtArch\\" } |
  Sort-Object FullName | Select-Object -Last 1
if (-not $crt) { throw "找不到 $crtArch 的 MSVC CRT 目录（$vs）" }
Copy-Item "$($crt.FullName)\*.dll" "$env:PACKAGE/bin/"

$zip = "$env:PACKAGE.zip"
Compress-Archive -Path $env:PACKAGE -DestinationPath $zip -CompressionLevel Optimal

$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
"$hash  $env:PACKAGE.zip" | Out-File -FilePath "$zip.sha256" -Encoding ascii
Get-Content "$zip.sha256"
