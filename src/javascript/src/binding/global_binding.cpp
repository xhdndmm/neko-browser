// neko::javascript DOM bindings — window/global-object family.
//
// Part of the dom_binding split (see binding/binding_internal.h): timers
// (setTimeout/setInterval), window-level event listener entry points,
// requestAnimationFrame, location, history, performance, screen/navigator
// helpers, matchMedia and getComputedStyle.

#include "binding_internal.h"

#include <array>
#include <quickjs.h>
#include <string>

namespace neko::javascript {

// The engine's reported platform string (navigator.platform), matching what
// mainstream browsers report per OS.
const char* NavigatorPlatform()
{
#if defined(_WIN32)
  return "Win32";
#elif defined(__APPLE__)
  return "MacIntel";
#else
  return "Linux";
#endif
}

JSValue TimerCreate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
    return JS_ThrowTypeError(ctx, "the first argument must be a function");
  }
  int64_t ms = 0;
  if (argc >= 2) {
    if (JS_ToInt64(ctx, &ms, argv[1]) != 0) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      return JS_EXCEPTION;
    }
  }
  if (ms < 0) {
    ms = 0;
  }
  Impl::Timer timer;
  timer.id = impl->next_timer_id++;
  timer.callback = JS_DupValue(ctx, argv[0]);
  timer.due = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  timer.interval = std::chrono::milliseconds(ms);
  timer.repeating = magic == 1;
  const int64_t timer_id = timer.id;
  impl->timers.push_back(timer); // Timer is trivially copyable; the copy owns the callback
  return JS_NewInt64(ctx, timer_id);
}

JSValue
TimerClear(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int /*magic*/)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr) {
    return JS_UNDEFINED;
  }
  int64_t id = 0;
  if (argc >= 1) {
    if (JS_ToInt64(ctx, &id, argv[0]) != 0) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      return JS_UNDEFINED;
    }
  }
  for (auto it = impl->timers.begin(); it != impl->timers.end(); ++it) {
    if (it->id == id) {
      JS_FreeValue(ctx, it->callback);
      impl->timers.erase(it);
      break;
    }
  }
  return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Window-level event functions.
//
// The window is a plain object (not a node), but in this minimal model the
// window and the document share one event target set: window-level listeners
// are stored under the document node.  So window.addEventListener(...) (and
// the bare global addEventListener(...) aliases) forward to the document.
// ---------------------------------------------------------------------------

JSValue WindowAddEventListener(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  JSValue doc_wrap = impl->WrapNode(&impl->document);
  JSValue result = NodeAddEventListener(ctx, doc_wrap, argc, argv);
  JS_FreeValue(ctx, doc_wrap);
  return result;
}

JSValue
WindowRemoveEventListener(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  JSValue doc_wrap = impl->WrapNode(&impl->document);
  JSValue result = NodeRemoveEventListener(ctx, doc_wrap, argc, argv);
  JS_FreeValue(ctx, doc_wrap);
  return result;
}

JSValue WindowDispatchEvent(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  JSValue doc_wrap = impl->WrapNode(&impl->document);
  JSValue result = NodeDispatchEvent(ctx, doc_wrap, argc, argv);
  JS_FreeValue(ctx, doc_wrap);
  return result;
}

// ---------------------------------------------------------------------------
// Event objects.
//
// `new Event(type, {bubbles, cancelable})` creates a real Event whose
// prototype exposes type/target/currentTarget/bubbles/cancelable/
// defaultPrevented/eventPhase/timeStamp and the standard methods.  Events
// created internally by a dispatch (including plain {type: ...} shortcuts)
// carry the same state.  The mutable dispatch state lives in the opaque
// EventWrapper; target/currentTarget are regular properties on the object.
// ---------------------------------------------------------------------------

// ---- location ---------------------------------------------------------------

// Splits an absolute URL string into its Location components using simple
// string parsing so the javascript layer stays decoupled from the url module.
struct LocationParts
{
  std::string protocol; // "https:"
  std::string host;     // "www.example.com:8080"
  std::string hostname; // "www.example.com"
  std::string port;     // "8080" (empty when the URL has no port)
  std::string pathname; // "/a/b" (empty when the URL has no path)
  std::string search;   // "?x=1" (empty when absent)
  std::string hash;     // "#frag" (empty when absent)
  std::string origin;   // "https://www.example.com:8080"
};

LocationParts ParseLocationParts(std::string_view href)
{
  LocationParts out;
  std::size_t path_start = 0;
  const std::size_t scheme_end = href.find("://");
  if (scheme_end == std::string_view::npos) {
    // Bare path (no scheme, e.g. a local form submission target): the whole
    // string is the path plus an optional query/fragment.
    const std::size_t colon = href.find(':');
    if (colon != std::string_view::npos) {
      out.protocol = std::string(href.substr(0, colon + 1));
    }
  } else {
    out.protocol = std::string(href.substr(0, scheme_end + 1));
    std::size_t i = scheme_end + 3;
    const std::size_t ps = href.find_first_of("/?#", i);
    const std::size_t host_end = ps == std::string_view::npos ? href.size() : ps;
    out.host = std::string(href.substr(i, host_end - i));
    out.origin = out.protocol + "//" + out.host;
    const std::size_t colon = out.host.rfind(':');
    if (colon != std::string::npos) {
      out.hostname = out.host.substr(0, colon);
      out.port = out.host.substr(colon + 1);
    } else {
      out.hostname = out.host;
    }
    path_start = ps == std::string_view::npos ? href.size() : ps;
  }

  // Path / search / hash extraction shared by both cases.
  const std::size_t q = href.find('?', path_start);
  const std::size_t h = href.find('#', path_start);
  const std::size_t path_end = std::min(q == std::string_view::npos ? href.size() : q,
                                        h == std::string_view::npos ? href.size() : h);
  if (path_start < href.size()) {
    out.pathname = std::string(href.substr(path_start, path_end - path_start));
  }
  if (q != std::string_view::npos) {
    const std::size_t search_end = h == std::string_view::npos ? href.size() : h;
    out.search = std::string(href.substr(q, search_end - q));
  }
  if (h != std::string_view::npos) {
    out.hash = std::string(href.substr(h));
  }
  return out;
}

// Resolves a (possibly relative) target against the page base, falling back to
// the raw string when resolution is unavailable or fails.
std::string ResolveLocationTarget(Impl* impl, std::string_view target)
{
  if (impl->apis.resolve_url) {
    const std::string resolved = impl->apis.resolve_url(std::string(target));
    if (!resolved.empty()) {
      return resolved;
    }
  }
  return std::string(target);
}

JSValue LocationHrefGetter(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.location_href) {
    return JS_NewStringLen(ctx, "", 0);
  }
  const std::string href = impl->apis.location_href();
  return JS_NewStringLen(ctx, href.data(), href.size());
}

JSValue LocationHrefSetter(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.navigate) {
    return JS_UNDEFINED;
  }
  bool ok = false;
  const std::string target = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  impl->apis.navigate(ResolveLocationTarget(impl, target));
  return JS_UNDEFINED;
}

// Read-only Location parts, dispatched by magic (see kLocationMagic below).
JSValue LocationPropGetter(JSContext* ctx, JSValueConst this_val, int magic)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.location_href) {
    return JS_NewStringLen(ctx, "", 0);
  }
  const LocationParts parts = ParseLocationParts(impl->apis.location_href());
  const std::string* value = nullptr;
  switch (magic) {
  case 0:
    value = &parts.protocol;
    break;
  case 1:
    value = &parts.host;
    break;
  case 2:
    value = &parts.hostname;
    break;
  case 3:
    value = &parts.port;
    break;
  case 4:
    value = &parts.pathname;
    break;
  case 5:
    value = &parts.search;
    break;
  case 6:
    value = &parts.hash;
    break;
  case 7:
    value = &parts.origin;
    break;
  default:
    return JS_NewStringLen(ctx, "", 0);
  }
  return JS_NewStringLen(ctx, value->data(), value->size());
}

JSValue LocationAssign(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.navigate) {
    return JS_UNDEFINED;
  }
  bool ok = false;
  const std::string target = ArgString(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  impl->apis.navigate(ResolveLocationTarget(impl, target));
  return JS_UNDEFINED;
}

JSValue LocationReplace(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.navigate) {
    return JS_UNDEFINED;
  }
  bool ok = false;
  const std::string target = ArgString(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  // Replace is a documented approximation: the synchronous engine has no
  // back/forward entry for the navigating page to swap, so it behaves like
  // assign().
  impl->apis.navigate(ResolveLocationTarget(impl, target));
  return JS_UNDEFINED;
}

JSValue LocationReload(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.reload) {
    return JS_UNDEFINED;
  }
  impl->apis.reload();
  return JS_UNDEFINED;
}

JSValue
LocationToString(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  return LocationHrefGetter(ctx, this_val);
}

JSValue WindowScrollTo(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  if (ImplFor(ctx, this_val) == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  // The page viewport is managed by the GUI scroll area; script-initiated
  // window scrolling is a no-op (documented).
  return JS_UNDEFINED;
}

JSValue WindowScrollBy(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  if (ImplFor(ctx, this_val) == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  return JS_UNDEFINED;
}

JSValue HistoryBack(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  if (ImplFor(ctx, this_val) == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  // Script-driven history traversal is not wired to the browser navigation
  // stack; no-op (documented).
  return JS_UNDEFINED;
}

JSValue HistoryForward(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  if (ImplFor(ctx, this_val) == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  return JS_UNDEFINED;
}

JSValue HistoryGo(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  if (ImplFor(ctx, this_val) == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  return JS_UNDEFINED;
}

JSValue
HistoryPushState(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  if (ImplFor(ctx, this_val) == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  // pushState/replaceState are accepted but do not change the URL (the
  // navigation stack is not exposed to scripts); documented.
  return JS_UNDEFINED;
}

JSValue
HistoryReplaceState(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  if (ImplFor(ctx, this_val) == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  return JS_UNDEFINED;
}

JSValue HistoryGetLength(JSContext* ctx, JSValueConst this_val)
{
  if (ImplFor(ctx, this_val) == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  // The script-visible session history has exactly one entry (the current
  // document); the browser back/forward stack is separate.
  return JS_NewInt32(ctx, 1);
}

JSValue PerformanceNow(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  const auto elapsed = std::chrono::steady_clock::now() - impl->performance_origin;
  return JS_NewFloat64(ctx, std::chrono::duration<double, std::milli>(elapsed).count());
}

// ---------------------------------------------------------------------------
// window.matchMedia(query).
//
// Returns a MediaQueryList evaluated against the engine's fixed viewport
// (800x600@1x, see screen/innerWidth).  Supports the media-feature queries
// real sites use at bootstrap: (min|max)-(width|height): Npx, orientation,
// prefers-color-scheme, prefers-reduced-motion, and the (any-)pointer/hover
// features; a comma-separated list matches when any alternative does.  Unknown
// features resolve to false (conservative).  The list object is static (no

namespace {

bool MatchMediaQueryImpl(const std::string& query)
{
  // (min|max)-(width|height): <N>px  /  (orientation: portrait|landscape)  /
  // (prefers-color-scheme: light|dark)  /  (prefers-reduced-motion: ...)  /
  // (pointer|hover|any-pointer|any-hover: none|coarse|fine|hover).
  auto num_feature = [](const std::string& q, const char* name) -> std::optional<double> {
    const std::string prefix = name;
    const auto pos = q.find(prefix);
    if (pos == std::string::npos) {
      return std::nullopt;
    }
    const auto colon = q.find(':', pos);
    if (colon == std::string::npos) {
      return std::nullopt;
    }
    const auto paren = q.find(')', colon);
    const auto end = paren == std::string::npos ? q.size() : paren;
    std::string val = q.substr(colon + 1, end - colon - 1);
    val.erase(std::remove_if(
                  val.begin(), val.end(), [](unsigned char c) { return std::isspace(c) != 0; }),
              val.end());
    if (val.size() < 3 || val.compare(val.size() - 2, 2, "px") != 0) {
      return std::nullopt;
    }
    try {
      return std::stod(val.substr(0, val.size() - 2));
    } catch (...) {
      return std::nullopt;
    }
  };
  const bool landscape = 800.0 > 600.0;
  bool negate = false;
  std::string q = query;
  // Trim, then honor a leading "not "/"only ".
  const auto first = q.find_first_not_of(" \t");
  q = first == std::string::npos ? "" : q.substr(first);
  if (q.rfind("not ", 0) == 0) {
    negate = true;
    q = q.substr(4);
  } else if (q.rfind("only ", 0) == 0) {
    q = q.substr(5);
  }
  // A bare media type (all/screen/print) matches (print does not).
  if (q.find('(') == std::string::npos) {
    const bool type_match = q == "all" || q == "screen";
    return negate ? !type_match : type_match;
  }
  bool matched = true;
  for (std::size_t i = 0; i < q.size();) {
    const auto open = q.find('(', i);
    if (open == std::string::npos) {
      break;
    }
    const auto close = q.find(')', open);
    if (close == std::string::npos) {
      break;
    }
    const std::string expr = q.substr(open + 1, close - open - 1);
    if (auto w_min = num_feature(expr, "min-width")) {
      matched = matched && 800.0 >= *w_min;
    } else if (auto w_max = num_feature(expr, "max-width")) {
      matched = matched && 800.0 <= *w_max;
    } else if (auto h_min = num_feature(expr, "min-height")) {
      matched = matched && 600.0 >= *h_min;
    } else if (auto h_max = num_feature(expr, "max-height")) {
      matched = matched && 600.0 <= *h_max;
    } else if (expr.rfind("orientation:", 0) == 0) {
      const bool p = expr.find("portrait") != std::string::npos;
      matched = matched && (p ? !landscape : landscape);
    } else if (expr.rfind("prefers-color-scheme:", 0) == 0) {
      matched = matched && expr.find("light") != std::string::npos;
    } else if (expr.rfind("prefers-reduced-motion:", 0) == 0) {
      matched = matched && expr.find("no-preference") != std::string::npos;
    } else if (expr.rfind("any-hover:", 0) == 0) {
      matched = matched && expr.find("hover") != std::string::npos;
    } else if (expr.rfind("hover:", 0) == 0) {
      matched = matched && expr.find("hover") != std::string::npos;
    } else if (expr.rfind("any-pointer:", 0) == 0 || expr.rfind("pointer:", 0) == 0) {
      matched = matched && expr.find("fine") != std::string::npos;
    } else {
      // Unknown feature: conservative no-match for this expression.
      matched = false;
    }
    i = close + 1;
  }
  return negate ? !matched : matched;
}

JSValue
MatchMediaNoOp(JSContext* /*ctx*/, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/)
{
  return JS_UNDEFINED;
}

} // namespace

// window.matchMedia(query) -> MediaQueryList (static; see above).
JSValue WindowMatchMedia(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  bool ok = false;
  const std::string query = argc >= 1 ? ArgString(ctx, argv[0], &ok) : std::string();
  if (!ok) {
    return JS_EXCEPTION;
  }
  // A comma-separated list matches when any alternative does.
  bool matches = false;
  std::size_t start = 0;
  while (start <= query.size()) {
    const auto comma = query.find(',', start);
    const auto part =
        query.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    if (MatchMediaQueryImpl(part)) {
      matches = true;
      break;
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }

  JSValue list = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, list, "matches", JS_NewBool(ctx, matches));
  JS_SetPropertyStr(ctx, list, "media", JS_NewStringLen(ctx, query.data(), query.size()));
  JS_SetPropertyStr(ctx, list, "onchange", JS_NULL);
  static const std::array<JSCFunctionListEntry, 4> kNoOps = {{
      JS_CFUNC_DEF("addEventListener", 0, MatchMediaNoOp),
      JS_CFUNC_DEF("removeEventListener", 0, MatchMediaNoOp),
      JS_CFUNC_DEF("addListener", 0, MatchMediaNoOp),
      JS_CFUNC_DEF("removeListener", 0, MatchMediaNoOp),
  }};
  JS_SetPropertyFunctionList(ctx, list, kNoOps.data(), static_cast<int>(kNoOps.size()));
  return list;
}

// ---------------------------------------------------------------------------
// window.getComputedStyle(element).
//
// Returns an object with getPropertyValue(name) plus camelCase accessors for
// every property the style engine reports.  The computed values come from the
// browser layer's PageApis::computed_style callback (serialized px strings);
// without it, an empty object is returned.
// ---------------------------------------------------------------------------

JSValue
ComputedStyleGetPropertyValue(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  JSValue map = JS_GetPropertyStr(ctx, this_val, "__props__");
  if (JS_IsUndefined(map)) {
    return JS_NewString(ctx, "");
  }
  if (argc < 1) {
    JS_FreeValue(ctx, map);
    return JS_NewString(ctx, "");
  }
  bool ok = false;
  const std::string prop = ToLower(ArgString(ctx, argv[0], &ok));
  if (!ok) {
    JS_FreeValue(ctx, map);
    return JS_EXCEPTION;
  }
  JSValue v = JS_GetPropertyStr(ctx, map, prop.c_str());
  JS_FreeValue(ctx, map);
  if (JS_IsUndefined(v)) {
    return JS_NewString(ctx, "");
  }
  return v;
}

// Direct camelCase accessor: func_data[0] is the kebab-case property name.
JSValue ComputedStyleGetByName(JSContext* ctx,
                               JSValueConst this_val,
                               int /*argc*/,
                               JSValueConst* /*argv*/,
                               int /*magic*/,
                               JSValueConst* func_data)
{
  JSValue map = JS_GetPropertyStr(ctx, this_val, "__props__");
  if (JS_IsUndefined(map)) {
    return JS_NewString(ctx, "");
  }
  const char* name = JS_ToCString(ctx, func_data[0]);
  JSValue v = name != nullptr ? JS_GetPropertyStr(ctx, map, name) : JS_UNDEFINED;
  if (name != nullptr) {
    JS_FreeCString(ctx, name);
  }
  JS_FreeValue(ctx, map);
  if (JS_IsUndefined(v)) {
    return JS_NewString(ctx, "");
  }
  return v;
}

// Converts "background-color" to "backgroundColor".
std::string PropToCamel(std::string_view prop)
{
  std::string camel;
  bool upper = false;
  for (const char c : prop) {
    if (c == '-') {
      upper = true;
      continue;
    }
    camel.push_back(upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c);
    upper = false;
  }
  return camel;
}

JSValue WindowGetComputedStyle(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = argc >= 1 ? AsElement(UnwrapNode(argv[0])) : nullptr;
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "getComputedStyle requires an element");
  }
  std::map<std::string, std::string> props;
  if (impl->apis.computed_style) {
    props = impl->apis.computed_style(*element);
  }
  // The property map (kebab-case keys) is stored as a hidden property so
  // both getPropertyValue() and the camelCase accessors can read it.
  JSValue map = JS_NewObject(ctx);
  for (const auto& [name, value] : props) {
    JS_SetPropertyStr(ctx, map, name.c_str(), JS_NewString(ctx, value.c_str()));
  }
  JSValue style = JS_NewObject(ctx);
  JS_DefinePropertyValueStr(ctx, style, "__props__", JS_DupValue(ctx, map), JS_PROP_C_W_E);
  JSValue get_prop = JS_NewCFunction(ctx, ComputedStyleGetPropertyValue, "getPropertyValue", 1);
  JS_SetPropertyStr(ctx, style, "getPropertyValue", get_prop); // steals
  for (const auto& [name, value] : props) {
    (void)value;
    const std::string camel = PropToCamel(name);
    // The getter closure captures the kebab-case property name.
    JSValue name_data = JS_NewString(ctx, name.c_str());
    JSValue getter = JS_NewCFunctionData(ctx, ComputedStyleGetByName, 0, 0, 1, &name_data);
    // DefineGetter (JS_DefinePropertyGetSet) steals the getter reference.
    DefineGetter(ctx, style, camel.c_str(), getter);
    JS_FreeValue(ctx, name_data);
  }
  JS_FreeValue(ctx, map);
  return style;
}

} // namespace neko::javascript
