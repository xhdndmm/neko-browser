#pragma once

#include "neko/javascript/dom_binding.h"
#include "neko/javascript/script_engine.h"
#include "neko/renderer/page.h"

#include <memory>

namespace neko::browser {

// Executes the page's inline <script> elements (in document order) through a
// fresh JavaScript runtime with DOM bindings, then re-applies the style
// cascade so DOM mutations made by the scripts are reflected.
//
// Only inline scripts are executed: external src= scripts, async/defer and
// module scripts are not fetched or supported yet (documented limitation).
// A failing script logs an error through |sink| (when provided) and does not
// stop the remaining scripts.
//
// Returns the live runtime handle so the caller can keep it for the page's
// lifetime and pump timers / dispatch events (see javascript::DomBinder), or
// nullptr when the page has no scripts (or the runtime failed to start).
std::shared_ptr<javascript::DomBinder> RunPageScripts(renderer::Page& page,
                                                      javascript::ScriptEngine::ConsoleSink sink);

} // namespace neko::browser
