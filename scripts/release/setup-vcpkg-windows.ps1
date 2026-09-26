# release.yml：Windows vcpkg 准备。
#
# 优先使用镜像自带的 vcpkg（VCPKG_INSTALLATION_ROOT）；镜像未提供时回退到
# clone + bootstrap。与 scripts/ci/setup-vcpkg-windows.ps1 刻意分开：发布构建
# 用镜像版本（速度优先，版本由 runner 镜像决定），CI 固定用 clone。
$ErrorActionPreference = 'Stop'

$root = $env:VCPKG_INSTALLATION_ROOT
if (-not $root -or -not (Test-Path "$root/scripts/buildsystems/vcpkg.cmake")) {
  $root = "$env:RUNNER_TEMP/vcpkg"
  if (-not (Test-Path "$root/bootstrap-vcpkg.bat")) {
    git clone --depth 1 https://github.com/microsoft/vcpkg.git $root
    if ($LASTEXITCODE -ne 0) { throw "vcpkg clone 失败（exit $LASTEXITCODE）" }
  }
  & "$root/bootstrap-vcpkg.bat" -disableMetrics
  if ($LASTEXITCODE -ne 0) { throw "vcpkg bootstrap 失败（exit $LASTEXITCODE）" }
}

"VCPKG_ROOT=$root" | Out-File -FilePath $env:GITHUB_ENV -Append
"$root" | Out-File -FilePath $env:GITHUB_PATH -Append
