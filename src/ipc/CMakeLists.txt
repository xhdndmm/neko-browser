# =============================================================================
# neko_ipc — inter-process communication for the multi-process architecture
# (ADR 0016).
#
# Framed byte-stream channels (POSIX pipes / Windows anonymous pipes) and
# subprocess spawning with stdin/stdout redirection.  Depends only on base;
# the message payloads are owned by the higher-level protocols (e.g. the
# browser's renderer protocol).
# =============================================================================

add_library(neko_ipc
  src/channel.cpp
  src/subprocess.cpp
)
add_library(neko::ipc ALIAS neko_ipc)

target_compile_features(neko_ipc PUBLIC cxx_std_20)
target_include_directories(neko_ipc PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(neko_ipc PUBLIC neko::base)
apply_compiler_warnings(neko_ipc)
