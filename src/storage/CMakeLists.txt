# =============================================================================
# neko_storage — persistent profile data (Phase 10 preview).
#
# Self-implemented, line-oriented stores with atomic writes:
#   cookies.txt  / history.txt  / bookmarks.txt  / local_storage.txt
# No external dependencies.  See docs/architecture/architecture.md.
# =============================================================================

add_library(neko_storage
  src/field_codec.cpp
  src/file_util.cpp
  src/cookie_store.cpp
  src/history_store.cpp
  src/bookmark_store.cpp
  src/local_storage.cpp
)
add_library(neko::storage ALIAS neko_storage)

target_compile_features(neko_storage PUBLIC cxx_std_20)
target_include_directories(neko_storage PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(neko_storage PUBLIC neko::url neko::base)
apply_compiler_warnings(neko_storage)
