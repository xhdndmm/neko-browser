// neko::javascript DOM bindings — IntersectionObserver.
//
// Real subset of DOM Standard IntersectionObserver: it computes the actual
// intersection of observed elements with the viewport (or an explicit element
// root) from the browser layer's layout geometry (PageApis::element_geometry)
// and delivers IntersectionObserverEntry objects asynchronously through the
// QuickJS job queue (a task), matching the spec's "callback runs after the
// intersection changes, not inside observe()".
//
// Motivation: https://acxun.github.io uses `new IntersectionObserver(...)` in
// its nav scroll-spy; a missing global killed the entire inline script at the
// first reference (ReferenceError at :37).  Exposing the constructor lets the
// script — and everything after it (search dialog, top button, focus trap) —
// run to completion.
//
// Documented honest limitations:
//   * The root is the viewport (engine-default 800x600) or a single Element.
//   * Scroll offsets are not modelled in the JS layer (getBoundingClientRect
//     is in document coordinates), so intersections are re-evaluated when
//     observe()/unobserve()/disconnect() run and when the observer's own state
//     changes — not on every scroll step.  Scroll-driven updates need scroll
//     modelling, which is future work.
//   * `threshold` is read but treated as 0 (any intersection counts), and the
//     `root`/`rootMargin`/`thresholds` read-only properties are not exposed.
//   * Each entry reports target / isIntersecting / intersectionRatio /
//     boundingClientRect / intersectionRect / rootBounds / time; isVisible is
//     always false.

#include "binding_internal.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <quickjs.h>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace neko::javascript {

JSClassID g_intersection_observer_class_id = 0;
std::mutex g_intersection_observer_class_mutex;
std::unordered_set<JSRuntime*> g_intersection_observer_class_registered;

namespace {

struct Rect
{
  double x0 = 0, y0 = 0, x1 = 0, y1 = 0;

  double width() const
  {
    return x1 - x0;
  }
  double height() const
  {
    return y1 - y0;
  }
};

struct IoTarget
{
  JSValue element = JS_UNDEFINED; // Dup'd element wrapper
  bool reported = false;
  bool last_intersecting = false;
};

struct IoWrapper
{
  Impl* impl = nullptr;
  JSValue callback = JS_UNDEFINED; // Dup'd
  JSValue self = JS_UNDEFINED;     // the observer object (callback `this`/2nd arg)
  JSValue root = JS_UNDEFINED;     // Dup'd root element, or undefined for viewport
  // rootMargin resolved to px (against the root's width/height for '%').
  double root_top = 0, root_right = 0, root_bottom = 0, root_left = 0;
  std::vector<IoTarget> targets;
  std::vector<JSValue> pending_entries; // Dup'd entry objects awaiting delivery
  bool delivery_pending = false;
};

void IoFinalizer(JSRuntime* rt, JSValue obj)
{
  auto* w = static_cast<IoWrapper*>(JS_GetOpaque(obj, g_intersection_observer_class_id));
  if (w == nullptr) {
    return;
  }
  JS_FreeValueRT(rt, w->callback);
  JS_FreeValueRT(rt, w->self);
  JS_FreeValueRT(rt, w->root);
  for (IoTarget& t : w->targets) {
    JS_FreeValueRT(rt, t.element);
  }
  for (JSValue entry : w->pending_entries) {
    JS_FreeValueRT(rt, entry);
  }
  delete w;
}

// The wrapper holds owned JSValues (callback/self/root/targets/entries) that
// the GC cannot see unless gc_mark marks them (same rationale as
// AbortSignalGcMark / EventGcMark — otherwise JS_FreeRuntime trips its gc
// assertion, and observed targets could be collected mid-observation).
void IoGcMark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func)
{
  auto* w = static_cast<IoWrapper*>(JS_GetOpaque(val, g_intersection_observer_class_id));
  if (w == nullptr) {
    return;
  }
  JS_MarkValue(rt, w->callback, mark_func);
  JS_MarkValue(rt, w->self, mark_func);
  JS_MarkValue(rt, w->root, mark_func);
  for (const IoTarget& t : w->targets) {
    JS_MarkValue(rt, t.element, mark_func);
  }
  for (JSValue entry : w->pending_entries) {
    JS_MarkValue(rt, entry, mark_func);
  }
}

void EnsureIntersectionObserverClassRegistered(JSRuntime* rt)
{
  std::lock_guard<std::mutex> lock(g_intersection_observer_class_mutex);
  JS_NewClassID(rt, &g_intersection_observer_class_id);
  if (g_intersection_observer_class_registered.contains(rt)) {
    return;
  }
  JSClassDef def;
  std::memset(&def, 0, sizeof(def));
  def.class_name = "IntersectionObserver";
  def.finalizer = &IoFinalizer;
  def.gc_mark = &IoGcMark;
  JS_NewClass(rt, g_intersection_observer_class_id, &def);
  g_intersection_observer_class_registered.insert(rt);
}

IoWrapper* UnwrapIo(JSValueConst value)
{
  return static_cast<IoWrapper*>(JS_GetOpaque(value, g_intersection_observer_class_id));
}

// The laid-out border-box rect of |el| in document coordinates (which match
// client coordinates here because scroll offsets are not modelled).
std::optional<Rect> RectOf(const Impl* impl, const dom::Element& el)
{
  if (!impl->apis.element_geometry) {
    return std::nullopt;
  }
  const std::optional<ElementGeometry> g = impl->apis.element_geometry(el);
  if (!g.has_value()) {
    return std::nullopt;
  }
  return Rect{g->x, g->y, g->x + g->width, g->y + g->height};
}

std::optional<Rect> Intersect(const Rect& a, const Rect& b)
{
  const double x0 = std::max(a.x0, b.x0);
  const double y0 = std::max(a.y0, b.y0);
  const double x1 = std::min(a.x1, b.x1);
  const double y1 = std::min(a.y1, b.y1);
  if (x1 <= x0 || y1 <= y0) {
    return std::nullopt;
  }
  return Rect{x0, y0, x1, y1};
}

double Area(const Rect& r)
{
  return r.width() * r.height();
}

double NowMsSinceOrigin(const Impl* impl)
{
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                   impl->performance_origin)
      .count();
}

JSValue MakeRectObject(JSContext* ctx, const Rect& r)
{
  JSValue rect = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, rect, "x", JS_NewFloat64(ctx, r.x0));
  JS_SetPropertyStr(ctx, rect, "y", JS_NewFloat64(ctx, r.y0));
  JS_SetPropertyStr(ctx, rect, "left", JS_NewFloat64(ctx, r.x0));
  JS_SetPropertyStr(ctx, rect, "top", JS_NewFloat64(ctx, r.y0));
  JS_SetPropertyStr(ctx, rect, "right", JS_NewFloat64(ctx, r.x1));
  JS_SetPropertyStr(ctx, rect, "bottom", JS_NewFloat64(ctx, r.y1));
  JS_SetPropertyStr(ctx, rect, "width", JS_NewFloat64(ctx, r.width()));
  JS_SetPropertyStr(ctx, rect, "height", JS_NewFloat64(ctx, r.height()));
  return rect;
}

// Parses one rootMargin token ("Npx" | "N%").  Returns the margin in px given
// the reference dimension |ref|; nullopt when the token is not a valid value.
std::optional<double> ParseMarginToken(std::string_view tok, double ref)
{
  char* end = nullptr;
  const double value = std::strtod(tok.data(), &end);
  if (end == tok.data()) {
    return std::nullopt; // no leading number
  }
  const std::string_view unit = tok.substr(static_cast<std::size_t>(end - tok.data()));
  if (unit == "px") {
    return value;
  }
  if (unit == "%") {
    return value * ref / 100.0;
  }
  return std::nullopt;
}

// Splits a rootMargin shorthand string ("top [right [bottom [left]]]") into
// [top, right, bottom, left] px.  Percentages resolve against the root's
// height (top/bottom) or width (left/right).  Malformed/absent -> all-zero.
void ParseRootMargin(IoWrapper* w, const std::string& text, double root_w, double root_h)
{
  std::vector<std::string> tokens;
  std::string cur;
  for (char c : text) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      if (!cur.empty()) {
        tokens.push_back(cur);
        cur.clear();
      }
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) {
    tokens.push_back(cur);
  }
  auto resolve = [&](std::size_t idx) -> double {
    const bool vertical = (idx == 0 || idx == 2); // top/bottom
    return ParseMarginToken(tokens[idx], vertical ? root_h : root_w).value_or(0);
  };
  double top, right, bottom, left;
  switch (tokens.size()) {
  case 0:
    return; // all-zero margins (the default)
  case 1:
    top = right = bottom = left = resolve(0);
    break;
  case 2:
    top = bottom = resolve(0);
    right = left = resolve(1);
    break;
  case 3:
    top = resolve(0);
    right = left = resolve(1);
    bottom = resolve(2);
    break;
  default:
    top = resolve(0);
    right = resolve(1);
    bottom = resolve(2);
    left = resolve(3);
    break;
  }
  w->root_top = top;
  w->root_right = right;
  w->root_bottom = bottom;
  w->root_left = left;
}

// The observer's effective root box (root bounds expanded by rootMargin).
std::optional<Rect> RootRect(IoWrapper* w)
{
  Rect base;
  if (JS_IsUndefined(w->root)) {
    // Viewport root: engine-default 800x600 (matches innerWidth/innerHeight).
    base = Rect{0, 0, 800, 600};
  } else {
    dom::Element* el = AsElement(UnwrapNode(w->root));
    if (el == nullptr) {
      return std::nullopt;
    }
    const std::optional<Rect> r = RectOf(w->impl, *el);
    if (!r.has_value()) {
      return std::nullopt;
    }
    base = *r;
  }
  return Rect{base.x0 - w->root_left,
              base.y0 - w->root_top,
              base.x1 + w->root_right,
              base.y1 + w->root_bottom};
}

JSValue
MakeEntry(JSContext* ctx, IoWrapper* w, const dom::Element* el, JSValue target, const Rect& root)
{
  const std::optional<Rect> el_rect = RectOf(w->impl, *el);
  const Rect elem = el_rect.value_or(Rect{0, 0, 0, 0});
  const std::optional<Rect> inter = el_rect.has_value() ? Intersect(elem, root) : std::nullopt;
  const bool intersecting = inter.has_value();
  const double ratio = inter.has_value() && Area(elem) > 0 ? Area(*inter) / Area(elem) : 0;

  JSValue entry = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, entry, "target", JS_DupValue(ctx, target));
  JS_SetPropertyStr(ctx, entry, "isIntersecting", JS_NewBool(ctx, intersecting));
  JS_SetPropertyStr(ctx, entry, "intersectionRatio", JS_NewFloat64(ctx, ratio));
  JS_SetPropertyStr(
      ctx, entry, "intersectionRect", MakeRectObject(ctx, inter.value_or(Rect{0, 0, 0, 0})));
  JS_SetPropertyStr(ctx, entry, "boundingClientRect", MakeRectObject(ctx, elem));
  JS_SetPropertyStr(ctx, entry, "rootBounds", MakeRectObject(ctx, root));
  JS_SetPropertyStr(ctx, entry, "time", JS_NewFloat64(ctx, NowMsSinceOrigin(w->impl)));
  JS_SetPropertyStr(ctx, entry, "isVisible", JS_NewBool(ctx, false));
  return entry;
}

JSValue IoJobEntry(JSContext* ctx, int argc, JSValueConst* argv);

// Recomputes intersection for every observed target, queues entries for the
// changed ones, and schedules a delivery task when there is work to deliver.
void ComputeAndEnqueue(JSContext* ctx, IoWrapper* w)
{
  if (w->targets.empty()) {
    return;
  }
  const std::optional<Rect> root = RootRect(w);
  bool changed = false;
  std::vector<JSValue> new_entries;
  for (IoTarget& t : w->targets) {
    dom::Element* el = AsElement(UnwrapNode(t.element));
    if (el == nullptr) {
      continue;
    }
    const std::optional<Rect> el_rect = RectOf(w->impl, *el);
    const bool intersecting =
        root.has_value() && el_rect.has_value() && Intersect(*el_rect, *root).has_value();
    if (t.reported && intersecting == t.last_intersecting) {
      continue;
    }
    t.reported = true;
    t.last_intersecting = intersecting;
    // The first report always fires (even for a non-intersecting target),
    // matching the spec's initial observer callback; later reports fire only
    // on change.
    changed = true;
    new_entries.push_back(MakeEntry(ctx, w, el, t.element, root.value_or(Rect{0, 0, 0, 0})));
  }
  for (JSValue e : new_entries) {
    w->pending_entries.push_back(e);
  }
  if (changed && !w->delivery_pending) {
    w->delivery_pending = true;
    JSValue self = JS_DupValue(ctx, w->self);
    JS_EnqueueJob(ctx, IoJobEntry, 1, &self);
    JS_FreeValue(ctx, self);
  }
}

// The QuickJS job entry that delivers queued entry objects to the callback.
JSValue IoJobEntry(JSContext* ctx, int /*argc*/, JSValueConst* argv)
{
  if (argv == nullptr) {
    return JS_UNDEFINED;
  }
  IoWrapper* w = UnwrapIo(argv[0]);
  if (w == nullptr) {
    return JS_UNDEFINED;
  }
  w->delivery_pending = false;
  if (w->pending_entries.empty()) {
    return JS_UNDEFINED;
  }
  JSValue entries = JS_NewArray(ctx);
  for (std::size_t i = 0; i < w->pending_entries.size(); ++i) {
    JS_SetPropertyUint32(ctx, entries, static_cast<uint32_t>(i), w->pending_entries[i]);
  }
  w->pending_entries.clear();
  JSValue args[2] = {entries, JS_DupValue(ctx, w->self)};
  JSValue this_obj = JS_DupValue(ctx, w->self);
  JSValue result = JS_Call(ctx, w->callback, this_obj, 2, args);
  JS_FreeValue(ctx, this_obj);
  JS_FreeValue(ctx, args[1]);
  if (JS_IsException(result)) {
    JS_FreeValue(ctx, entries);
    return JS_EXCEPTION;
  }
  JS_FreeValue(ctx, result);
  JS_FreeValue(ctx, entries);
  return JS_UNDEFINED;
}

JSValue IoObserverObserve(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  IoWrapper* w = UnwrapIo(this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an IntersectionObserver");
  }
  if (argc < 1 || AsElement(UnwrapNode(argv[0])) == nullptr) {
    return JS_ThrowTypeError(ctx, "observe requires an Element");
  }
  for (const IoTarget& t : w->targets) {
    if (JS_IsStrictEqual(ctx, t.element, argv[0])) {
      return JS_UNDEFINED; // already observed
    }
  }
  IoTarget t;
  t.element = JS_DupValue(ctx, argv[0]);
  w->targets.push_back(std::move(t));
  ComputeAndEnqueue(ctx, w);
  return JS_UNDEFINED;
}

JSValue IoObserverUnobserve(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  IoWrapper* w = UnwrapIo(this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an IntersectionObserver");
  }
  if (argc < 1) {
    return JS_UNDEFINED;
  }
  for (auto it = w->targets.begin(); it != w->targets.end(); ++it) {
    if (JS_IsStrictEqual(ctx, it->element, argv[0])) {
      JS_FreeValue(ctx, it->element);
      w->targets.erase(it);
      break;
    }
  }
  return JS_UNDEFINED;
}

JSValue
IoObserverDisconnect(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  IoWrapper* w = UnwrapIo(this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an IntersectionObserver");
  }
  for (IoTarget& t : w->targets) {
    JS_FreeValue(ctx, t.element);
  }
  w->targets.clear();
  for (JSValue e : w->pending_entries) {
    JS_FreeValue(ctx, e);
  }
  w->pending_entries.clear();
  return JS_UNDEFINED;
}

JSValue
IoObserverTakeRecords(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  IoWrapper* w = UnwrapIo(this_val);
  if (w == nullptr) {
    return JS_ThrowTypeError(ctx, "not an IntersectionObserver");
  }
  JSValue records = JS_NewArray(ctx);
  for (std::size_t i = 0; i < w->pending_entries.size(); ++i) {
    JS_SetPropertyUint32(ctx, records, static_cast<uint32_t>(i), w->pending_entries[i]);
  }
  w->pending_entries.clear();
  return records;
}

JSValue IntersectionObserverConstructor(JSContext* ctx,
                                        JSValueConst /*new_target*/,
                                        int argc,
                                        JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, JS_UNDEFINED);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
    return JS_ThrowTypeError(ctx, "callback must be a function");
  }
  auto* w = new IoWrapper();
  w->impl = impl;
  w->callback = JS_DupValue(ctx, argv[0]);
  if (argc >= 2 && JS_IsObject(argv[1])) {
    JSValue root = JS_GetPropertyStr(ctx, argv[1], "root");
    if (JS_IsNull(root) || JS_IsUndefined(root)) {
      w->root = JS_UNDEFINED; // viewport
    } else {
      w->root = JS_DupValue(ctx, root);
    }
    JS_FreeValue(ctx, root);
    JSValue rm = JS_GetPropertyStr(ctx, argv[1], "rootMargin");
    if (JS_IsString(rm)) {
      bool ok = false;
      ParseRootMargin(w, ArgString(ctx, rm, &ok), 800, 600);
    }
    JS_FreeValue(ctx, rm);
    // `threshold` is accepted but treated as 0 (any intersection counts).
  }
  JSValue obj = JS_NewObjectProtoClass(
      ctx, impl->intersection_observer_proto, g_intersection_observer_class_id);
  if (JS_IsException(obj)) {
    IoFinalizer(JS_GetRuntime(ctx), obj); // no opaque set; frees nothing but is safe
    delete w;
    return obj;
  }
  JS_SetOpaque(obj, w);
  w->self = JS_DupValue(ctx, obj);
  return obj;
}

} // namespace

void InstallIntersectionObserverGlobal(JSContext* ctx, JSValue global, Impl& impl)
{
  EnsureIntersectionObserverClassRegistered(JS_GetRuntime(ctx));

  impl.intersection_observer_proto = JS_NewObject(ctx);
  static const std::array<JSCFunctionListEntry, 4> kIoMethods = {{
      JS_CFUNC_DEF("observe", 1, IoObserverObserve),
      JS_CFUNC_DEF("unobserve", 1, IoObserverUnobserve),
      JS_CFUNC_DEF("disconnect", 0, IoObserverDisconnect),
      JS_CFUNC_DEF("takeRecords", 0, IoObserverTakeRecords),
  }};
  JS_SetPropertyFunctionList(ctx,
                             impl.intersection_observer_proto,
                             kIoMethods.data(),
                             static_cast<int>(kIoMethods.size()));

  JSValue ctor = JS_NewCFunction2(
      ctx, IntersectionObserverConstructor, "IntersectionObserver", 1, JS_CFUNC_constructor, 0);
  JS_SetPropertyStr(ctx, ctor, "prototype", JS_DupValue(ctx, impl.intersection_observer_proto));
  JS_SetPropertyStr(ctx, impl.intersection_observer_proto, "constructor", JS_DupValue(ctx, ctor));
  JS_SetPropertyStr(ctx, global, "IntersectionObserver", ctor); // steals
}

void ForgetIntersectionObserverRuntime(JSRuntime* rt)
{
  std::lock_guard<std::mutex> lock(g_intersection_observer_class_mutex);
  g_intersection_observer_class_registered.erase(rt);
}

} // namespace neko::javascript
