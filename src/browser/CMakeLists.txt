# =============================================================================
# neko_browser — the browser application layer.
#
# neko::browser_options : CLI argument parsing (kept small for tests).
# neko::browser          : BrowserController (tabs, navigation, content-type
#                          routing), DownloadManager, DevTools data.
# neko_browser           : CLI executable.
#
# The UI (Qt) layer lives in src/ui and talks only to neko::browser.
# =============================================================================

add_library(neko_browser_options STATIC src/browser_options.cpp)
add_library(neko::browser_options ALIAS neko_browser_options)
target_compile_features(neko_browser_options PUBLIC cxx_std_20)
target_include_directories(neko_browser_options PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(neko_browser_options PUBLIC neko::base)
apply_compiler_warnings(neko_browser_options)

add_library(neko_browser_core
  src/browser_controller.cpp
  src/download_manager.cpp
)
add_library(neko::browser ALIAS neko_browser_core)
target_compile_features(neko_browser_core PUBLIC cxx_std_20)
target_include_directories(neko_browser_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(neko_browser_core PUBLIC
  neko::renderer
  neko::network
  neko::url
  neko::base
  neko::storage
  neko::image
  neko::pdf
  neko::media
)
apply_compiler_warnings(neko_browser_core)

add_executable(neko_browser main.cpp)
target_link_libraries(neko_browser PRIVATE neko::browser neko::browser_options neko::javascript)
apply_compiler_warnings(neko_browser)
