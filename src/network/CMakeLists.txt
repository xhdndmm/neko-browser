# =============================================================================
# neko_network — socket abstraction and HTTP/1.1 client (Phase 2).
#
# URL -> HTTP -> TLS -> TCP -> Socket.  HTTPS is provided by neko::network::
# TlsSocket wrapping OpenSSL (find_package above).  HTTP content-coding
# decompression (gzip / deflate) uses zlib, wrapped behind
# neko::network::compression.
# =============================================================================

find_package(OpenSSL REQUIRED)

add_library(neko_network
  src/socket.cpp
  src/compression.cpp
  src/tls_socket.cpp
  src/http.cpp
)
add_library(neko::network ALIAS neko_network)

target_compile_features(neko_network PUBLIC cxx_std_20)
target_include_directories(neko_network PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(neko_network PUBLIC neko::url neko::base ZLIB::ZLIB
  OpenSSL::SSL OpenSSL::Crypto)
# Windows socket functions (closesocket/send/recv) live in Winsock2.
if(WIN32)
  target_link_libraries(neko_network PUBLIC ws2_32)
endif()
apply_compiler_warnings(neko_network)
