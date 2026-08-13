# =============================================================================
# neko_ui — the Qt6 browser GUI (Phase 7).
#
# The GUI talks only to neko::browser (BrowserController) through
# BrowserWorker, which runs the single-threaded controller on a dedicated
# worker thread so network fetches never block the UI thread.
#
# Views: toolbar + tabs + WebView (rendered HTML / decoded images / text),
# plus DevTools (DOM, network, console) and History / Bookmarks / Downloads /
# Settings docks.  See docs/architecture/architecture.md.
# =============================================================================

find_package(Qt6 REQUIRED COMPONENTS Widgets)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

add_library(neko_ui
  src/browser_worker.cpp
  src/web_view.cpp
  src/main_window.cpp
  include/neko/ui/browser_worker.h
  include/neko/ui/web_view.h
  include/neko/ui/main_window.h
)
add_library(neko::ui ALIAS neko_ui)
target_compile_features(neko_ui PUBLIC cxx_std_20)
target_include_directories(neko_ui PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(neko_ui PUBLIC neko::browser Qt6::Widgets)
apply_compiler_warnings(neko_ui)

add_executable(neko_browser_gui main.cpp)
target_link_libraries(neko_browser_gui PRIVATE neko::ui)
apply_compiler_warnings(neko_browser_gui)

# Development/validation tool: load a URL in the real GUI stack and save a
# window grab as PNG (works headless via the offscreen platform).
add_executable(neko_gui_screenshot screenshot_tool.cpp)
target_link_libraries(neko_gui_screenshot PRIVATE neko::ui)
apply_compiler_warnings(neko_gui_screenshot)
