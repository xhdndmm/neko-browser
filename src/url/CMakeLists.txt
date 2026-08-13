# =============================================================================
# neko_url — URL parsing and serialization (Phase 1).
#
# Modeled on WHATWG URL / RFC 3986.  Supports absolute parsing, relative
# resolution against a base URL, dot-segment removal, percent encoding and
# origin computation.  See docs/design/url.md.
# =============================================================================

add_library(neko_url src/url.cpp)
add_library(neko::url ALIAS neko_url)

target_compile_features(neko_url PUBLIC cxx_std_20)
target_include_directories(neko_url PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(neko_url PUBLIC neko::base)
apply_compiler_warnings(neko_url)
