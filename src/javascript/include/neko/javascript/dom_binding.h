#pragma once

#include "neko/base/status.h"
#include "neko/dom/element.h"
#include "neko/javascript/script_engine.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace neko::javascript {

// Opaque per-document binding state (defined in dom_binding.cpp).
struct Impl;

// Binds a DOM document into a JavaScript runtime, exposing a practical subset
// of the DOM APIs (Web IDL-inspired) plus a minimal event loop.
//
// This is Phase 8 M2 (DOM bindings / page <script> execution).  The supported
// surface:
//
//   globals:    document, window, setTimeout, clearTimeout, setInterval,
//               clearInterval
//   Document:   documentElement, body, title (get/set), getElementById,
//               querySelector(All), createElement, createTextNode
//   Node:       nodeType, nodeName, textContent (get/set), parentNode,
//               firstChild, lastChild, childNodes, appendChild, insertBefore,
//               removeChild, hasChildNodes, cloneNode, addEventListener,
//               removeEventListener, dispatchEvent
//   Element:    tagName, id (get/set), className (get/set), attributes,
//               getAttribute/setAttribute/removeAttribute/hasAttribute,
//               children, firstElementChild, querySelector(All),
//               getElementsByTagName, getElementsByClassName,
//               innerHTML (get/set), style (CSSStyleDeclaration)
//   Style:      setProperty/getPropertyValue/removeProperty plus direct
//               accessors for a documented subset of properties
//
// NOT implemented (documented limitation): property getters beyond the above,
// live NodeList objects (childNodes is a snapshot array), event propagation
// (bubbling/capture/default actions), async script loading (async/defer),
// module scripts, and full CSSOM.
//
// Ownership and lifetime:
//   * The binder owns its own ScriptEngine (one runtime per document).
//   * |document| must outlive the binder.  Typically a binder is created per
//     page load and destroyed together with the document.
//   * Node wrappers keep their underlying C++ node reachable for as long as
//     the binder lives: nodes removed from the tree are retained (not freed)
//     until the binder is destroyed, so JS can re-insert them safely.
//   * Elements created via createElement/createTextNode are owned by the
//     binder until appended into the document.
//
// Threading: thread-confined, like ScriptEngine.  Use a binder from one
// thread at a time.
class DomBinder
{
public:
  explicit DomBinder(dom::Document& document);
  ~DomBinder();

  DomBinder(const DomBinder&) = delete;
  DomBinder& operator=(const DomBinder&) = delete;

  // Evaluates a script in the page's global scope (used for <script>
  // execution).  Syntax errors surface as Error::Parse; runtime errors as
  // Error::Javascript.
  base::Result<ScriptValue> Evaluate(std::string_view source, std::string_view filename = "script");

  // Redirects console output from page scripts.  Default: dropped.
  void SetConsoleSink(ScriptEngine::ConsoleSink sink);

  // ---- Event loop ---------------------------------------------------------
  // Runs every timer whose deadline has passed (setTimeout/setInterval).
  // Returns the number of timers executed.
  int RunPendingTimers();

  // The deadline of the next pending timer, or nullopt when none.
  [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> NextTimerDeadline() const;

  // Dispatches a synthetic event of |type| to |element|; registered listeners
  // run synchronously (no bubbling).  No default action is defined for any
  // event type.
  void DispatchEvent(dom::Element& element, std::string_view type);

  // Underlying engine (for tests and integration).
  ScriptEngine& engine();
  [[nodiscard]] dom::Document& document() const;

private:
  std::unique_ptr<Impl> impl_;
};

} // namespace neko::javascript
