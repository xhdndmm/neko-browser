# release.yml：Windows 零安装校验。
#
# 被静态链接的依赖不得再以 DLL 形式出现；运行库必须随包；包内每个 PE 文件
# （exe + dll）的导入表只能引用包内文件、系统 DLL 或 Windows API set。
#
# 环境（由前面的步骤写入）：PACKAGE
$ErrorActionPreference = 'Stop'

$bin = "$env:PACKAGE/bin"
$failed = $false

# 1) 静态链接的依赖不应再作为 DLL 存在。
$forbidden = @('zlib1.dll', 'libcrypto-*.dll', 'libssl-*.dll', 'avif.dll',
               'dav1d.dll', 'libwebp*.dll', 'webp*.dll', 'jpeg*.dll',
               'turbojpeg.dll', 'freetype*.dll', 'yuv.dll',
               'brotli*.dll', 'bz2.dll', 'libpng*.dll', 'png*.dll')
foreach ($pattern in $forbidden) {
  foreach ($hit in @(Get-ChildItem "$bin/$pattern" -ErrorAction SilentlyContinue)) {
    Write-Host "::error::静态链接后仍存在 $($hit.Name)"
    $failed = $true
  }
}

# 2) 必须随包的运行库。
$required = @('vcruntime140.dll', 'msvcp140.dll')
if (Test-Path "$bin/neko_browser_gui.exe") {
  $required += @('Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'platforms/qwindows.dll')
}
foreach ($name in $required) {
  if (-not (Test-Path "$bin/$name")) {
    Write-Host "::error::缺少运行库 $name"
    $failed = $true
  }
}
if (-not (Get-ChildItem "$bin/avcodec-*.dll" -ErrorAction SilentlyContinue)) {
  Write-Host "::error::缺少 FFmpeg 运行库（avcodec-*.dll）"
  $failed = $true
}

# 3) 导入表检查：包内每个 PE 文件（.exe 与 .dll）的每个导入都必须来自
#    包内、System32，或 Windows API set。把包内所有 DLL 也检查一遍
#    等价于验证完整的传递闭包——只看 exe 会漏掉“主程序能启动、某个
#    DLL 间接缺依赖”的情况（swscale-*.dll 就是这类漏检）。
#    api-ms-win-* / ext-ms-win-* 是 API set 契约名，由加载器按
#    apisetschema 解析（Win10+ 系统自带），不是磁盘文件，因此不能用
#    Test-Path System32 判断。
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
  -latest -property installationPath
$dumpbin = Get-ChildItem "$vs\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe" |
  Sort-Object FullName | Select-Object -Last 1
if (-not $dumpbin) { throw '找不到 dumpbin.exe' }

$packageFiles = @{}
Get-ChildItem $bin -Recurse -File |
  ForEach-Object { $packageFiles[$_.Name.ToLower()] = $true }

$checked = 0
foreach ($binary in @(Get-ChildItem $bin -Recurse -File |
                        Where-Object { $_.Extension -in '.exe', '.dll' })) {
  $checked++
  $imports = & $dumpbin.FullName /nologo /dependents $binary.FullName |
    ForEach-Object {
      if ($_ -match '^\s+(\S+\.dll)\s*$') { $Matches[1].ToLower() }
    }
  foreach ($dll in $imports) {
    if ($dll -match '^(api|ext)-ms-win-') { continue }
    if ($packageFiles.ContainsKey($dll)) { continue }
    if (Test-Path "$env:SystemRoot\System32\$dll") { continue }
    Write-Host "::error::$($binary.Name) 导入 $dll，但包内与系统目录都不存在"
    $failed = $true
  }
}
Write-Host "checked $checked PE files"

if ($failed) { throw '零安装校验失败' }
Write-Host 'package dependency check OK'
