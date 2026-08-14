# =============================================================================
# neko_security — the security subsystem (Phase 10).
#
# Milestone 1: the Origin model (scheme + host + effective port) behind the
# Same-Origin Policy.  Enforcement (SOP on network reads, CORS, CSP, secure
# contexts, permissions) is documented as future work in
# docs/security/security-model.md.
# =============================================================================

add_library(neko_security
  src/origin.cpp
)
add_library(neko::security ALIAS neko_security)

target_compile_features(neko_security PUBLIC cxx_std_20)
target_include_directories(neko_security PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(neko_security PUBLIC neko::base neko::url)
apply_compiler_warnings(neko_security)
