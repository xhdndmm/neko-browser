#!/usr/bin/env bash
# CI（ci.yml）配置步骤。
#
# 用法：scripts/ci/configure.sh <preset>
#
# 环境：
#   RUNNER_OS  GitHub Actions 提供；仅在 Windows 上额外注入 vcpkg toolchain
#   VCPKG_ROOT Windows 上指向 vcpkg 仓库根目录
#   CC / CXX   可选，矩阵里的编译器覆盖
set -euo pipefail

preset="${1:?usage: scripts/ci/configure.sh <preset>}"
runner_os="${RUNNER_OS:-unknown}"

extra_args=()
if [ "$runner_os" = "Windows" ]; then
  extra_args+=("-DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
fi

cmake --preset "$preset" -DNEKO_WARNINGS_AS_ERRORS=ON "${extra_args[@]}"
