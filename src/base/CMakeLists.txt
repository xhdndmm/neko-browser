# =============================================================================
# neko_base — foundational infrastructure used by every other module.
#
# Phase 0 scope: logging, error/result model, string utilities, version
# constants and assertion helpers.  No external dependencies (except Threads).
# =============================================================================

add_library(neko_base
  src/version.cpp
  src/logging.cpp
  src/status.cpp
  src/string_util.cpp
)
add_library(neko::base ALIAS neko_base)

target_compile_features(neko_base PUBLIC cxx_std_20)

# Public headers: module include dir + generated version header.
target_include_directories(neko_base PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
)

target_link_libraries(neko_base PUBLIC Threads::Threads)

# Generate version.h from the CMake project version.
configure_file(
  include/neko/base/version.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/include/neko/base/version.h
  @ONLY
)

apply_compiler_warnings(neko_base)
