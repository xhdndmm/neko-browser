# release.yml：校验 Windows 产物的 PE Machine 与发布矩阵一致。
#
# 与 Linux/macOS 版（verify-artifact-arch-unix.sh）同样的理由：交叉编译配置
# 下悄悄产出 x64 二进制同样能在 x64 runner 上运行，必须显式断言。
#
# 用法：verify-artifact-arch-windows.ps1 -Arch <x86_64|arm64>
param(
  [Parameter(Mandatory)][string]$Arch
)
$ErrorActionPreference = 'Stop'

$expected = @{ 'x86_64' = 0x8664; 'arm64' = 0xAA64 }[$Arch]
if (-not $expected) { throw "未知架构：$Arch" }

# VS 是多配置生成器：产物落在 bin/<配置>/ 下（构建 preset 已固定 Release）。
$binaries = Get-ChildItem 'build/release/bin/Release/*.exe'
if (-not $binaries) { throw 'build/release/bin/Release 下没有可执行文件' }

foreach ($exe in $binaries) {
  $fs = [System.IO.File]::OpenRead($exe.FullName)
  try {
    $reader = [System.IO.BinaryReader]::new($fs)
    $fs.Position = 0x3C
    $peOffset = $reader.ReadInt32()
    $fs.Position = $peOffset + 4
    $machine = $reader.ReadUInt16()
  } finally {
    $fs.Dispose()
  }

  if ($machine -ne $expected) {
    throw "$($exe.Name) 的 PE Machine 为 0x$('{0:X4}' -f $machine)，期望 $Arch"
  }
  Write-Host "$($exe.Name): $Arch"
}
