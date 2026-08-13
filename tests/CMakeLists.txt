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
