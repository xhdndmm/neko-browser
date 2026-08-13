# =============================================================================
# neko_browser — the browser application entry point and CLI.
#
# Phase 0 scope: argument parsing, version/help output, logging bootstrap.
# Navigation/rendering arrive in later phases and are explicitly reported as
# NOT IMPLEMENTED by the CLI.
#
# CLI options live in a small static library (neko::browser_options) so the
# test suite can exercise parsing without running the executable.
# =============================================================================

add_library(neko_browser_options STATIC src/browser_options.cpp)
add_library(neko::browser_options ALIAS neko_browser_options)

target_compile_features(neko_browser_options PUBLIC cxx_std_20)
target_include_directories(neko_browser_options PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(neko_browser_options PUBLIC neko::base)
apply_compiler_warnings(neko_browser_options)

add_executable(neko_browser main.cpp)
target_link_libraries(neko_browser PRIVATE
  neko::browser_options
  neko::renderer
  neko::network
  neko::url
)
apply_compiler_warnings(neko_browser)
