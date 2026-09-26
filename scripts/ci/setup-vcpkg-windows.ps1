# CI（ci.yml）Windows vcpkg 准备。
#
# vcpkg 没有官方 setup action：手动 clone + bootstrap，不依赖第三方 action。
# release.yml 用镜像自带 VCPKG_INSTALLATION_ROOT（或 clone 回退），两处刻意
# 分开：CI 固定用 clone 的 vcpkg，避免随 runner 镜像版本漂移。
$ErrorActionPreference = 'Stop'

git clone https://github.com/microsoft/vcpkg.git "$env:RUNNER_TEMP\vcpkg"
if ($LASTEXITCODE -ne 0) { throw "vcpkg clone 失败（exit $LASTEXITCODE）" }
& "$env:RUNNER_TEMP\vcpkg\bootstrap-vcpkg.bat" -disableMetrics
if ($LASTEXITCODE -ne 0) { throw "vcpkg bootstrap 失败（exit $LASTEXITCODE）" }

"VCPKG_ROOT=$env:RUNNER_TEMP\vcpkg" | Out-File -FilePath $env:GITHUB_ENV -Append
"$env:RUNNER_TEMP\vcpkg" | Out-File -FilePath $env:GITHUB_PATH -Append
