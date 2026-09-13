# =============================================================================
# Regression test for neko_filter_third_party_include_dirs()
# (cmake/CompilerWarnings.cmake), driven by ctest as
# build_system.third_party_include_filter.
#
# The filter decides which dependency include directories
# apply_compiler_warnings() may re-declare with -isystem.  A directory that
# provides the C standard library's own headers must never be passed on: a user
# -isystem directory is searched ahead of the C++ standard library, so libc++'s
# C compatibility headers would be shadowed and <cstddef> / <cstdint> would
# fail to compile -- the macOS CI failure this filter fixes.
# =============================================================================

cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED NEKO_MODULE_DIR OR NOT DEFINED NEKO_TEST_SCRATCH_DIR)
  message(FATAL_ERROR "NEKO_MODULE_DIR and NEKO_TEST_SCRATCH_DIR are required")
endif()

set(_scratch "${NEKO_TEST_SCRATCH_DIR}")
file(REMOVE_RECURSE "${_scratch}")
file(MAKE_DIRECTORY
  "${_scratch}/sdk" # toolchain directory: provides the C standard library
  "${_scratch}/vendor" # dependency prefix
  "${_scratch}/vendor/freetype2"
)
file(WRITE "${_scratch}/sdk/stddef.h" "")
file(WRITE "${_scratch}/sdk/stdint.h" "")
file(WRITE "${_scratch}/vendor/jpeglib.h" "")
file(WRITE "${_scratch}/vendor/freetype2/ft2build.h" "")

include("${NEKO_MODULE_DIR}/CompilerWarnings.cmake")

set(_input
  "${_scratch}/sdk" # must be dropped: C standard library headers
  "${_scratch}/vendor" # must be kept
  "${_scratch}/vendor/freetype2" # must be kept
  "${_scratch}/missing" # must be dropped: not a directory
)
neko_filter_third_party_include_dirs(_kept _input)

set(_want "${_scratch}/vendor;${_scratch}/vendor/freetype2")
if(NOT "${_kept}" STREQUAL "${_want}")
  message(FATAL_ERROR "filter kept '${_kept}', expected '${_want}'")
endif()

# ... and the opposite shape: a plain dependency prefix without C headers is
# kept, while a directory holding only stdint.h is dropped as well.
file(MAKE_DIRECTORY "${_scratch}/sdk-int-only")
file(WRITE "${_scratch}/sdk-int-only/stdint.h" "")
set(_input "${_scratch}/sdk-int-only" "${_scratch}/vendor")
neko_filter_third_party_include_dirs(_int_only_kept _input)
if(NOT "${_int_only_kept}" STREQUAL "${_scratch}/vendor")
  message(FATAL_ERROR "stdint-only directory was kept: '${_int_only_kept}'")
endif()

# An empty input must stay empty rather than gaining a stray empty element.
set(_none "")
neko_filter_third_party_include_dirs(_empty _none)
if(NOT "${_empty}" STREQUAL "")
  message(FATAL_ERROR "filter turned an empty list into '${_empty}'")
endif()

message(STATUS "third-party include filter: OK")
