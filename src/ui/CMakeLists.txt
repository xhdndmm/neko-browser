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

# --- Translations (i18n) -----------------------------------------------------
# The .ts catalogs in src/ui/i18n are the sources of truth; the committed .qm
# files next to them are the fallback for build machines without Qt Linguist
# (for example the aqtinstall qtbase packages used by CI).  Catalogs are
# prepared at configure time so the resource compiler always reads them from
# the build directory.  Re-run CMake (or tools/update_translations.sh) after
# changing UI strings.
set(NEKO_I18N_LANGS zh_CN zh_TW ja ko ar ru)
find_package(Qt6 QUIET COMPONENTS LinguistTools)
set(NEKO_I18N_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/i18n)
file(MAKE_DIRECTORY ${NEKO_I18N_BUILD_DIR})
foreach(lang IN LISTS NEKO_I18N_LANGS)
  set(ts_file ${CMAKE_CURRENT_SOURCE_DIR}/i18n/neko_${lang}.ts)
  set(qm_file ${NEKO_I18N_BUILD_DIR}/neko_${lang}.qm)
  if(Qt6LinguistTools_FOUND AND Qt6_LRELEASE_EXECUTABLE)
    execute_process(
      COMMAND ${Qt6_LRELEASE_EXECUTABLE} -silent ${ts_file} -qm ${qm_file}
      RESULT_VARIABLE neko_lrelease_result)
    if(NOT neko_lrelease_result EQUAL 0)
      message(FATAL_ERROR "lrelease failed for ${ts_file}")
    endif()
  else()
    file(COPY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/i18n/neko_${lang}.qm ${qm_file}
         ONLY_IF_DIFFERENT)
  endif()
endforeach()

# A generated .qrc makes the catalogs loadable from ":/i18n/..." in every
# executable that links neko_ui (no install-time file lookup).
set(NEKO_I18N_QRC ${NEKO_I18N_BUILD_DIR}/translations.qrc)
set(neko_qrc_content "<RCC>\n  <qresource prefix=\"/i18n\">\n")
foreach(lang IN LISTS NEKO_I18N_LANGS)
  string(APPEND neko_qrc_content "    <file>neko_${lang}.qm</file>\n")
endforeach()
string(APPEND neko_qrc_content "  </qresource>\n</RCC>\n")
file(WRITE ${NEKO_I18N_QRC} "${neko_qrc_content}")

# The Qt imported targets live in this directory scope; verify they resolve to
# real library files (a Qt package without locations would drop out of every
# link line later — see cmake/CheckLinkableDependencies.cmake).
neko_verify_linkable_dependencies(Qt6::Widgets)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

add_library(neko_ui
  src/browser_worker.cpp
  src/web_view.cpp
  src/main_window.cpp
  src/i18n.cpp
  ${NEKO_I18N_QRC}
  include/neko/ui/browser_worker.h
  include/neko/ui/web_view.h
  include/neko/ui/main_window.h
  include/neko/ui/i18n.h
)
add_library(neko::ui ALIAS neko_ui)
target_compile_features(neko_ui PUBLIC cxx_std_20)
target_include_directories(neko_ui PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(neko_ui PUBLIC neko::browser neko::javascript neko::compositor Qt6::Widgets)
apply_compiler_warnings(neko_ui)

add_executable(neko_browser_gui main.cpp)
target_link_libraries(neko_browser_gui PRIVATE neko::ui)
apply_compiler_warnings(neko_browser_gui)

# Development/validation tool: load a URL in the real GUI stack and save a
# window grab as PNG (works headless via the offscreen platform).
add_executable(neko_gui_screenshot screenshot_tool.cpp)
target_link_libraries(neko_gui_screenshot PRIVATE neko::ui)
apply_compiler_warnings(neko_gui_screenshot)
