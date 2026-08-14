// neko::browser::RunPageScripts — executes a page's inline <script> elements
// with DOM bindings (Phase 8 M2).  Called on the worker thread right after
// HTML parsing, before the page is published to the UI.

#include "neko/browser/page_scripts.h"

#include "neko/base/logging.h"

#include <memory>
#include <string>
#include <string_view>
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

  for (dom::Element* script : scripts) {
    // Inline only: an external src= script needs a fetch and is not
    // implemented yet (async/defer/module likewise).
    if (script->HasAttribute("src")) {
      continue;
    }
    const std::string source = script->TextContent();
    if (source.empty()) {
      continue;
    }
    const auto result = binder->Evaluate(source);
    if (!result.has_value()) {
      const std::string message = "Uncaught " + result.error().message();
      NEKO_LOG_WARNING("page script error: " + message);
      if (error_sink) {
        error_sink("error", message);
      }
    }
  }

  // Scripts may have mutated the DOM; re-run the cascade so layout reflects
  // the new state.
  page.ReapplyStyles();
  return binder;
}

} // namespace neko::browser
