# =============================================================================
# neko_dom — DOM tree types (Phase 3).
#
# Parent nodes own their children (std::unique_ptr); children hold a
# non-owning pointer back to the parent, avoiding reference cycles.
# =============================================================================

add_library(neko_dom
  src/node.cpp
  src/element.cpp
  src/query.cpp
)
add_library(neko::dom ALIAS neko_dom)

target_compile_features(neko_dom PUBLIC cxx_std_20)
target_include_directories(neko_dom PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(neko_dom PUBLIC neko::base)
apply_compiler_warnings(neko_dom)
