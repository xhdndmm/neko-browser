# =============================================================================
# CompilerWarnings.cmake
#
# A curated, strict warning set applied to every project target.
# When NEKO_WARNINGS_AS_ERRORS is ON the warnings are promoted to errors.
#
# Production targets use apply_compiler_warnings().  Third-party code fetched
# via FetchContent is compiled with its own flags and must NOT be configured
# through this file.
# =============================================================================

# -----------------------------------------------------------------------------
# neko_third_party_system_includes(<target>)
#
# Adds the dependency include directories listed in
# NEKO_THIRD_PARTY_INCLUDE_DIRS (set by the top-level CMakeLists.txt) to
# <target> as explicit system include directories.
#
# CMake already marks include directories that come from imported targets
# (JPEG::JPEG, OpenSSL::Crypto, ...) as SYSTEM, but it *drops* the -isystem flag
# for any directory the compiler reports as an implicit search directory
# (CMAKE_<LANG>_IMPLICIT_INCLUDE_DIRECTORIES).  On macOS the Homebrew prefixes
# (/usr/local/include on Intel, /opt/homebrew/include on Apple Silicon) are such
# directories, so the headers of libjpeg and OpenSSL are found through the
# compiler's default search path.  Some AppleClang toolchains classify that path
# as a *user* directory, and then third-party C headers are compiled with the
# project's -Werror warning set and fail the build -- observed in macOS CI with
# libjpeg's jpeg_create_decompress macro and OpenSSL's safestack.h, both of which
# expand old-style casts (-Wold-style-cast).  Re-adding the flag explicitly keeps
# third-party code out of the project's warning set without weakening anything
# for our own sources: a directory given as both -I and -isystem is treated as a
# system directory by GCC and Clang, so this also holds when another include path
# (CPATH, a stray -I, a compiler default) already points at the same directory.
#
# A #pragma-based suppression is not an alternative: `#pragma clang diagnostic
# ignored` around the #include silences the header's own code, but *not* macro
# expansions that land at a call site in our translation units (verified with
# clang); only a system include directory does.
#
# On other platforms the compiler already treats its default directories
# (/usr/include, ...) as system directories, so this is a no-op there.
# -----------------------------------------------------------------------------
function(neko_third_party_system_includes target)
  if(NOT APPLE)
    return()
  endif()
  if(NOT DEFINED NEKO_THIRD_PARTY_INCLUDE_DIRS)
    message(FATAL_ERROR
      "NEKO_THIRD_PARTY_INCLUDE_DIRS is not defined; it is set by the top-level "
      "CMakeLists.txt and is required to keep third-party headers out of the "
      "project's warning set on Apple platforms.")
  endif()
  foreach(dir IN LISTS NEKO_THIRD_PARTY_INCLUDE_DIRS)
    if(dir AND IS_DIRECTORY "${dir}")
      target_compile_options(${target} PRIVATE "SHELL:-isystem ${dir}")
    endif()
  endforeach()
endfunction()

function(apply_compiler_warnings target)
  neko_third_party_system_includes(${target})
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W4
      /permissive-
      /Zc:__cplusplus
      /EHsc
      /utf-8
    )
    # The code uses standard C/POSIX functions (strerror, fopen, ...) rather
    # than MSVC's _s variants; silence the corresponding deprecation warnings.
    target_compile_definitions(${target} PRIVATE
      _CRT_SECURE_NO_WARNINGS
      _CRT_NONSTDC_NO_WARNINGS
    )
    if(NEKO_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wshadow
      -Wconversion
      -Wsign-conversion
      -Wformat=2
      -Wnull-dereference
      -Wdouble-promotion
      -Wcast-align
      -Woverloaded-virtual
      -Wold-style-cast
      -Wmisleading-indentation
      -Wduplicated-cond
      -Wduplicated-branches
      -Wlogical-op
    )
    # -Wno-unknown-warning-option is Clang-specific; GCC ignores unknown
    # -Wno-* options silently but prints a note.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      target_compile_options(${target} PRIVATE -Wno-unknown-warning-option)
    endif()
    if(NEKO_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()
