// neko::javascript DOM bindings — Element family.
//
// Part of the dom_binding split (see binding/binding_internal.h): the Element
// prototype callbacks (attributes, traversal, query, innerHTML/outerHTML,
// classList, dataset), the dataset exotic-object machinery, and
// DefineElementPrototype / DefineClassListPrototype.

#include "neko/dom/element.h"
#include "neko/dom/node.h"
#include "neko/dom/query.h"
#include "neko/html/input_value.h"
#include "neko/html/parser.h"

#include "binding_internal.h"

#include <algorithm>
#include <cstring>
#include <quickjs.h>
#include <string>
#include <string_view>
#include <vector>

namespace neko::javascript {

// ---------------------------------------------------------------------------
// Dataset objects (element.dataset): a class whose exotic get/set property
// handlers map camelCase keys to the element's data-* attributes, so both
// reads (el.dataset.foo) and writes (el.dataset.fooBar = 'x') round-trip
// through the attribute list without needing per-property accessors.
// ---------------------------------------------------------------------------
JSClassID g_dataset_class_id = 0;
std::mutex g_dataset_class_mutex;
std::unordered_set<JSRuntime*> g_dataset_class_registered;
static JSClassExoticMethods g_dataset_exotic = {};

void DatasetFinalizer(JSRuntime* /*rt*/, JSValue obj)
{
  auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(obj, g_dataset_class_id));
  delete w;
}

// "fooBar" -> "data-foo-bar".
std::string DatasetKeyToAttr(const std::string& key)
{
  std::string attr = "data-";
  for (const char c : key) {
    if (c >= 'A' && c <= 'Z') {
      attr.push_back('-');
      attr.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    } else {
      attr.push_back(c);
    }
  }
  return attr;
}

// "data-foo-bar" -> "fooBar".
std::string DatasetAttrToKey(const std::string& attr)
{
  std::string key;
  bool upper = false;
  for (std::size_t i = 5; i < attr.size(); ++i) { // skip "data-"
    const char c = attr[i];
    if (c == '-') {
      upper = true;
      continue;
    }
    key.push_back(upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c);
    upper = false;
  }
  return key;
}

JSValue DatasetGetProperty(JSContext* ctx, JSValueConst obj, JSAtom atom, JSValueConst /*receiver*/)
{
  auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(obj, g_dataset_class_id));
  dom::Element* el = w != nullptr ? AsElement(w->node) : nullptr;
  if (el == nullptr) {
    return JS_UNDEFINED;
  }
  const char* name = JS_AtomToCString(ctx, atom);
  if (name == nullptr) {
    return JS_UNDEFINED;
  }
  const std::string attr = DatasetKeyToAttr(name);
  JS_FreeCString(ctx, name);
  const std::optional<std::string_view> value = el->GetAttribute(attr);
  return value.has_value() ? JS_NewStringLen(ctx, value->data(), value->size()) : JS_UNDEFINED;
}

int DatasetSetProperty(JSContext* ctx,
                       JSValueConst obj,
                       JSAtom atom,
                       JSValueConst value,
                       JSValueConst /*receiver*/,
                       int /*flags*/)
{
  auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(obj, g_dataset_class_id));
  dom::Element* el = w != nullptr ? AsElement(w->node) : nullptr;
  if (el == nullptr) {
    return 0;
  }
  const char* name = JS_AtomToCString(ctx, atom);
  if (name == nullptr) {
    return 0;
  }
  const std::string attr = DatasetKeyToAttr(name);
  JS_FreeCString(ctx, name);
  bool ok = false;
  const std::string value_str = ArgString(ctx, value, &ok);
  if (!ok) {
    return 0; // exception pending
  }
  if (value_str.empty()) {
    el->RemoveAttribute(attr);
  } else {
    el->SetAttribute(attr, value_str);
  }
  // data-* attributes are styled through [data-x] selectors on real sites:
  // the mutation must mark the DOM dirty like setAttribute does.  They are
  // style-only, so the batch stays on the scoped-restyle path.
  if (w->impl != nullptr) {
    w->impl->MarkAttrDirty(el, attr);
  }
  return 1;
}

// Returns the element's data-* attributes as an object keyed by camelCase
// (used by get_own_property_names so Object.keys / JSON see them).
int DatasetGetOwnPropertyNames(JSContext* ctx,
                               JSPropertyEnum** ptab,
                               uint32_t* plen,
                               JSValueConst obj)
{
  auto* w = static_cast<NodeWrapper*>(JS_GetOpaque(obj, g_dataset_class_id));
  dom::Element* el = w != nullptr ? AsElement(w->node) : nullptr;
  if (el == nullptr) {
    *ptab = nullptr;
    *plen = 0;
    return 0;
  }
  std::vector<std::string> keys;
  for (const dom::Attribute& attr : el->attributes()) {
    if (attr.name.rfind("data-", 0) == 0) {
      keys.push_back(DatasetAttrToKey(attr.name));
    }
  }
  JSPropertyEnum* table =
      static_cast<JSPropertyEnum*>(js_malloc(ctx, keys.size() * sizeof(JSPropertyEnum)));
  if (keys.empty()) {
    *ptab = table;
    *plen = 0;
    return 0;
  }
  if (table == nullptr) {
    return -1;
  }
  for (std::size_t i = 0; i < keys.size(); ++i) {
    table[i].atom = JS_NewAtomLen(ctx, keys[i].data(), keys[i].size());
    table[i].is_enumerable = 1;
  }
  *ptab = table;
  *plen = static_cast<uint32_t>(keys.size());
  return 0;
}

// Reads the 'type' of an event-like value: the opaque wrapper's type for a
// real Event, else the JS 'type' property (for plain {type: ...} objects,

JSValue ElementGetTagName(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string name = NodeNameOf(*element);
  return JS_NewStringLen(ctx, name.data(), name.size());
}

JSValue ElementGetNamespaceURI(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string_view namespace_uri = element->namespace_uri();
  return JS_NewStringLen(ctx, namespace_uri.data(), namespace_uri.size());
}

JSValue ElementGetId(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::optional<std::string_view> id = element->Id();
  if (id.has_value()) {
    return JS_NewStringLen(ctx, id->data(), id->size());
  }
  return JS_NewStringLen(ctx, "", 0);
}

JSValue ElementSetId(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string id = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  if (id.empty()) {
    element->RemoveAttribute("id");
  } else {
    element->SetAttribute("id", id);
  }
  ImplFor(ctx, this_val)->MarkDomDirty(element);
  return JS_UNDEFINED;
}

JSValue ElementGetClassName(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::optional<std::string_view> cls = element->GetAttribute("class");
  if (cls.has_value()) {
    return JS_NewStringLen(ctx, cls->data(), cls->size());
  }
  return JS_NewStringLen(ctx, "", 0);
}

JSValue ElementSetClassName(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string cls = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  if (cls.empty()) {
    element->RemoveAttribute("class");
  } else {
    element->SetAttribute("class", cls);
  }
  ImplFor(ctx, this_val)->MarkAttrDirty(element, "class");
  return JS_UNDEFINED;
}

namespace {

AttrWrapper* UnwrapAttribute(JSContext* ctx, JSValueConst this_val)
{
  auto* wrapper = static_cast<AttrWrapper*>(JS_GetOpaque(this_val, g_attr_class_id));
  if (wrapper == nullptr || wrapper->element == nullptr) {
    JS_ThrowTypeError(ctx, "not an Attr");
    return nullptr;
  }
  return wrapper;
}

} // namespace

JSValue AttrGetName(JSContext* ctx, JSValueConst this_val)
{
  AttrWrapper* wrapper = UnwrapAttribute(ctx, this_val);
  if (wrapper == nullptr) {
    return JS_EXCEPTION;
  }
  return JS_NewStringLen(ctx, wrapper->name.data(), wrapper->name.size());
}

JSValue AttrGetValue(JSContext* ctx, JSValueConst this_val)
{
  AttrWrapper* wrapper = UnwrapAttribute(ctx, this_val);
  if (wrapper == nullptr) {
    return JS_EXCEPTION;
  }
  const auto value = wrapper->element->GetAttribute(wrapper->name);
  if (!value.has_value()) {
    return JS_NewStringLen(ctx, "", 0);
  }
  return JS_NewStringLen(ctx, value->data(), value->size());
}

JSValue AttrSetValue(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  AttrWrapper* wrapper = UnwrapAttribute(ctx, this_val);
  if (wrapper == nullptr) {
    return JS_EXCEPTION;
  }
  bool ok = false;
  const std::string string_value = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  wrapper->element->SetAttribute(wrapper->name, string_value);
  wrapper->impl->MarkDomDirty(wrapper->element);
  return JS_UNDEFINED;
}

JSValue AttrGetNodeValue(JSContext* ctx, JSValueConst this_val)
{
  return AttrGetValue(ctx, this_val);
}

JSValue AttrSetNodeValue(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  return AttrSetValue(ctx, this_val, value);
}

JSValue AttrGetOwnerElement(JSContext* ctx, JSValueConst this_val)
{
  AttrWrapper* wrapper = UnwrapAttribute(ctx, this_val);
  if (wrapper == nullptr) {
    return JS_EXCEPTION;
  }
  return wrapper->impl->WrapNode(wrapper->element);
}

JSValue ElementGetAttributes(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  JSValue arr = JS_NewArray(ctx);
  JS_SetPrototype(ctx, arr, impl->named_node_map_proto);
  const auto& attrs = element->attributes();
  for (std::size_t i = 0; i < attrs.size(); ++i) {
    JSValue obj = impl->WrapAttribute(element, attrs[i].name);
    JS_SetPropertyUint32(ctx, // NOLINT: (this_obj, index, value); steals obj
                         arr,
                         static_cast<uint32_t>(i),
                         JS_DupValue(ctx, obj));
    // NamedNodeMap supports both indexed and named access, e.g.
    // element.attributes.style.value.
    JS_SetPropertyStr(ctx, arr, attrs[i].name.c_str(), obj);
  }
  return arr;
}

JSValue NamedNodeMapItem(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  int32_t index = -1;
  if (argc < 1 || JS_ToInt32(ctx, &index, argv[0]) < 0) {
    return JS_EXCEPTION;
  }
  if (index < 0) {
    return JS_NULL;
  }
  JSValue value = JS_GetPropertyUint32(ctx, this_val, static_cast<uint32_t>(index));
  return JS_IsUndefined(value) ? (JS_FreeValue(ctx, value), JS_NULL) : value;
}

JSValue
NamedNodeMapGetNamedItem(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "getNamedItem requires a name");
  }
  bool ok = false;
  const std::string name = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  JSValue value = JS_GetPropertyStr(ctx, this_val, name.c_str());
  return JS_IsUndefined(value) ? (JS_FreeValue(ctx, value), JS_NULL) : value;
}

JSValue ElementGetChildren(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(ctx, this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "not a ParentNode");
  }
  // Live: re-queries the direct element children on every JS DOM mutation.
  return impl->MakeLiveCollection(node, Impl::LiveKind::kChildren, "");
}

JSValue ElementGetFirstElementChild(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  for (dom::Node* child : element->ChildNodes()) {
    if (dom::Element* el = AsElement(child)) {
      return impl->WrapNode(el);
    }
  }
  return JS_NULL;
}

JSValue ElementGetLastElementChild(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  dom::Element* found = nullptr;
  for (dom::Node* child : element->ChildNodes()) {
    if (dom::Element* el = AsElement(child)) {
      found = el;
    }
  }
  return impl->WrapNode(found);
}

// The adjacent element sibling of |element| in the given direction.
dom::Element* ElementSiblingOf(dom::Element* element, int offset)
{
  if (element == nullptr || element->parent() == nullptr) {
    return nullptr;
  }
  dom::Node* parent = element->parent();
  dom::Element* prev = nullptr;
  bool seen = false;
  for (dom::Node* child : parent->ChildNodes()) {
    dom::Element* el = AsElement(child);
    if (el == nullptr) {
      continue;
    }
    if (el == element) {
      seen = true;
      if (offset < 0) {
        return prev;
      }
    } else if (seen && offset > 0) {
      return el;
    } else if (!seen) {
      prev = el;
    }
  }
  return nullptr;
}

JSValue ElementGetNextElementSibling(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  return impl->WrapNode(ElementSiblingOf(element, +1));
}

JSValue ElementGetPreviousElementSibling(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  return impl->WrapNode(ElementSiblingOf(element, -1));
}

JSValue ElementGetDataset(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  // A dataset object per access; its exotic property handlers read/write the
  // element's data-* attributes through the class's NodeWrapper opaque.
  auto* w = new NodeWrapper{impl, element};
  JSValue dataset = JS_NewObjectClass(ctx, g_dataset_class_id);
  JS_SetOpaque(dataset, w);
  return dataset;
}

JSValue ElementMatches(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "matches: not an element");
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "matches requires a selector");
  }
  bool ok = false;
  const std::string selector = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  return JS_NewBool(ctx, dom::MatchesCompoundSelector(*element, selector));
}

JSValue ElementClosest(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "closest: not an element");
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "closest requires a selector");
  }
  bool ok = false;
  const std::string selector = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  for (dom::Element* e = element; e != nullptr; e = AsElement(e->parent())) {
    if (dom::MatchesCompoundSelector(*e, selector)) {
      return impl->WrapNode(e);
    }
  }
  return JS_NULL;
}

JSValue ElementRemove(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(ctx, this_val);
  dom::Node* parent = node != nullptr ? node->parent() : nullptr;
  if (impl == nullptr || node == nullptr || parent == nullptr) {
    return JS_UNDEFINED;
  }
  std::unique_ptr<dom::Node> removed = parent->RemoveChild(node);
  impl->TakeOwnership(node, std::move(removed));
  impl->MarkDomDirty(parent);
  return JS_UNDEFINED;
}

// Parses the tabindex content attribute with HTML's "rules for parsing
// integers": leading ASCII whitespace, an optional sign, then digits; any
// trailing characters are ignored, and a value with no digits is invalid.
// Values are clamped to +/- 1e6 (well beyond any real page's tab order; the
// clamp only stops a huge literal from overflowing while it is parsed).
std::optional<int> ParseTabIndexAttribute(std::string_view value)
{
  const auto is_space = [](char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
  };
  std::size_t i = 0;
  while (i < value.size() && is_space(value[i])) {
    ++i;
  }
  bool negative = false;
  if (i < value.size() && (value[i] == '+' || value[i] == '-')) {
    negative = value[i] == '-';
    ++i;
  }
  if (i >= value.size() || value[i] < '0' || value[i] > '9') {
    return std::nullopt;
  }
  int parsed = 0;
  while (i < value.size() && value[i] >= '0' && value[i] <= '9') {
    if (parsed < 1000000) {
      parsed = (parsed * 10) + (value[i] - '0');
    }
    ++i;
  }
  return negative ? -parsed : parsed;
}

// HTMLElement.focus(options) (WHATWG HTML §6.6.3): run the browser's focusing
// steps through the PageApis callback (the same path clicks, Tab and autofocus
// use), then dispatch the UI Events focus sequence.  The options dictionary is
// accepted but ignored: preventScroll has no effect because focusing does not
// scroll the page in this engine (documented limitation).  Focusing an element
// that is not a focusable area, or is detached, is a no-op.
JSValue ElementFocus(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.focus_element) {
    // No browsing context / focus controller attached (a standalone binder):
    // there is no element that could become focused, so focus() is a no-op.
    return JS_UNDEFINED;
  }
  dom::Element* previous = impl->apis.active_element ? impl->apis.active_element() : nullptr;
  if (!impl->apis.focus_element(*element)) {
    return JS_UNDEFINED; // not focusable / detached: no-op
  }
  if (previous != element) {
    impl->PerformFocusUpdate(previous, element);
  }
  return JS_UNDEFINED;
}

// HTMLElement.blur() (WHATWG HTML §6.6.3): clear the focus when the element is
// the document's focused element, dispatching blur/focusout.
JSValue ElementBlur(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.blur_element) {
    return JS_UNDEFINED;
  }
  if (impl->apis.blur_element(*element)) {
    impl->PerformFocusUpdate(element, nullptr);
  }
  return JS_UNDEFINED;
}

// The historical default tabIndex when no valid tabindex attribute is present
// (HTML §6.6.3): 0 for the form-control/link element types, -1 otherwise.  The
// varying default is a legacy artifact of the IDL, not of focusability.
int DefaultTabIndex(const dom::Element& element)
{
  const std::string_view tag = element.tag_name();
  const std::string_view ns = element.namespace_uri();
  if ((ns == "http://www.w3.org/2000/svg" || ns == "http://www.w3.org/1998/Math/MathML") &&
      tag == "a") {
    return 0;
  }
  if (tag == "a" || tag == "area" || tag == "button" || tag == "frame" || tag == "iframe" ||
      tag == "input" || tag == "object" || tag == "select" || tag == "textarea") {
    return 0;
  }
  if (tag == "summary") {
    // A summary is tabIndex 0 only when it is the first summary child of its
    // <details> parent.
    const dom::Node* parent = element.parent();
    if (parent == nullptr || parent->node_type() != dom::NodeType::kElement ||
        static_cast<const dom::Element&>(*parent).tag_name() != "details") {
      return -1;
    }
    for (dom::Node* child : parent->ChildNodes()) {
      if (child->node_type() == dom::NodeType::kElement &&
          static_cast<const dom::Element&>(*child).tag_name() == "summary") {
        return child == &element ? 0 : -1;
      }
    }
  }
  return -1;
}

// HTMLElement.tabIndex (HTML §6.6.3): the parsed tabindex attribute when it is
// a valid integer, otherwise the historical per-element default (0 for links
// and form controls, -1 otherwise).
JSValue ElementGetTabIndex(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  if (const std::optional<std::string_view> value = element->GetAttribute("tabindex");
      value.has_value()) {
    if (const std::optional<int> parsed = ParseTabIndexAttribute(value.value());
        parsed.has_value()) {
      return JS_NewInt32(ctx, parsed.value());
    }
  }
  return JS_NewInt32(ctx, DefaultTabIndex(*element));
}

JSValue ElementSetTabIndex(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  Impl* impl = ImplFor(ctx, this_val);
  if (element == nullptr || impl == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  int32_t index = 0;
  if (JS_ToInt32(ctx, &index, value) != 0) {
    return JS_EXCEPTION;
  }
  element->SetAttribute("tabindex", std::to_string(index));
  impl->MarkAttrDirty(element, "tabindex");
  return JS_UNDEFINED;
}

JSValue ElementGetDir(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  // HTML: dir reflects the content attribute limited to the known values
  // ltr/rtl/auto; an absent or invalid value reads back as "".  Swiper (the
  // slider qq.com mounts over its banners) calls `el.dir.toLowerCase()`, so a
  // missing property used to abort the whole page render.
  const std::optional<std::string_view> dir = element->GetAttribute("dir");
  if (dir.has_value()) {
    const std::string lowered = ToLower(std::string(*dir));
    if (lowered == "ltr" || lowered == "rtl" || lowered == "auto") {
      return JS_NewStringLen(ctx, lowered.data(), lowered.size());
    }
  }
  return JS_NewStringLen(ctx, "", 0);
}

JSValue ElementSetDir(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string dir = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  if (dir.empty()) {
    element->RemoveAttribute("dir");
  } else {
    element->SetAttribute("dir", dir);
  }
  ImplFor(ctx, this_val)->MarkDomDirty(element);
  return JS_UNDEFINED;
}

// Fires the <details> "toggle" event when an attribute write changed whether
// the `open` content attribute is present (HTML §4.11.1).  The spec queues the
// event as a task; this engine dispatches it synchronously, a documented
// approximation.  Only a state *change* fires (setting open="" on an already
// open element is silent).
void FireDetailsToggleOnAttributeChange(Impl* impl, dom::Element& element, bool was_open)
{
  if (impl == nullptr || element.tag_name() != "details" ||
      element.HasAttribute("open") == was_open) {
    return;
  }
  impl->DispatchNonBubblingToNode(&element, "toggle");
}

// HTMLDetailsElement.open: reflects the `open` content attribute.  Setting it
// toggles the details' rendered state (layout reads the attribute) and fires
// "toggle" like any other attribute change would.
JSValue ElementGetOpen(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  return JS_NewBool(ctx, element->HasAttribute("open"));
}

JSValue ElementSetOpen(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const int open = JS_ToBool(ctx, value);
  if (open < 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_EXCEPTION;
  }
  Impl* impl = ImplFor(ctx, this_val);
  const bool was_open = element->HasAttribute("open");
  if (open != 0) {
    element->SetAttribute("open", "");
  } else {
    element->RemoveAttribute("open");
  }
  // `open` is read directly by layout, not through the cascade: the full
  // restyle path (any non-style-only attribute does this) rebuilds the tree.
  impl->MarkAttrDirty(element, "open");
  FireDetailsToggleOnAttributeChange(impl, *element, was_open);
  return JS_UNDEFINED;
}

JSValue ElementGetOuterHTML(JSContext* ctx, JSValueConst this_val)
{
  dom::Node* node = UnwrapNode(ctx, this_val);
  if (node == nullptr) {
    return JS_ThrowTypeError(ctx, "detached node");
  }
  const std::string out = node->ToString();
  return JS_NewStringLen(ctx, out.data(), out.size());
}

// ---------------------------------------------------------------------------
// Form controls / links / images.
//
// value/checked/type/placeholder/disabled/name apply to form controls
// (input/textarea/select/option); href/target/rel to <a>; src/alt/width/
// height/natural*/complete to <img> (and src to <script>).  They live on the
// Element prototype and dispatch on the element's tag name.
// ---------------------------------------------------------------------------

std::vector<std::string> ClassTokens(const dom::Element& element)
{
  std::vector<std::string> out;
  const std::optional<std::string_view> cls = element.GetAttribute("class");
  if (!cls.has_value()) {
    return out;
  }
  std::string current;
  auto flush = [&]() {
    if (!current.empty()) {
      out.push_back(current);
      current.clear();
    }
  };
  for (const char c : cls.value()) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      flush();
    } else {
      current.push_back(c);
    }
  }
  flush();
  return out;
}

void SetClassTokens(dom::Element& element, const std::vector<std::string>& tokens)
{
  std::string out;
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    if (i > 0) {
      out += ' ';
    }
    out += tokens[i];
  }
  if (out.empty()) {
    element.RemoveAttribute("class");
  } else {
    element.SetAttribute("class", out);
  }
}

// classList mutators funnel through here: the class attribute feeds the
// cascade, so the mutation must mark the DOM (and style) dirty like
// setAttribute("class", ...) does.
void ApplyClassTokens(JSContext* ctx,
                      JSValueConst this_val,
                      dom::Element& element,
                      const std::vector<std::string>& tokens)
{
  SetClassTokens(element, tokens);
  if (Impl* impl = ImplFor(ctx, this_val); impl != nullptr) {
    impl->MarkAttrDirty(&element, "class");
  }
}

JSValue ClassListAdd(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "classList: not an element");
  }
  std::vector<std::string> tokens = ClassTokens(*element);
  for (int i = 0; i < argc; ++i) {
    bool ok = false;
    const std::string token = ArgString(ctx, argv[i], &ok);
    if (!ok) {
      return JS_EXCEPTION;
    }
    if (token.empty()) {
      continue;
    }
    bool present = false;
    for (const std::string& t : tokens) {
      if (t == token) {
        present = true;
        break;
      }
    }
    if (!present) {
      tokens.push_back(token);
    }
  }
  ApplyClassTokens(ctx, this_val, *element, tokens);
  return JS_UNDEFINED;
}

JSValue ClassListRemove(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "classList: not an element");
  }
  std::vector<std::string> tokens = ClassTokens(*element);
  for (int i = 0; i < argc; ++i) {
    bool ok = false;
    const std::string token = ArgString(ctx, argv[i], &ok);
    if (!ok) {
      return JS_EXCEPTION;
    }
    tokens.erase(std::remove(tokens.begin(), tokens.end(), token), tokens.end());
  }
  ApplyClassTokens(ctx, this_val, *element, tokens);
  return JS_UNDEFINED;
}

JSValue ClassListContains(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "classList: not an element");
  }
  if (argc < 1) {
    return JS_NewBool(ctx, false);
  }
  bool ok = false;
  const std::string token = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  const std::vector<std::string> tokens = ClassTokens(*element);
  for (const std::string& t : tokens) {
    if (t == token) {
      return JS_NewBool(ctx, true);
    }
  }
  return JS_NewBool(ctx, false);
}

JSValue ClassListToggle(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "classList: not an element");
  }
  if (argc < 1) {
    return JS_NewBool(ctx, false);
  }
  bool ok = false;
  const std::string token = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  std::vector<std::string> tokens = ClassTokens(*element);
  const bool present = std::find(tokens.begin(), tokens.end(), token) != tokens.end();
  // Optional |force| argument: true -> add, false -> remove.
  if (argc >= 2) {
    const int force = JS_ToBool(ctx, argv[1]);
    if (force < 0) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      return JS_EXCEPTION;
    }
    const bool want = force != 0;
    const bool added = want && !present;
    if (want && !present) {
      tokens.push_back(token);
    } else if (!want && present) {
      tokens.erase(std::remove(tokens.begin(), tokens.end(), token), tokens.end());
    }
    ApplyClassTokens(ctx, this_val, *element, tokens);
    return JS_NewBool(ctx, added);
  }
  if (present) {
    tokens.erase(std::remove(tokens.begin(), tokens.end(), token), tokens.end());
    ApplyClassTokens(ctx, this_val, *element, tokens);
    return JS_NewBool(ctx, false);
  }
  tokens.push_back(token);
  ApplyClassTokens(ctx, this_val, *element, tokens);
  return JS_NewBool(ctx, true);
}

JSValue ClassListReplace(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "classList: not an element");
  }
  if (argc < 2) {
    return JS_NewBool(ctx, false);
  }
  bool ok = false;
  const std::string old_token = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  const std::string new_token = ArgString(ctx, argv[1], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  std::vector<std::string> tokens = ClassTokens(*element);
  for (std::string& t : tokens) {
    if (t == old_token) {
      t = new_token;
      ApplyClassTokens(ctx, this_val, *element, tokens);
      return JS_NewBool(ctx, true);
    }
  }
  return JS_NewBool(ctx, false);
}

JSValue ClassListItem(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "classList: not an element");
  }
  const std::vector<std::string> tokens = ClassTokens(*element);
  int64_t index = -1;
  if (argc >= 1) {
    if (JS_ToInt64(ctx, &index, argv[0]) != 0) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      return JS_EXCEPTION;
    }
  }
  if (index < 0 || index >= static_cast<int64_t>(tokens.size())) {
    return JS_NULL;
  }
  return JS_NewStringLen(ctx,
                         tokens[static_cast<std::size_t>(index)].data(),
                         tokens[static_cast<std::size_t>(index)].size());
}

JSValue ClassListGetLength(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "classList: not an element");
  }
  return JS_NewInt32(ctx, static_cast<int32_t>(ClassTokens(*element).size()));
}

JSValue
ClassListToString(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "classList: not an element");
  }
  const std::vector<std::string> tokens = ClassTokens(*element);
  std::string out;
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    if (i > 0) {
      out += ' ';
    }
    out += tokens[i];
  }
  return JS_NewStringLen(ctx, out.data(), out.size());
}

JSValue
ClassListIterator(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "classList: not an element");
  }

  JSValue values = JS_NewArray(ctx);
  const std::vector<std::string> tokens = ClassTokens(*element);
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    JS_SetPropertyUint32(ctx,
                         values,
                         static_cast<std::uint32_t>(index),
                         JS_NewStringLen(ctx, tokens[index].data(), tokens[index].size()));
  }

  JSValue global = JS_GetGlobalObject(ctx);
  JSValue symbol = JS_GetPropertyStr(ctx, global, "Symbol");
  JSValue iterator_symbol = JS_GetPropertyStr(ctx, symbol, "iterator");
  const JSAtom iterator_atom = JS_ValueToAtom(ctx, iterator_symbol);
  JSValue iterator = JS_GetProperty(ctx, values, iterator_atom);
  JSValue result = JS_Call(ctx, iterator, values, 0, nullptr);
  JS_FreeValue(ctx, iterator);
  JS_FreeAtom(ctx, iterator_atom);
  JS_FreeValue(ctx, iterator_symbol);
  JS_FreeValue(ctx, symbol);
  JS_FreeValue(ctx, global);
  JS_FreeValue(ctx, values);
  return result;
}

// HTMLTemplateElement.prototype.content: the template contents fragment.
// Only template elements carry the accessor (it lives on the template
// prototype), and a foreign receiver answers undefined like other
// interface getters do when the platform object is absent.
JSValue ElementGetTemplateContent(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(ctx, this_val);
  dom::HTMLTemplateElement* tpl = (node != nullptr && node->node_type() == dom::NodeType::kElement)
                                      ? static_cast<dom::Element*>(node)->AsTemplate()
                                      : nullptr;
  if (impl == nullptr || tpl == nullptr) {
    return JS_UNDEFINED;
  }
  return impl->WrapNode(tpl->content());
}

JSValue ElementGetClassList(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  // Reuse the node wrapper class (NodeWrapper opaque carries the element);
  // the classList prototype provides the DOMTokenList methods.
  auto* w = new NodeWrapper{impl, element};
  JSValue list = JS_NewObjectClass(ctx, g_node_class_id);
  JS_SetOpaque(list, w);
  JS_SetPrototype(ctx, list, impl->class_list_proto);
  return list;
}

JSValue ElementGetInnerHTML(JSContext* ctx, JSValueConst this_val)
{
  dom::Node* node = UnwrapNode(ctx, this_val);
  if (node == nullptr) {
    return JS_ThrowTypeError(ctx, "detached node");
  }
  // For <template>, innerHTML reads the template contents (browser behavior:
  // the HTML fragment serialization algorithm uses the template's contents).
  if (node->node_type() == dom::NodeType::kElement) {
    if (dom::HTMLTemplateElement* tpl = static_cast<dom::Element*>(node)->AsTemplate()) {
      node = tpl->content();
    }
  }
  std::string out;
  for (dom::Node* child : node->ChildNodes()) {
    out += child->ToString();
  }
  return JS_NewStringLen(ctx, out.data(), out.size());
}

JSValue ElementSetInnerHTML(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string html = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  // <template> writes into its template contents, not the element itself
  // (HTML 4.12.3): the fragment is parsed with the template element as the
  // context and its children are then adopted into template.content().
  dom::Node* target = element;
  if (dom::HTMLTemplateElement* tpl = element->AsTemplate()) {
    target = tpl->content();
  }
  // WHATWG HTML 13.4: parse |html| as a fragment with |element| as the
  // context element (NOT as a full document).  This makes table contexts,
  // <select>, RCDATA/RAWTEXT contexts and the "in body" stray-table-tag rule
  // behave correctly.  The fragment's children become the element's new inner
  // content.  Scripts in the fragment are not executed (the parser only
  // builds elements; it never runs them).
  std::unique_ptr<dom::DocumentFragment> fragment = html::Parser(html, *element).ParseFragment();
  // Scripts created by fragment parsing never execute, wherever the parsed
  // nodes are later moved: flag them "already started" (HTML §4.12.1) so the
  // dynamic-insertion hook leaves them inert.
  impl->MarkScriptsAlreadyStarted(*fragment);
  while (target->first_child() != nullptr) {
    std::unique_ptr<dom::Node> removed = target->RemoveChild(target->first_child());
    impl->TakeOwnership(removed.get(), std::move(removed));
  }
  while (fragment->first_child() != nullptr) {
    target->AppendChild(fragment->RemoveChild(fragment->first_child()));
  }
  impl->MarkDomDirty(target);
  return JS_UNDEFINED;
}

// Parses |html| as a fragment with |context| as the fragment context element
// (WHATWG HTML 13.4) and returns the parsed nodes in document order (scripts
// are not run).
std::vector<std::unique_ptr<dom::Node>> TakeFragmentChildren(std::string_view html,
                                                             const dom::Element& context)
{
  std::unique_ptr<dom::DocumentFragment> fragment = html::Parser(html, context).ParseFragment();
  std::vector<std::unique_ptr<dom::Node>> out;
  while (fragment->first_child() != nullptr) {
    out.push_back(fragment->RemoveChild(fragment->first_child()));
  }
  return out;
}

// Element.insertAdjacentHTML(position, html): parses the fragment and inserts
// it relative to the element — beforebegin/afterend outside (sibling) and
// afterbegin/beforeend inside.
namespace {

// Resolves an insertAdjacent* position to its insertion point.  Returns false
// (with a SyntaxError thrown) when the position is not one of the four.  |*parent|
// is null for "beforebegin"/"afterend" on a detached element: nothing to do.
bool ResolveAdjacentInsertionPoint(JSContext* ctx,
                                   dom::Element* element,
                                   const std::string& position,
                                   dom::Node** parent,
                                   dom::Node** reference)
{
  *parent = nullptr;
  *reference = nullptr;
  if (position == "beforeend") {
    *parent = element;
  } else if (position == "afterbegin") {
    *parent = element;
    *reference = element->first_child();
  } else if (position == "beforebegin" || position == "afterend") {
    *parent = element->parent();
    if (*parent != nullptr) {
      *reference = position == "beforebegin" ? element : SiblingOf(element, +1);
    }
  } else {
    (void)ThrowDomException(
        ctx, "SyntaxError", "insertAdjacent*: unknown position '" + position + "'");
    return false;
  }
  return true;
}

} // namespace

// Element.insertAdjacentHTML(position, html): parses the fragment and inserts it
// relative to the element -- beforebegin/afterend outside (sibling) and
// afterbegin/beforeend inside.  The fragment keeps its document order: inserting
// each node *before the same anchor*, walking forwards, yields that order (the
// previous backwards walk reversed multi-node fragments).
JSValue
ElementInsertAdjacentHTML(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  if (argc < 2) {
    return JS_ThrowTypeError(ctx, "insertAdjacentHTML requires (position, text)");
  }
  bool ok = false;
  const std::string position = ToLower(ArgString(ctx, argv[0], &ok));
  if (!ok) {
    return JS_EXCEPTION;
  }
  const std::string html = ArgString(ctx, argv[1], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  dom::Node* parent = nullptr;
  dom::Node* reference = nullptr;
  if (!ResolveAdjacentInsertionPoint(ctx, element, position, &parent, &reference)) {
    return JS_EXCEPTION;
  }
  if (parent == nullptr) {
    return JS_UNDEFINED; // detached element: nothing to do
  }
  std::vector<std::unique_ptr<dom::Node>> nodes = TakeFragmentChildren(html, *element);
  // insertAdjacentHTML is fragment parsing: its <script> children must never
  // execute, even if moved into the document later (HTML §4.12.1).
  for (auto& node : nodes) {
    impl->MarkScriptsAlreadyStarted(*node);
  }
  for (auto& node : nodes) {
    parent->InsertBefore(std::move(node), reference);
  }
  impl->MarkDomDirty(element);
  return JS_UNDEFINED;
}

// Element.insertAdjacentElement(position, element): inserts an existing element
// at the given position and returns it (null for a detached sibling position).
JSValue
ElementInsertAdjacentElement(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  if (argc < 2) {
    return JS_ThrowTypeError(ctx, "insertAdjacentElement requires (position, element)");
  }
  bool ok = false;
  const std::string position = ToLower(ArgString(ctx, argv[0], &ok));
  if (!ok) {
    return JS_EXCEPTION;
  }
  dom::Element* inserted = AsElement(UnwrapNode(ctx, argv[1]));
  if (inserted == nullptr) {
    return JS_ThrowTypeError(ctx, "insertAdjacentElement: argument is not an element");
  }
  dom::Node* parent = nullptr;
  dom::Node* reference = nullptr;
  if (!ResolveAdjacentInsertionPoint(ctx, element, position, &parent, &reference)) {
    return JS_EXCEPTION;
  }
  if (parent == nullptr) {
    return JS_NULL; // detached element: spec returns null
  }
  return InsertNodeChecked(ctx, impl, parent, inserted, reference, "insertAdjacentElement");
}

// Element.insertAdjacentText(position, text): inserts a new Text node holding
// |text| at the given position.
JSValue
ElementInsertAdjacentText(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  if (argc < 2) {
    return JS_ThrowTypeError(ctx, "insertAdjacentText requires (position, text)");
  }
  bool ok = false;
  const std::string position = ToLower(ArgString(ctx, argv[0], &ok));
  if (!ok) {
    return JS_EXCEPTION;
  }
  const std::string text = ArgString(ctx, argv[1], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  dom::Node* parent = nullptr;
  dom::Node* reference = nullptr;
  if (!ResolveAdjacentInsertionPoint(ctx, element, position, &parent, &reference)) {
    return JS_EXCEPTION;
  }
  if (parent == nullptr) {
    return JS_UNDEFINED;
  }
  // The new Text node goes straight into the DOM: JS gets no reference to it, so
  // it needs no wrapper registry entry.
  parent->InsertBefore(std::make_unique<dom::Text>(text), reference);
  impl->MarkDomDirty(parent);
  return JS_UNDEFINED;
}

JSValue ElementGetStyle(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  // A fresh style object per access; its opaque carries the element so the
  // style methods can read/write the element's style attribute.
  auto* w = new NodeWrapper{impl, element};
  JSValue style = JS_NewObjectClass(ctx, g_node_class_id);
  JS_SetOpaque(style, w);
  JS_SetPrototype(ctx, style, impl->style_proto);
  return style;
}

JSValue ElementGetAttribute(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "getAttribute requires a name");
  }
  bool ok = false;
  const std::string name = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  const std::optional<std::string_view> value = element->GetAttribute(name);
  if (value.has_value()) {
    return JS_NewStringLen(ctx, value->data(), value->size());
  }
  return JS_NULL;
}

// element.getAttributeNames(): the attribute names in document order (DOM
// Standard §4.9).
JSValue ElementGetAttributeNames(JSContext* ctx,
                                 JSValueConst this_val,
                                 int /*argc*/,
                                 JSValueConst* /*argv*/)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  JSValue names = JS_NewArray(ctx);
  std::uint32_t index = 0;
  for (const dom::Attribute& attribute : element->attributes()) {
    JS_SetPropertyUint32(
        ctx, names, index++, JS_NewStringLen(ctx, attribute.name.data(), attribute.name.size()));
  }
  return names;
}

// element.scrollIntoView([options]): scrolls the element into view through the
// browser layer's scroll request (only the vertical axis is live, so the
// horizontal part is a no-op — documented).
JSValue ElementScrollIntoView(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr || argc > 0) {
    // Options objects (block/inline/behavior) are accepted but ignored: the
    // engine scrolls the element's top into view, like block:"start".
    (void)argv;
  }
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.scroll_to || !impl->apis.element_geometry) {
    return JS_UNDEFINED;
  }
  const auto geometry = impl->apis.element_geometry(*element);
  if (geometry.has_value()) {
    impl->apis.scroll_to(0.0, static_cast<double>(geometry->y));
  }
  return JS_UNDEFINED;
}

JSValue ElementSetAttribute(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  if (argc < 2) {
    return JS_ThrowTypeError(ctx, "setAttribute requires (name, value)");
  }
  bool ok = false;
  const std::string name = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  const std::string value = ArgString(ctx, argv[1], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  Impl* impl = ImplFor(ctx, this_val);
  const bool was_open = element->HasAttribute("open");
  const bool is_input = element->tag_name() == "input";
  const std::string old_type = is_input ? html::InputTypeKeyword(*element) : std::string();
  const std::string old_value =
      is_input && name == "type" ? html::InputValue(*element) : std::string();
  element->SetAttribute(name, value);
  impl->RecordAttributeMutation(element, name);
  // Media-loading attributes (src/srcset/...) only need the late-fetch pass;
  // style-only names take the scoped-restyle path; anything else can affect
  // geometry outside the cascade and takes the full restyle path.
  impl->MarkAttrDirty(element, name);
  if (is_input) {
    // The value/defaultValue model hooks (WHATWG HTML §4.10.5.5): a changed
    // `value` content attribute moves a non-dirty current value, and a `type`
    // change runs the type-change steps and re-sanitizes the current value.
    if (name == "value") {
      html::InputValueAttributeChanged(*element);
    } else if (name == "type") {
      html::InputTypeChanged(*element, old_type, old_value);
    }
  }
  if (name == "open") {
    FireDetailsToggleOnAttributeChange(impl, *element, was_open);
  }
  return JS_UNDEFINED;
}

JSValue ElementRemoveAttribute(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  if (argc < 1) {
    return JS_UNDEFINED;
  }
  bool ok = false;
  const std::string name = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  Impl* impl = ImplFor(ctx, this_val);
  const bool was_open = element->HasAttribute("open");
  const bool is_input = element->tag_name() == "input";
  const std::string old_type = is_input ? html::InputTypeKeyword(*element) : std::string();
  const std::string old_value =
      is_input && name == "type" ? html::InputValue(*element) : std::string();
  element->RemoveAttribute(name);
  impl->RecordAttributeMutation(element, name);
  impl->MarkAttrDirty(element, name);
  if (is_input) {
    if (name == "value") {
      html::InputValueAttributeChanged(*element);
    } else if (name == "type") {
      html::InputTypeChanged(*element, old_type, old_value);
    }
  }
  if (name == "open") {
    FireDetailsToggleOnAttributeChange(impl, *element, was_open);
  }
  return JS_UNDEFINED;
}

JSValue ElementHasAttribute(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  if (argc < 1) {
    return JS_NewBool(ctx, false);
  }
  bool ok = false;
  const std::string name = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  return JS_NewBool(ctx, element->HasAttribute(name));
}

JSValue ElementToggleAttribute(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "toggleAttribute requires a name");
  }
  bool ok = false;
  const std::string name = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }

  const bool present = element->HasAttribute(name);
  const bool force = argc >= 2 ? JS_ToBool(ctx, argv[1]) != 0 : !present;
  if (force == present) {
    return JS_NewBool(ctx, present);
  }
  Impl* impl = ImplFor(ctx, this_val);
  const bool was_open = element->HasAttribute("open");
  const bool is_input = element->tag_name() == "input";
  const std::string old_type = is_input ? html::InputTypeKeyword(*element) : std::string();
  const std::string old_value =
      is_input && name == "type" ? html::InputValue(*element) : std::string();
  if (force) {
    element->SetAttribute(name, "");
  } else {
    element->RemoveAttribute(name);
  }
  impl->RecordAttributeMutation(element, name);
  impl->MarkDomDirty(element);
  if (is_input && (name == "value" || name == "type")) {
    if (name == "value") {
      html::InputValueAttributeChanged(*element);
    } else {
      html::InputTypeChanged(*element, old_type, old_value);
    }
  }
  if (name == "open") {
    FireDetailsToggleOnAttributeChange(impl, *element, was_open);
  }
  return JS_NewBool(ctx, force);
}

JSValue ElementQuerySelector(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(ctx, this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "detached node");
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "querySelector requires a selector");
  }
  bool ok = false;
  const std::string selector = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  return impl->WrapNode(dom::QuerySelector(*node, selector));
}

JSValue ElementQuerySelectorAll(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(ctx, this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "detached node");
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "querySelectorAll requires a selector");
  }
  bool ok = false;
  const std::string selector = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  return impl->MakeElementArray(dom::QuerySelectorAll(*node, selector));
}

// Collects descendant elements whose tag matches |name| ("*" matches all).
void CollectByTag(const dom::Node& root, std::string_view name, std::vector<dom::Element*>& out)
{
  for (dom::Node* child : root.ChildNodes()) {
    if (dom::Element* el = AsElement(child)) {
      if (name == "*" || el->tag_name() == name) {
        out.push_back(el);
      }
      CollectByTag(*el, name, out);
    }
  }
}

// Collects descendant elements that carry every class in |classes|.
void CollectByClass(const dom::Node& root,
                    const std::vector<std::string_view>& classes,
                    std::vector<dom::Element*>& out)
{
  for (dom::Node* child : root.ChildNodes()) {
    if (dom::Element* el = AsElement(child)) {
      const std::vector<std::string_view> el_classes = el->ClassList();
      bool match = true;
      for (std::string_view cls : classes) {
        const bool found = std::any_of(
            el_classes.begin(), el_classes.end(), [&](std::string_view c) { return c == cls; });
        if (!found) {
          match = false;
          break;
        }
      }
      if (match) {
        out.push_back(el);
      }
      CollectByClass(*el, classes, out);
    }
  }
}

JSValue
ElementGetElementsByTagName(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(ctx, this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "detached node");
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "getElementsByTagName requires a name");
  }
  bool ok = false;
  const std::string name = ToLower(ArgString(ctx, argv[0], &ok));
  if (!ok) {
    return JS_EXCEPTION;
  }
  // Live: re-queries the scoped descendants on every JS DOM mutation.
  return impl->MakeLiveCollection(node, Impl::LiveKind::kTagName, name);
}

JSValue
ElementGetElementsByClassName(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(ctx, this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "detached node");
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "getElementsByClassName requires a name");
  }
  bool ok = false;
  const std::string arg = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  std::vector<std::string_view> classes;
  std::size_t i = 0;
  while (i < arg.size()) {
    while (i < arg.size() && (arg[i] == ' ' || arg[i] == '\t')) {
      ++i;
    }
    const std::size_t start = i;
    while (i < arg.size() && arg[i] != ' ' && arg[i] != '\t') {
      ++i;
    }
    if (i > start) {
      classes.emplace_back(arg.data() + start, i - start);
    }
  }
  (void)classes; // MakeLiveCollection re-queries using the raw |arg| token list.
  // Live: re-queries the scoped descendants on every JS DOM mutation.
  return impl->MakeLiveCollection(node, Impl::LiveKind::kClassName, arg);
}

// ---------------------------------------------------------------------------
// Document methods and accessors.
// ---------------------------------------------------------------------------

#include "generated/element_binding_generated.inc"
#include "generated/html_element_generated.inc"

void DefineClassListPrototype(JSContext* ctx, Impl& impl)
{

  JSValue global = JS_GetGlobalObject(ctx);
  JSValue symbol = JS_GetPropertyStr(ctx, global, "Symbol");
  JSValue iterator_symbol = JS_GetPropertyStr(ctx, symbol, "iterator");
  const JSAtom iterator_atom = JS_ValueToAtom(ctx, iterator_symbol);
  JS_DefinePropertyValue(ctx,
                         impl.class_list_proto,
                         iterator_atom,
                         JS_NewCFunction(ctx, ClassListIterator, "values", 0),
                         JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
  JS_FreeAtom(ctx, iterator_atom);
  JS_FreeValue(ctx, iterator_symbol);
  JS_FreeValue(ctx, symbol);
  JS_FreeValue(ctx, global);
  DefineGeneratedElementBindingClassListProto(ctx, impl);
}

void DefineElementPrototype(JSContext* ctx, Impl& impl)
{

  // Element layout geometry (getBoundingClientRect + offset/client series).

  // Element scroll geometry (documented subset: only documentElement/<body>
  // scroll; other elements report 0 and their setters no-op).

  // Element-level global event handler attributes (element.onclick = fn);
  // the table and accessors live in event_binding.cpp.
  DefineElementEventHandlers(ctx, impl);

  // innerText (read-only): an approximation returning the element's
  // textContent.  Real innerText reflects *rendered* text (hidden elements
  // excluded, whitespace normalized); the engine has no layout-backed innerText
  // yet, and bing reads innerText mainly to sniff page text during bootstrap.

  // HTMLTemplateElement.prototype.content (HTML 4.12.3): only template
  // elements get the accessor, so `'content' in div` stays false.

  // HTMLDetailsElement.open (HTML 4.11.1): only <details> elements get the
  // accessor, so `'open' in div` stays false.

  // Reflected attributes generated from html_element.webidl: hidden, title,
  // lang and inert (plus the focus() registration).
  DefineGeneratedHTMLElement(ctx, impl);

  // Focus-related attributes: tabindex drives sequential focus navigation and
  // inert removes a subtree from focus/activation (HTML §6.6.1).

  // Form controls (input/textarea/select/option/button).

  // Links (<a>).

  for (const auto& [name, part] :
       std::array<std::pair<const char*, AnchorUrlPart>, 7>{{{"protocol", AnchorUrlPart::kProtocol},
                                                             {"host", AnchorUrlPart::kHost},
                                                             {"hostname", AnchorUrlPart::kHostname},
                                                             {"port", AnchorUrlPart::kPort},
                                                             {"pathname", AnchorUrlPart::kPathname},
                                                             {"search", AnchorUrlPart::kSearch},
                                                             {"hash", AnchorUrlPart::kHash}}}) {
    DefineAccessor(ctx,
                   impl.element_proto,
                   name,
                   MakeGetterMagic(ctx, name, ElementGetAnchorUrlPart, static_cast<int>(part)),
                   JS_UNDEFINED);
  }

  // Images (<img>) and script src.

  // Media (<video>): duration/currentTime/paused + play()/pause() (methods
  // above).  currentTime is seekable; non-video elements report the media
  // defaults (NaN duration, paused = true).

  DefineGeneratedElementBindingElementProto(ctx, impl);
  DefineGeneratedElementBindingSvgElementProto(ctx, impl);
  DefineGeneratedElementBindingHtmlTemplateElementProto(ctx, impl);
  DefineGeneratedElementBindingHtmlDetailsElementProto(ctx, impl);
}

void EnsureDatasetClassRegistered(JSRuntime* rt)
{
  std::lock_guard<std::mutex> lock(g_dataset_class_mutex);
  JS_NewClassID(rt, &g_dataset_class_id);
  // A reused JSRuntime address must not skip registration (see
  // xhr_binding.cpp); JS_IsRegisteredClass is the authoritative check.
  if (!JS_IsRegisteredClass(rt, g_dataset_class_id)) {
    g_dataset_exotic.get_property = &DatasetGetProperty;
    g_dataset_exotic.set_property = &DatasetSetProperty;
    g_dataset_exotic.get_own_property_names = &DatasetGetOwnPropertyNames;
    JSClassDef def;
    std::memset(&def, 0, sizeof(def));
    def.class_name = "DOMStringMap";
    def.finalizer = &DatasetFinalizer;
    def.exotic = &g_dataset_exotic;
    JS_NewClass(rt, g_dataset_class_id, &def);
    g_dataset_class_registered.insert(rt);
  }
}

} // namespace neko::javascript
