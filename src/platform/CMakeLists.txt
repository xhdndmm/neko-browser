# =============================================================================
# neko_platform — OS filesystem/runtime primitives (AGENTS.md §11).
#
# Win32 and POSIX calls live ONLY here: engine code depends on this module's
# interfaces, and each OS has its own translation unit under src/posix or
# src/windows (no #ifdef'd OS calls outside this directory).
# =============================================================================

if(WIN32)
  set(_neko_platform_sources
    src/windows/fs.cpp
    src/windows/pipe.cpp
    src/windows/process.cpp)
else()
  set(_neko_platform_sources
    src/posix/fs.cpp
    src/posix/pipe.cpp
    src/posix/process.cpp)
endif()

add_library(neko_platform ${_neko_platform_sources})
add_library(neko::platform ALIAS neko_platform)

target_compile_features(neko_platform PUBLIC cxx_std_20)
target_include_directories(neko_platform PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(neko_platform PUBLIC neko::base)
apply_compiler_warnings(neko_platform)
