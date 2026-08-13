# =============================================================================
# neko_network — socket abstraction and HTTP/1.1 client (Phase 2).
#
# URL -> HTTP -> TCP -> Socket.  HTTPS/TLS is NOT implemented yet; the
# transport seam is Socket::Connect so a TLS layer can be added later.
# =============================================================================

add_library(neko_network
  src/socket.cpp
  src/http.cpp
)
add_library(neko::network ALIAS neko_network)

target_compile_features(neko_network PUBLIC cxx_std_20)
target_include_directories(neko_network PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(neko_network PUBLIC neko::url neko::base)
apply_compiler_warnings(neko_network)
