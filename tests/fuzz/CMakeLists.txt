# =============================================================================
# libFuzzer harnesses for the untrusted-input parsers (AGENTS.md §32).
#
# Opt-in: configure with -DNEKO_BUILD_FUZZERS=ON under Clang.  The harness
# bodies live in fuzz_targets.cpp and are shared with the deterministic
# regression suite (tests/unit/fuzz/fuzz_regression_test.cpp) which replays
# the committed corpora on every build on every toolchain.
#
# Usage:
#   cmake --preset debug -DNEKO_BUILD_FUZZERS=ON   (Clang)
#   ./build/tests/fuzz/neko_fuzz_html tests/fuzz/corpus/html -max_total_time=60
#   ./build/tests/fuzz/neko_fuzz_html crash-<hash>   # minimize / reproduce
#
# Every reproducible crash input is committed under tests/fuzz/corpus/<area>/
# so it becomes a permanent regression test.  See docs/testing/fuzzing.md.
# =============================================================================

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(WARNING "NEKO_BUILD_FUZZERS requires Clang (libFuzzer); fuzz targets not built")
  return()
endif()

# neko_add_fuzzer(<name> <source>): one libFuzzer executable per parser.  All
# harness bodies live in the shared fuzz_targets.cpp, so every target links
# all four parser modules.
set(NEKO_FUZZ_MODULES neko::url neko::html neko::css neko::network ZLIB::ZLIB
  OpenSSL::SSL OpenSSL::Crypto)

function(neko_add_fuzzer name)
  add_executable(${name} ${ARGN} fuzz_targets.cpp)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${name} PRIVATE ${NEKO_FUZZ_MODULES})
  target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
  target_link_options(${name} PRIVATE -fsanitize=fuzzer)
  apply_compiler_warnings(${name})
endfunction()

neko_add_fuzzer(neko_fuzz_url fuzz_url.cpp)

neko_add_fuzzer(neko_fuzz_http fuzz_http.cpp)

neko_add_fuzzer(neko_fuzz_html fuzz_html.cpp)

neko_add_fuzzer(neko_fuzz_css fuzz_css.cpp)
