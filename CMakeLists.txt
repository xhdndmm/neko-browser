# =============================================================================
# neko-browser
#
# A from-scratch, cross-platform browser engine and browser application.
# Primary language: C++20.
#
# Phase 0: Project bootstrap.  This build file only wires up the build
# infrastructure; browser functionality arrives in later phases.
# =============================================================================

cmake_minimum_required(VERSION 3.24)

project(
  neko-browser
  VERSION 0.1.0
  DESCRIPTION "A from-scratch, cross-platform browser engine and browser application"
  HOMEPAGE_URL "https://github.com/xhdndmm/neko-browser"
  LANGUAGES CXX
)

# -----------------------------------------------------------------------------
# Build options
# -----------------------------------------------------------------------------
option(NEKO_BUILD_TESTS        "Build the unit / integration test suite" ON)
option(NEKO_WARNINGS_AS_ERRORS "Treat compiler warnings as errors"        OFF)
option(NEKO_ENABLE_LTO         "Enable link-time optimization"            OFF)
option(NEKO_ENABLE_COVERAGE    "Enable code coverage instrumentation"     OFF)

# Semicolon-separated list of sanitizers, e.g. "address;undefined".
# Supported values depend on the compiler (GCC/Clang: address, undefined,
# thread, leak, ...). See docs/development/sanitizers.md.
set(NEKO_SANITIZERS "" CACHE STRING "Sanitizers to enable (e.g. 'address;undefined')")

# -----------------------------------------------------------------------------
# Language & toolchain baseline
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Pick a sensible default build type for single-config generators.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# Keep all produced artifacts under a single, predictable location.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

if(NEKO_ENABLE_LTO)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

find_package(Threads REQUIRED)
# Image decoding infrastructure: zlib (inflate for PNG IDAT) and libjpeg
# (baseline JPEG decode).  Both are wrapped behind neko::image's own
# interface; see docs/development/dependency-policy.md.
find_package(ZLIB REQUIRED)
find_package(JPEG REQUIRED)

# -----------------------------------------------------------------------------
# Global tooling integration (warnings, sanitizers, coverage)
# -----------------------------------------------------------------------------
include(cmake/CompilerWarnings.cmake)
include(cmake/Sanitizers.cmake)

# ctest entry point; test suites register via add_test().
enable_testing()

# -----------------------------------------------------------------------------
# Subdirectories
# -----------------------------------------------------------------------------
add_subdirectory(src)

if(NEKO_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# -----------------------------------------------------------------------------
# Configure summary
# -----------------------------------------------------------------------------
message(STATUS "")
message(STATUS "neko-browser ${PROJECT_VERSION} (${CMAKE_BUILD_TYPE})")
message(STATUS "  C++ standard        : C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Compiler            : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Generator           : ${CMAKE_GENERATOR}")
message(STATUS "  Tests               : ${NEKO_BUILD_TESTS}")
message(STATUS "  Warnings as errors  : ${NEKO_WARNINGS_AS_ERRORS}")
message(STATUS "  Sanitizers          : '${NEKO_SANITIZERS}'")
message(STATUS "  Coverage            : ${NEKO_ENABLE_COVERAGE}")
message(STATUS "  LTO                 : ${NEKO_ENABLE_LTO}")
message(STATUS "  ZLIB                : ${ZLIB_VERSION}")
message(STATUS "  JPEG                : ${JPEG_VERSION}")
message(STATUS "")
