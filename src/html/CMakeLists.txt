# =============================================================================
# neko_html — HTML tokenizer and tree construction (Phase 3).
#
# Pipeline: HTML source -> Tokenizer -> Token stream -> Parser -> DOM.
# Malformed HTML is ordinary input, never an exception.
# =============================================================================

add_library(neko_html
  src/token.cpp
  src/tokenizer.cpp
  src/parser.cpp
)
add_library(neko::html ALIAS neko_html)

target_compile_features(neko_html PUBLIC cxx_std_20)
target_include_directories(neko_html PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(neko_html PUBLIC neko::dom neko::base)
apply_compiler_warnings(neko_html)
