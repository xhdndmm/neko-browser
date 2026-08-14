// neko::browser::RunPageScripts — executes a page's <script> elements
// (inline text or external src=) with DOM bindings (Phase 8 M2 + external
// script loading).  Called on the worker thread right after HTML parsing,
// before the page is published to the UI.
//
// Loading model (see page_scripts.h): classic scripts fetch+run in document
// order (blocking), defer scripts run after all classics in document order,
// and async scripts run after the classic+defer phases in document order
// (a documented approximation of the spec's non-blocking async model).

#include "neko/browser/page_scripts.h"

#include "neko/base/logging.h"
#include "neko/url/url.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neko::browser {
namespace {

// Collects <script> elements in document order (pre-order traversal), so
// scripts run in the order the author wrote them (head before body).
void CollectScripts(const dom::Node& root, std::vector<dom::Element*>& out)
{
  for (dom::Node* child : root.ChildNodes()) {
    if (child->node_type() != dom::NodeType::kElement) {
      continue;
    }
    auto* element = static_cast<dom::Element*>(child);
    if (element->tag_name() == "script") {
      out.push_back(element);
    }
    CollectScripts(*element, out);
  }
}

} // namespace

std::shared_ptr<javascript::DomBinder> RunPageScripts(renderer::Page& page,
                                                      const std::string& base_url,
                                                      ScriptFetcher fetch,
                                                      javascript::ScriptEngine::ConsoleSink sink)
{
  dom::Document* document = page.document();
  if (document == nullptr) {
    return nullptr;
  }
  std::vector<dom::Element*> scripts;
  CollectScripts(*document, scripts);
  if (scripts.empty()) {
    return nullptr;
  }

  // Keep a local copy of the sink for script-error reporting; the binder's
  // engine takes its own copy for console.log etc.
  javascript::ScriptEngine::ConsoleSink error_sink = sink;
  auto binder = std::make_shared<javascript::DomBinder>(*document);
  binder->SetConsoleSink(std::move(sink));

  const base::Result<url::Url> base = url::Url::Parse(base_url);

  // Runs one script body (inline text or fetched external file); failures are
  // logged and do not stop the remaining scripts.
  auto run_source = [&](std::string_view source, std::string_view filename) {
    const auto result = binder->Evaluate(source, filename);
    if (!result.has_value()) {
      const std::string message = "Uncaught " + result.error().message();
      NEKO_LOG_WARNING("page script error: " + message);
      if (error_sink) {
        error_sink("error", message);
      }
    }
  };

  // Returns the executable source of one <script>, or nullopt when the
  // element must be skipped (empty, or its external fetch failed).
  auto script_source = [&](dom::Element* script) -> std::optional<std::string> {
    if (script->HasAttribute("src")) {
      const std::optional<std::string_view> src = script->GetAttribute("src");
      base::Result<url::Url> target =
          base.has_value() ? url::Url::Parse(*src, base.value()) : url::Url::Parse(*src);
      if (!target.has_value()) {
        const std::string message = "script: cannot resolve src \"" + std::string(*src) + "\"";
        NEKO_LOG_WARNING(message);
        if (error_sink) {
          error_sink("error", message);
        }
        return std::nullopt;
      }
      const auto response = fetch(target.value());
      if (!response) {
        const std::string message = "script: fetch failed for " + target.value().Serialize() +
                                    ": " + response.error().message();
        NEKO_LOG_WARNING(message);
        if (error_sink) {
          error_sink("error", message);
        }
        return std::nullopt;
      }
      return response.value().body;
    }
    const std::string source = script->TextContent();
    return source.empty() ? std::nullopt : std::optional<std::string>(source);
  };

  // Pass 1: classic scripts (no async, no defer) in document order.
  for (dom::Element* script : scripts) {
    if (script->HasAttribute("async") || script->HasAttribute("defer")) {
      continue;
    }
    const std::optional<std::string> source = script_source(script);
    if (source.has_value()) {
      run_source(source.value(), "inline-script");
    }
  }
  // Pass 2: defer scripts in document order (they run after parsing, which
  // has already completed — same observable phase as classic here, but after
  // every classic script regardless of source position).
  for (dom::Element* script : scripts) {
    if (!script->HasAttribute("defer") || script->HasAttribute("async")) {
      continue;
    }
    const std::optional<std::string> source = script_source(script);
    if (source.has_value()) {
      run_source(source.value(), "deferred-script");
    }
  }
  // Pass 3: async scripts in document order (approximation of the spec's
  // run-when-ready model).
  for (dom::Element* script : scripts) {
    if (!script->HasAttribute("async")) {
      continue;
    }
    const std::optional<std::string> source = script_source(script);
    if (source.has_value()) {
      run_source(source.value(), "async-script");
    }
  }

  // Fire the document lifecycle events now that every script has run:
  // parsing is complete, so DOMContentLoaded then load fire on the document
  // (window-level listeners were registered on the document, so they run
  // too).  Approximation: real browsers fire load only after all subresources
  // finish, which this synchronous model has no signal for.
  binder->DispatchDocumentEvent("DOMContentLoaded");
  binder->DispatchDocumentEvent("load");

  // Scripts may have mutated the DOM; re-run the cascade so layout reflects
  // the new state.
  page.ReapplyStyles();
  return binder;
}

} // namespace neko::browser
