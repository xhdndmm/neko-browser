# =============================================================================
# neko_compositor — the compositor abstraction (ADR 0015, ADR 0017).
#
# Presentation seam between rasterization and the UI: the compositor owns the
# output surface and an ordered list of layers (page backdrop + overlays such
# as the caret), composites them on the CPU and performs the scroll blit.
# The GPU front end (gpu_compositor / gpu_context) presents the same interface;
# its Linux EGL/OpenGL backend (egl_gpu_context) is real, the other platforms
# and window-system presentation are not implemented (ADR 0017).
#
# Dependencies stay low-level on purpose (css for colors, base for warnings):
# the compositor must not depend on paint/renderer, so GPU backends can reuse
# it without pulling in the software rasterizer.  The EGL backend loads every
# EGL/GL entry point at runtime (dlopen), which is why it needs ${CMAKE_DL_LIBS}
# but no link-time graphics dependency.
# =============================================================================

add_library(neko_compositor
  src/surface.cpp
  src/compositor.cpp
  src/gpu_context.cpp
  src/gpu_compositor.cpp
  src/egl_gpu_context.cpp
)
add_library(neko::compositor ALIAS neko_compositor)

target_compile_features(neko_compositor PUBLIC cxx_std_20)
target_include_directories(neko_compositor PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(neko_compositor PUBLIC neko::css neko::base PRIVATE ${CMAKE_DL_LIBS})
apply_compiler_warnings(neko_compositor)
