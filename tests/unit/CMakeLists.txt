# =============================================================================
# Unit test suite.
#
# GoogleTest is fetched via FetchContent and pinned to a specific release so
# builds are reproducible.  See docs/development/dependency-policy.md.
# =============================================================================

include(FetchContent)
include(GoogleTest)

set(FETCHCONTENT_QUIET OFF)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(googletest)

# Treat vendored GTest headers as system headers so our strict warning set
# only ever sees project code.
foreach(_tgt gtest gtest_main gmock gmock_main)
  if(TARGET ${_tgt})
    get_target_property(_inc ${_tgt} INTERFACE_INCLUDE_DIRECTORIES)
    if(_inc)
      target_include_directories(${_tgt} SYSTEM INTERFACE ${_inc})
    endif()
  endif()
endforeach()

# -----------------------------------------------------------------------------
# neko_base_tests
# -----------------------------------------------------------------------------
add_executable(neko_base_tests
  base/status_test.cpp
  base/string_util_test.cpp
  base/logging_test.cpp
  base/encoding_test.cpp
  base/thread_pool_test.cpp
)
target_link_libraries(neko_base_tests PRIVATE neko::base gtest_main)
apply_compiler_warnings(neko_base_tests)
# PRE_TEST discovery: test binaries are enumerated at ctest time, not during
# the build.  This stays robust when a binary cannot run at build time (e.g.
# TSan under high-entropy ASLR kernels).
gtest_discover_tests(neko_base_tests DISCOVERY_MODE PRE_TEST)

# -----------------------------------------------------------------------------
# neko_url_tests
# -----------------------------------------------------------------------------
add_executable(neko_url_tests
  url/url_test.cpp
)
target_link_libraries(neko_url_tests PRIVATE neko::url gtest_main)
apply_compiler_warnings(neko_url_tests)
gtest_discover_tests(neko_url_tests DISCOVERY_MODE PRE_TEST)

# -----------------------------------------------------------------------------
# neko_dom_tests
# -----------------------------------------------------------------------------
add_executable(neko_dom_tests
  dom/dom_test.cpp
)
target_link_libraries(neko_dom_tests PRIVATE neko::dom gtest_main)
apply_compiler_warnings(neko_dom_tests)
gtest_discover_tests(neko_dom_tests DISCOVERY_MODE PRE_TEST)

# -----------------------------------------------------------------------------
# neko_html_tests
# -----------------------------------------------------------------------------
add_executable(neko_html_tests
  html/html_test.cpp
)
target_link_libraries(neko_html_tests PRIVATE neko::html gtest_main)
apply_compiler_warnings(neko_html_tests)
gtest_discover_tests(neko_html_tests DISCOVERY_MODE PRE_TEST)

# -----------------------------------------------------------------------------
# neko_css_tests
# -----------------------------------------------------------------------------
add_executable(neko_css_tests
  css/css_test.cpp
)
target_link_libraries(neko_css_tests PRIVATE neko::css gtest_main)
apply_compiler_warnings(neko_css_tests)
gtest_discover_tests(neko_css_tests DISCOVERY_MODE PRE_TEST)

# -----------------------------------------------------------------------------
# neko_style_tests
# -----------------------------------------------------------------------------
add_executable(neko_style_tests
  style/style_test.cpp
)
target_link_libraries(neko_style_tests PRIVATE neko::style neko::html gtest_main)
apply_compiler_warnings(neko_style_tests)
gtest_discover_tests(neko_style_tests DISCOVERY_MODE PRE_TEST)

# -----------------------------------------------------------------------------
# neko_layout_tests
# -----------------------------------------------------------------------------
add_executable(neko_layout_tests
  layout/layout_test.cpp
)
target_link_libraries(neko_layout_tests PRIVATE neko::layout neko::html gtest_main)
apply_compiler_warnings(neko_layout_tests)
gtest_discover_tests(neko_layout_tests DISCOVERY_MODE PRE_TEST)

# -----------------------------------------------------------------------------
# neko_paint_tests
# -----------------------------------------------------------------------------
add_executable(neko_paint_tests
  paint/paint_test.cpp
)
target_link_libraries(neko_paint_tests PRIVATE neko::paint neko::html neko::base gtest_main)
apply_compiler_warnings(neko_paint_tests)
gtest_discover_tests(neko_paint_tests DISCOVERY_MODE PRE_TEST)

# -----------------------------------------------------------------------------
# neko_compositor_tests
# -----------------------------------------------------------------------------
add_executable(neko_compositor_tests
  compositor/compositor_test.cpp
)
target_link_libraries(neko_compositor_tests PRIVATE neko::compositor gtest_main)
apply_compiler_warnings(neko_compositor_tests)
gtest_discover_tests(neko_compositor_tests DISCOVERY_MODE PRE_TEST)

# -----------------------------------------------------------------------------
# neko_graphics_tests
# -----------------------------------------------------------------------------
add_executable(neko_graphics_tests
  graphics/graphics_test.cpp
)
target_link_libraries(neko_graphics_tests PRIVATE neko::graphics gtest_main)
apply_compiler_warnings(neko_graphics_tests)
gtest_discover_tests(neko_graphics_tests DISCOVERY_MODE PRE_TEST)

# -----------------------------------------------------------------------------
# neko_network_tests
# -----------------------------------------------------------------------------
add_executable(neko_network_tests
  network/network_test.cpp
)
target_link_libraries(neko_network_tests PRIVATE neko::network ZLIB::ZLIB
  OpenSSL::SSL OpenSSL::Crypto gtest_main)
apply_compiler_warnings(neko_network_tests)
gtest_discover_tests(neko_network_tests DISCOVERY_MODE PRE_TEST)

# -----------------------------------------------------------------------------
# neko_renderer_tests
# -----------------------------------------------------------------------------
add_executable(neko_renderer_tests
  renderer/renderer_test.cpp
)
target_link_libraries(neko_renderer_tests PRIVATE neko::renderer gtest_main)
apply_compiler_warnings(neko_renderer_tests)
gtest_discover_tests(neko_renderer_tests DISCOVERY_MODE PRE_TEST)

# -----------------------------------------------------------------------------
# neko_image_tests
# -----------------------------------------------------------------------------
add_executable(neko_image_tests
  image/image_test.cpp
)
target_link_libraries(neko_image_tests PRIVATE neko::image gtest_main)
apply_compiler_warnings(neko_image_tests)
gtest_discover_tests(neko_image_tests DISCOVERY_MODE PRE_TEST)

# -----------------------------------------------------------------------------
# neko_media_tests
# -----------------------------------------------------------------------------
add_executable(neko_media_tests
  media/media_test.cpp
)
target_link_libraries(neko_media_tests PRIVATE neko::media gtest_main)
target_compile_definitions(neko_media_tests PRIVATE
  NEKO_TEST_PAGES_DIR="${CMAKE_SOURCE_DIR}/tests/pages"
)
apply_compiler_warnings(neko_media_tests)
gtest_discover_tests(neko_media_tests DISCOVERY_MODE PRE_TEST)

# -----------------------------------------------------------------------------
# neko_pdf_tests
# -----------------------------------------------------------------------------
add_executable(neko_pdf_tests
  pdf/pdf_test.cpp
)
target_link_libraries(neko_pdf_tests PRIVATE neko::pdf gmock_main)
apply_compiler_warnings(neko_pdf_tests)
gtest_discover_tests(neko_pdf_tests DISCOVERY_MODE PRE_TEST)

# -----------------------------------------------------------------------------
# neko_storage_tests
# -----------------------------------------------------------------------------
add_executable(neko_storage_tests
  storage/storage_test.cpp
  storage/idb_test.cpp
)
target_link_libraries(neko_storage_tests PRIVATE neko::storage gmock_main)
apply_compiler_warnings(neko_storage_tests)
gtest_discover_tests(neko_storage_tests DISCOVERY_MODE PRE_TEST)

# -----------------------------------------------------------------------------
# neko_ipc_tests
# -----------------------------------------------------------------------------
add_executable(neko_ipc_tests
  ipc/ipc_test.cpp
)
target_link_libraries(neko_ipc_tests PRIVATE neko::ipc gtest_main)
apply_compiler_warnings(neko_ipc_tests)
gtest_discover_tests(neko_ipc_tests DISCOVERY_MODE PRE_TEST)

# -----------------------------------------------------------------------------
# neko_browser_tests
# -----------------------------------------------------------------------------
add_executable(neko_browser_tests
  browser/options_test.cpp
  browser/browser_controller_test.cpp
  browser/hyperlink_test.cpp
  browser/renderer_protocol_test.cpp
)
target_link_libraries(neko_browser_tests PRIVATE neko::browser neko::browser_options gmock_main)
target_compile_definitions(neko_browser_tests PRIVATE
  NEKO_TEST_PAGES_DIR="${CMAKE_SOURCE_DIR}/tests/pages"
  NEKO_BROWSER_BIN="$<TARGET_FILE:neko_browser>"
)
apply_compiler_warnings(neko_browser_tests)
gtest_discover_tests(neko_browser_tests DISCOVERY_MODE PRE_TEST)

# -----------------------------------------------------------------------------
# neko_security_tests
# -----------------------------------------------------------------------------
add_executable(neko_security_tests
  security/origin_test.cpp
)
target_link_libraries(neko_security_tests PRIVATE neko::security gtest_main)
apply_compiler_warnings(neko_security_tests)
gtest_discover_tests(neko_security_tests DISCOVERY_MODE PRE_TEST)

# -----------------------------------------------------------------------------
# neko_javascript_tests
# -----------------------------------------------------------------------------
add_executable(neko_javascript_tests
  javascript/javascript_test.cpp
  javascript/dom_binding_test.cpp
  javascript/event_test.cpp
)
target_link_libraries(neko_javascript_tests PRIVATE neko::javascript neko::storage gtest_main)
apply_compiler_warnings(neko_javascript_tests)
gtest_discover_tests(neko_javascript_tests DISCOVERY_MODE PRE_TEST)

# -----------------------------------------------------------------------------
# neko_ui_tests (Qt offscreen smoke tests)
# -----------------------------------------------------------------------------
if(NEKO_BUILD_UI)
  add_executable(neko_ui_tests
    ui/ui_smoke_test.cpp
  )
  target_link_libraries(neko_ui_tests PRIVATE neko::ui neko::storage gtest)
  apply_compiler_warnings(neko_ui_tests)
  # The Qt offscreen platform plugin lets the tests run without a display.
  gtest_discover_tests(neko_ui_tests DISCOVERY_MODE PRE_TEST
    PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
  )
endif()

# -----------------------------------------------------------------------------
# Executable smoke tests (integration-style, exercise the real binary).
# -----------------------------------------------------------------------------
add_test(NAME neko_browser_version COMMAND neko_browser --version)
set_tests_properties(neko_browser_version PROPERTIES
  PASS_REGULAR_EXPRESSION "neko-browser [0-9]+\\.[0-9]+\\.[0-9]+"
)

add_test(NAME neko_browser_help COMMAND neko_browser --help)
set_tests_properties(neko_browser_help PROPERTIES
  PASS_REGULAR_EXPRESSION "Usage: neko-browser"
)
