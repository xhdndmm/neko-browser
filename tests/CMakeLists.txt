# =============================================================================
# Test suites.  Only included when NEKO_BUILD_TESTS is ON (root CMakeLists).
#
# Layout:
#   tests/unit/       fast unit tests (no I/O beyond temp files)
#   tests/integration/  (Phase 2+) engine-level integration tests
#   tests/network/    (Phase 2+) network stack tests
#   tests/html/       (Phase 3+)
#   tests/css/        (Phase 4+)
#   tests/layout/     (Phase 5+)
#   tests/rendering/  (Phase 6+)
#   tests/fuzz/       (Phase 1+)
#   tests/web-platform/ (Phase 13+)
# =============================================================================

add_subdirectory(unit)

# -----------------------------------------------------------------------------
# Build-system regression test (no compiled target): the third-party include
# filter must keep directories that provide the C standard library headers out
# of the list apply_compiler_warnings() re-declares with -isystem -- doing so
# put them ahead of the C++ standard library and broke <cstddef>/<cstdint> on
# macOS CI.  See cmake/CompilerWarnings.cmake.
# -----------------------------------------------------------------------------
add_test(
  NAME build_system.third_party_include_filter
  COMMAND ${CMAKE_COMMAND}
    -DNEKO_MODULE_DIR=${PROJECT_SOURCE_DIR}/cmake
    -DNEKO_TEST_SCRATCH_DIR=${CMAKE_CURRENT_BINARY_DIR}/third_party_include_filter
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/third_party_include_filter_test.cmake)
