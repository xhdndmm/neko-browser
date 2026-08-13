# =============================================================================
# Sanitizers.cmake
#
# Global sanitizer and coverage instrumentation.
#
# NEKO_SANITIZERS is a semicolon-separated list of sanitizers, e.g.
#   "address;undefined"   (ASan + UBSan)
#   "thread"              (TSan)
#
# The sanitizer presets target GCC/Clang on Linux/macOS.  MSVC support for
# AddressSanitizer (/fsanitize=address) can be added when a concrete need
# arises; see docs/development/sanitizers.md.
# =============================================================================

if(NEKO_SANITIZERS AND NOT MSVC)
  # The cache variable is a CMake list (semicolon-separated); the compiler
  # flag needs comma separation: -fsanitize=address,undefined
  string(REPLACE ";" "," NEKO_SANITIZER_FLAGS "${NEKO_SANITIZERS}")
  add_compile_options(-fsanitize=${NEKO_SANITIZER_FLAGS} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${NEKO_SANITIZER_FLAGS})
endif()

if(NEKO_ENABLE_COVERAGE AND NOT MSVC)
  # gcov-based coverage (GCC/Clang --coverage). Collect with lcov/gcovr.
  add_compile_options(--coverage -fno-omit-frame-pointer)
  add_link_options(--coverage)
endif()
