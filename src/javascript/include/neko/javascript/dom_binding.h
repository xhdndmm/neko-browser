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

// Laid-out geometry of an element (border box origin/size in document
// coordinates, css px) plus its padding-box size and border widths.  Computed
// from the layout tree by the browser layer.
struct ElementGeometry
{
  double x = 0;
  double y = 0;
  double width = 0;
  double height = 0;
  double client_width = 0;
  double client_height = 0;
  double border_top = 0;
  double border_left = 0;
};

// Metadata of one IndexedDB object store (name + key configuration).
struct IdbStoreMeta
{
  std::string name;
  std::string key_path; // empty = out-of-line keys
  bool auto_increment = false;
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

  // Element layout geometry (getBoundingClientRect / offsetWidth / offsetTop
  // etc).  Wired by the browser layer from the renderer's layout tree; when
  // absent (or for elements with no laid-out box) the geometry getters report
  // 0 / the zero rect.
  std::function<std::optional<ElementGeometry>(const dom::Element&)> element_geometry;

  // HTMLMediaElement (video) controls.  Wired by the browser layer from the
  // renderer's video playback state; when absent, the media properties
  // report defaults (NaN duration, paused = true).
  std::function<void(const dom::Element&)> video_play;
  std::function<void(const dom::Element&)> video_pause;
  std::function<void(const dom::Element&, double)> video_seek;
  std::function<std::optional<double>(const dom::Element&)> video_duration;
  std::function<std::optional<double>(const dom::Element&)> video_current_time;
  std::function<bool(const dom::Element&)> video_paused;

  // window.indexedDB (per-origin; the caller scopes everything by origin).
  // Records travel as JSON text (the structured-clone subset); keys are JSON
  // numbers or strings.  Errors carry an "IDB:<ExceptionName>:" prefix.
  std::function<base::Result<int64_t>(std::string_view db)> idb_current_version;
  std::function<base::Result<int64_t>(std::string_view db)> idb_create_db;
  std::function<base::Result<void>(std::string_view db, int64_t version)> idb_set_version;
  std::function<base::Result<void>(std::string_view db)> idb_delete_db;
  std::function<base::Result<std::vector<IdbStoreMeta>>(std::string_view db)> idb_store_names;
  std::function<base::Result<void>(std::string_view db, std::string_view store,
                                   std::string_view key_path, bool auto_increment)>
      idb_create_store;
  std::function<base::Result<void>(std::string_view db, std::string_view store)>
      idb_delete_store;
  std::function<base::Result<std::string>(std::string_view db, std::string_view store,
                                          std::optional<std::string> key, std::string value)>
      idb_add;
  std::function<base::Result<std::string>(std::string_view db, std::string_view store,
                                          std::optional<std::string> key, std::string value)>
      idb_put;
  std::function<base::Result<std::optional<std::string>>(std::string_view db,
                                                         std::string_view store,
                                                         std::string key)>
      idb_get;
  std::function<base::Result<void>(std::string_view db, std::string_view store, std::string key)>
      idb_delete;
  std::function<base::Result<void>(std::string_view db, std::string_view store)> idb_clear;
  std::function<base::Result<int64_t>(std::string_view db, std::string_view store)> idb_count;
  std::function<base::Result<std::vector<std::string>>(std::string_view db,
                                                       std::string_view store)>
      idb_get_all;
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
//               performance.now(), requestAnimationFrame, indexedDB
//               (versioned databases, object stores with key path /
//               auto-increment, add/put/get/delete/clear/count/getAll via
//               transactions and IDBRequest-style objects with onsuccess/
//               onerror handlers fired from microtasks; no cursors/indexes)
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
//               (CSSStyleDeclaration), hidden/title/lang, layout geometry
//               (getBoundingClientRect / offsetWidth/Height/Left/Top /
//               offsetParent / clientWidth/Height/Top/Left, backed by the
//               browser layer's layout tree via PageApis::element_geometry)
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
//               a non-bubbling shortcut.  Browser-dispatched pointer events
//               (mousedown/mouseup/click) carry MouseEvent fields
//               (clientX/clientY/button); keyboard events expose the legacy
//               keyCode; wheel events carry deltaY; focus/blur and input
//               events are dispatched by the browser layer; element global
//               event handler attributes work both as IDL assignments
//               (element.onclick = fn) and content attributes (on*="code").
//
// append/replaceChildren accept node arguments (DOM spec converts string
// arguments to text nodes; that is a documented limitation here).
//
// NOT implemented (documented limitation): property getters beyond the above,
// live NodeList objects (childNodes/querySelectorAll return snapshot arrays),
// event default actions for the browser's built-in behaviors, async script
// loading (async/defer), module scripts, full CSSOM, scroll-aware element
// geometry (getBoundingClientRect is in document coordinates — scroll offsets
// and scrollWidth/scrollHeight/scrollTop are not yet modelled; offsetParent
// is always <body>), and computed styles for properties the style engine does
// not track.
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

  // Dispatches a user-interaction event (e.g. "click", "submit", "keydown")
  // to |element| with full capture -> target -> bubble propagation and
  // cancelable=true.  Returns true when the event was NOT canceled (no
  // listener called preventDefault), so the caller knows to run the event's
  // default action (link navigation, form submission).
  bool DispatchCancelableEvent(dom::Element& element, std::string_view type);

  // Dispatches a cancelable keyboard event (keydown/keyup) to |element| with
  // the UI Events key/code strings.  Returns true when NOT canceled.
  bool DispatchKeyboardEvent(dom::Element& element, std::string_view type, std::string_view key,
                             std::string_view code);

  // Dispatches a cancelable pointer event (mousedown/mouseup/click) to
  // |element| with client coordinates and the mouse button.  Returns true
  // when NOT canceled.
  bool DispatchMouseEvent(dom::Element& element, std::string_view type, double client_x,
                          double client_y, int button);

  // Dispatches a cancelable wheel event to |element| with the vertical scroll
  // delta (px).  Returns true when NOT canceled.
  bool DispatchWheelEvent(dom::Element& element, std::string_view type, double delta_y);

  // Dispatches a non-bubbling focus/blur event to |element| (fires listeners
  // and the element's onfocus/onblur handler).
  void DispatchFocusEvent(dom::Element& element, std::string_view type);

  // Dispatches a bubbling "input" event to |element| after a text control's
  // value changed (fires listeners and the element's oninput handler).
  void DispatchInputEvent(dom::Element& element);

  // Returns whether any JS DOM mutation has run since the last time this was
  // called (or since the binder was created), clearing the flag.  The browser
  // layer uses it after dispatching user-interaction events to decide whether
  // to re-run the style cascade/layout so event-handler DOM changes appear.
  bool TakeDomDirty();

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
