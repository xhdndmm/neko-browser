# =============================================================================
# neko_layout — layout tree construction (Phase 5).
#
# DOM + computed styles -> layout tree -> layout.
# Block layout, inline layout (line boxes), box model.
# =============================================================================

add_library(neko_layout
  src/layout.cpp
)
add_library(neko::layout ALIAS neko_layout)

target_compile_features(neko_layout PUBLIC cxx_std_20)
target_include_directories(neko_layout PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(neko_layout PUBLIC neko::style neko::dom neko::css neko::base)
apply_compiler_warnings(neko_layout)
