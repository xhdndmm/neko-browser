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

function(apply_compiler_warnings target)
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
