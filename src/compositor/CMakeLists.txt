# =============================================================================
# neko_compositor — the software compositor abstraction (ADR 0015).
#
# Presentation seam between rasterization and the UI: the compositor owns the
# output surface and an ordered list of layers (page backdrop + overlays such
# as the caret), composites them on the CPU and performs the scroll blit.
# A GPU implementation can provide the same Compositor interface later.
#
# Dependencies stay low-level on purpose (css for colors, base for warnings):
# the compositor must not depend on paint/renderer, so GPU backends can reuse
# it without pulling in the software rasterizer.
# =============================================================================

add_library(neko_compositor
  src/surface.cpp
  src/compositor.cpp
)
add_library(neko::compositor ALIAS neko_compositor)

target_compile_features(neko_compositor PUBLIC cxx_std_20)
target_include_directories(neko_compositor PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(neko_compositor PUBLIC neko::css neko::base)
apply_compiler_warnings(neko_compositor)
