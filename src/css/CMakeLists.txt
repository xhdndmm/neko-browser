# =============================================================================
# neko_css — CSS tokenizer, parser, selectors, values (Phase 4).
#
# Pipeline: CSS source -> Tokenizer -> Parser -> Stylesheet ->
#           Selector matching -> Cascade -> Computed style (neko::style).
# =============================================================================

add_library(neko_css
  src/color.cpp
  src/tokenizer.cpp
  src/parser.cpp
  src/selector.cpp
  src/value.cpp
)
add_library(neko::css ALIAS neko_css)

target_compile_features(neko_css PUBLIC cxx_std_20)
target_include_directories(neko_css PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(neko_css PUBLIC neko::dom neko::base)
apply_compiler_warnings(neko_css)
