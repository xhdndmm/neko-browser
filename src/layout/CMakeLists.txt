# =============================================================================
# neko_layout — layout tree construction (Phase 5).
#
# DOM + computed styles -> layout tree -> layout.
# Block layout, inline layout (line boxes), table layout, box model.
# Text width measurement uses neko::graphics (FreeType) when a face is
# provided; otherwise a monospace fallback (font_size per character).
# =============================================================================

add_library(neko_layout
  src/layout.cpp
)
add_library(neko::layout ALIAS neko_layout)

target_compile_features(neko_layout PUBLIC cxx_std_20)
target_include_directories(neko_layout PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(neko_layout PUBLIC neko::style neko::dom neko::css neko::graphics neko::base)
apply_compiler_warnings(neko_layout)
