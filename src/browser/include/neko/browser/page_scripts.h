#pragma once

#include "neko/javascript/dom_binding.h"
#include "neko/javascript/script_engine.h"
#include "neko/network/http.h"
#include "neko/renderer/page.h"
#include "neko/url/url.h"

#include <functional>
#include <memory>
#include <string>

namespace neko::browser {

// Fetches an external script by URL.  Production uses the network stack with
// the page's cookies; tests inject a fake.  Called on the worker thread.
using ScriptFetcher = std::function<base::Result<network::HttpResponse>(const url::Url& url)>;

// Executes the page's <script> elements (inline text or external src=) through
// a fresh JavaScript runtime with DOM bindings, then re-applies the style
// cascade so DOM mutations made by the scripts are reflected.
//
// Script loading follows the classic model (WHATWG HTML §4.12.1):
//   * classic scripts (no async/defer) are fetched and executed in document
//     order, blocking — later scripts wait;
//   * defer scripts are executed after all classic scripts, in document order
//     (parsing is already complete when this runs, which matches the spec's
//     "run after parsing" phase);
//   * async scripts are fetched and executed after the classic+defer phases,
//     in document order — a documented approximation: in a synchronous engine
//     they never preempt the pipeline, so they cannot run before a later
//     classic script that is ready earlier.
// module scripts and dynamic import are not supported (documented limitation).
//
// A failing script (parse/runtime/fetch) logs an error through |sink| (when
// provided) and does not stop the remaining scripts.
//
// Returns the live runtime handle so the caller can keep it for the page's
// lifetime and pump timers / dispatch events (see javascript::DomBinder), or
// nullptr when the page has no scripts (or the runtime failed to start).
std::shared_ptr<javascript::DomBinder> RunPageScripts(renderer::Page& page,
                                                      const std::string& base_url,
                                                      ScriptFetcher fetch,
                                                      javascript::ScriptEngine::ConsoleSink sink);

} // namespace neko::browser
