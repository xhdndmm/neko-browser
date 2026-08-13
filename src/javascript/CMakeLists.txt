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
)
add_library(neko::javascript ALIAS neko_javascript)

target_compile_features(neko_javascript PUBLIC cxx_std_20)
target_include_directories(neko_javascript PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(neko_javascript PUBLIC neko::base PRIVATE qjs)
# QuickJS is a third-party dependency; mark its headers SYSTEM so our strict
# warnings (-Wold-style-cast / -Wdouble-promotion) don't fire inside quickjs.h.
target_include_directories(neko_javascript SYSTEM PRIVATE ${quickjs_SOURCE_DIR})
apply_compiler_warnings(neko_javascript)
