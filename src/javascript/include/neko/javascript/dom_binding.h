#pragma once

#include "neko/base/status.h"
#include "neko/dom/element.h"
#include "neko/javascript/script_engine.h"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neko::javascript {

// Opaque per-document binding state (defined in dom_binding.cpp).
struct Impl;

// The result of a JS fetch(): a minimal subset of the network response, kept
// here so the javascript layer does not depend on the network types.
struct FetchResponse
{
  int status = 0;
  std::string status_text;
  std::string final_url;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
};

// Optional browser Web APIs exposed to page scripts (Phase 8 M3 subset:
// window.localStorage + window.fetch).  Callbacks keep the binder decoupled
// from the storage/network implementations; the browser layer wires them
// (see browser::RunPageScripts).
struct PageApis
{
  // Resolves a (possibly relative) URL against the page base to an absolute
  // URL, or returns empty when it cannot be parsed.
  std::function<std::string(const std::string&)> resolve_url;

  // window.localStorage (per-origin; the caller scopes keys by origin).
  std::function<std::optional<std::string>(std::string_view)> storage_get;
  std::function<void(std::string_view, std::string_view)> storage_set;
  std::function<bool(std::string_view)> storage_remove;
  std::function<void()> storage_clear;
  std::function<std::vector<std::string>()> storage_keys;

  // window.fetch: performs the request for the (absolute) URL string; an Err
  // result rejects the returned promise (network error).
  std::function<base::Result<FetchResponse>(const std::string&)> fetch;

  // window.location.  |location_href| returns the current document URL (used
  // for the read-only parts of location and for resolving relative targets);
  // |navigate| requests navigation to an absolute URL (location.href
  // assignment, assign(), replace()); |reload| requests a reload of the
  // current document.  When a navigation is requested the browser layer acts
  // on it after the synchronous script run (see browser::RunPageScripts), so
  // these never recurse into the network stack mid-script.
  std::function<std::string()> location_href;
  std::function<void(const std::string&)> navigate;
  std::function<void()> reload;

  // window.getComputedStyle(element): serialized computed style of an element
  // (property -> resolved px value, kebab-case keys).  Wired by the browser
  // layer from the renderer's style engine; when absent, getComputedStyle
  // returns an empty object.
  std::function<std::map<std::string, std::string>(const dom::Element&)> computed_style;
};

// Binds a DOM document into a JavaScript runtime, exposing a practical subset
// of the DOM APIs (Web IDL-inspired) plus a minimal event loop.
//
// This is Phase 8 M2 (DOM bindings / page <script> execution).  The supported
// surface:
//
//   globals:    document, window, setTimeout, clearTimeout, setInterval,
//               clearInterval, addEventListener, removeEventListener,
//               dispatchEvent (window aliases), navigator, screen, Event,
//               requestAnimationFrame/cancelAnimationFrame, getComputedStyle,
//               scrollTo/scrollBy, history, performance, plus the DOM
//               interface constructors Node, Element, HTMLElement, Document,
//               Text, Comment, DocumentFragment, CSSStyleDeclaration
//               (whose .prototype is the live wrapper prototype, so `x
//               instanceof Element` and prototype extension work; constructing
//               them directly throws "Illegal constructor")
//   window:     navigator, screen, innerWidth/innerHeight/devicePixelRatio
//               (engine-default viewport 800x600@1x; real window-size wiring
//               is future work), location, localStorage, fetch, history,
//               performance.now(), requestAnimationFrame
//   navigator:  userAgent (same string the network stack sends), platform,
//               language/languages ("en-US" defaults), onLine, cookieEnabled,
//               hardwareConcurrency, vendor
//   screen:     width/height/availWidth/availHeight (800x600),
//               colorDepth/pixelDepth (24)
//   Document:   documentElement, body, head, readyState ("complete"),
//               title (get/set), getElementById, querySelector(All),
//               getElementsByTagName(All), getElementsByClassName,
//               createElement, createElementNS, createTextNode,
//               createDocumentFragment, createComment, URL, baseURI,
//               documentURI, characterSet, contentType, referrer,
//               forms/images/links/scripts (collections)
//   Node:       nodeType, nodeName, textContent (get/set), parentNode,
//               parentElement, firstChild, lastChild, childNodes,
//               nextSibling, previousSibling, ownerDocument, isConnected,
//               appendChild, append, replaceChildren, insertBefore,
//               replaceChild, removeChild, hasChildNodes, cloneNode,
//               contains, normalize, addEventListener, removeEventListener,
//               dispatchEvent
//   Element:    tagName, id (get/set), className (get/set), classList,
//               dataset, attributes, getAttribute/setAttribute/removeAttribute/
//               hasAttribute, children, firstElementChild, lastElementChild,
//               nextElementSibling, previousElementSibling, querySelector(All),
//               getElementsByTagName, getElementsByClassName, matches, closest,
//               remove, innerHTML (get/set), outerHTML, style
//               (CSSStyleDeclaration), hidden/title/lang, getBoundingClientRect
//   Form/links: input/textarea/select/option value/checked/type/placeholder/
//               disabled/name; a.href (resolved absolute)/target/rel;
//               img.src (resolved)/currentSrc/alt/width/height/
//               naturalWidth/naturalHeight/complete
//   Style:      setProperty/getPropertyValue/removeProperty plus direct
//               accessors for a documented subset of properties
//   Events:     new Event(type, {bubbles, cancelable}) with type/target/
//               currentTarget/bubbles/cancelable/defaultPrevented/eventPhase/
//               timeStamp/isTrusted and preventDefault/stopPropagation/
//               stopImmediatePropagation/composedPath; addEventListener
//               options {capture, once}; full capture -> target -> bubble
//               propagation; dispatchEvent returns false when canceled.
//               Plain {type: ...} objects remain accepted by dispatchEvent as
//               a non-bubbling shortcut.
//
// append/replaceChildren accept node arguments (DOM spec converts string
// arguments to text nodes; that is a documented limitation here).
//
// NOT implemented (documented limitation): property getters beyond the above,
// live NodeList objects (childNodes/querySelectorAll return snapshot arrays),
// event default actions for the browser's built-in behaviors, async script
// loading (async/defer), module scripts, full CSSOM, real element geometry
// (getBoundingClientRect returns a zero rect), and computed styles for
// properties the style engine does not track.
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
  // Constructs a binder with optional browser Web APIs (localStorage/fetch)
  // installed as globals when the corresponding callbacks are provided.
  DomBinder(dom::Document& document, const PageApis& apis);
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

  // Dispatches a synthetic document-level event (e.g. "DOMContentLoaded",
  // "load") to listeners registered on the document — which is where
  // window-level listeners are stored.  RunPageScripts calls this after the
  // page's scripts have run.
  void DispatchDocumentEvent(std::string_view type);

  // Underlying engine (for tests and integration).
  ScriptEngine& engine();
  [[nodiscard]] dom::Document& document() const;

private:
  std::unique_ptr<Impl> impl_;
};

} // namespace neko::javascript
