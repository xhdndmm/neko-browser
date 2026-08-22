# =============================================================================
# neko_javascript — JavaScript runtime (Phase 8).
#
# Wraps the QuickJS engine (quickjs-ng, fetched in the root CMakeLists) behind
# a project-owned interface: neko::javascript::ScriptEngine.  Only the core
# language and a project-provided `console` binding are exposed.  The
# third-party headers are PRIVATE — they never leak into neko::javascript's
# public API.
# =============================================================================

add_library(neko_javascript
  src/script_engine.cpp
  src/dom_binding.cpp
  src/binding/impl.cpp
  src/import_map.cpp
  src/binding/node_binding.cpp
  src/binding/element_binding.cpp
  src/binding/document_binding.cpp
  src/binding/style_binding.cpp
  src/binding/event_binding.cpp
  src/binding/storage_binding.cpp
  src/binding/global_binding.cpp
  src/binding/ui_binding.cpp
  src/binding/xhr_binding.cpp
)
add_library(neko::javascript ALIAS neko_javascript)

target_compile_features(neko_javascript PUBLIC cxx_std_20)
target_include_directories(neko_javascript PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# DOM bindings (Phase 8 M2) make the module depend on the DOM/HTML/CSS
# layers; none of those depend back on javascript, so there is no cycle.
target_link_libraries(neko_javascript PUBLIC neko::base neko::url neko::dom neko::css neko::html PRIVATE qjs)
# QuickJS is a third-party dependency; mark its headers SYSTEM so our strict
# warnings (-Wold-style-cast / -Wdouble-promotion) don't fire inside quickjs.h.
target_include_directories(neko_javascript SYSTEM PRIVATE ${quickjs_SOURCE_DIR})
apply_compiler_warnings(neko_javascript)
