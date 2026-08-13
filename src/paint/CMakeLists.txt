# =============================================================================
# neko_paint — display list, software rasterization and page painting (Phase 6).
#
# Layout tree -> paint -> display list -> rasterization -> PPM output.
# Text is rendered with the embedded 8x8 public-domain bitmap font
# (ASCII only; FreeType/HarfBuzz integration is future work).
# =============================================================================

add_library(neko_paint
  src/display_list.cpp
  src/rasterizer.cpp
  src/painter.cpp
)
add_library(neko::paint ALIAS neko_paint)

target_compile_features(neko_paint PUBLIC cxx_std_20)
target_include_directories(neko_paint PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(neko_paint PUBLIC neko::layout neko::css neko::base)
apply_compiler_warnings(neko_paint)
