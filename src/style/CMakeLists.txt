# =============================================================================
# neko_style — computed style resolution (Phase 4).
#
# DOM + CSS rules -> selector matching -> cascade -> computed style.
# Depends on neko::css for parsing and matching.
# =============================================================================

add_library(neko_style
  src/style_engine.cpp
  src/computed_style.cpp
)
add_library(neko::style ALIAS neko_style)

target_compile_features(neko_style PUBLIC cxx_std_20)
target_include_directories(neko_style PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(neko_style PUBLIC neko::css neko::dom neko::base)
apply_compiler_warnings(neko_style)
