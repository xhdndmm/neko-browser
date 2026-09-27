# release.yml：Windows 链接失败时的诊断（步骤条件 failure() && Windows）。
#
# 打印实际写进 vcxproj 的链接输入：把“依赖没进链接行”与“依赖搜错（路径/
# 架构不对）”区分开，避免只靠 unresolved 符号猜原因。直接读 CMake 生成的
# vcxproj，不依赖 msbuild 在 PATH 上——最初版本正是在调用 msbuild 时失败，
# 导致该诊断没有产出任何信息。
#
# 诊断步骤本身不应再制造失败：所有解析错误都只是打印，最后不抛异常。
$ErrorActionPreference = 'Continue'

foreach ($project in @('build/release/tests/unit/neko_ui_tests.vcxproj',
                       'build/release/src/browser/neko_browser.vcxproj')) {
  if (-not (Test-Path $project)) {
    Write-Host "no project at $project"
    continue
  }
  Write-Host "=== $project ==="
  $raw = Get-Content $project -Raw
  try {
    [xml]$doc = $raw
    $nodes = $doc.GetElementsByTagName('AdditionalDependencies')
  } catch {
    Write-Host "cannot parse $project as XML: $_"
    $nodes = @()
  }
  $all = @()
  foreach ($node in $nodes) {
    $parent = $node.ParentNode
    $condition = ''
    if ($parent -and $parent.Attributes['Condition']) {
      $condition = $parent.Attributes['Condition'].Value
    }
    if (-not $condition) { $condition = '(unconditional)' }
    $items = @($node.InnerText -split ';' | Where-Object { $_ })
    Write-Host "AdditionalDependencies $condition ($($items.Count) entries):"
    $items | ForEach-Object { Write-Host "  $_" }
    $all += $items
  }
  if ($all.Count -eq 0) {
    # 兜底：XML 解析失败时至少把原始行打出来。
    $raw -split "`n" | Select-String -Pattern 'AdditionalDependencies' |
      Select-Object -First 3 | ForEach-Object { Write-Host $_ }
  }
  # 一眼看出依赖是否整体搜错架构/三元组。
  $arm64 = @($all | Where-Object { $_ -match 'arm64' }).Count
  $x64 = @($all | Where-Object { $_ -match 'x64' }).Count
  Write-Host "total entries: $($all.Count), referencing arm64: $arm64, referencing x64: $x64"
}
