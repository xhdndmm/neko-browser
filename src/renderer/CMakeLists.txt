# =============================================================================
# neko_renderer — the page pipeline (HTML -> DOM -> style -> layout -> paint).
#
# Network fetching is orchestrated by the browser application; this module is
# intentionally network-free so the whole pipeline is testable headless.
# =============================================================================

add_library(neko_renderer
  src/page.cpp
)
add_library(neko::renderer ALIAS neko_renderer)

target_compile_features(neko_renderer PUBLIC cxx_std_20)
target_include_directories(neko_renderer PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(neko_renderer PUBLIC neko::paint neko::layout neko::style neko::dom neko::html neko::base)
apply_compiler_warnings(neko_renderer)
