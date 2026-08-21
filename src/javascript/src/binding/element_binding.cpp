// neko::javascript DOM bindings — Element family.
//
// Part of the dom_binding split (see binding/binding_internal.h): the Element
// prototype callbacks (attributes, traversal, query, innerHTML/outerHTML,
// classList, dataset), the dataset exotic-object machinery, and
// DefineElementPrototype / DefineClassListPrototype.

#include "neko/dom/element.h"
#include "neko/dom/node.h"
#include "neko/dom/query.h"
#include "neko/html/parser.h"

#include "binding_internal.h"

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
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::string name = NodeNameOf(*element);
  return JS_NewStringLen(ctx, name.data(), name.size());
}

JSValue ElementGetId(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
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
  dom::Element* element = AsElement(UnwrapNode(this_val));
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
  ImplFor(ctx, this_val)->MarkDomDirty();
  return JS_UNDEFINED;
}

JSValue ElementGetClassName(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
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
  dom::Element* element = AsElement(UnwrapNode(this_val));
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
  ImplFor(ctx, this_val)->MarkDomDirty();
  return JS_UNDEFINED;
}

JSValue ElementGetAttributes(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  JSValue arr = JS_NewArray(ctx);
  const auto& attrs = element->attributes();
  for (std::size_t i = 0; i < attrs.size(); ++i) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(
        ctx, obj, "name", JS_NewStringLen(ctx, attrs[i].name.data(), attrs[i].name.size()));
    JS_SetPropertyStr(
        ctx, obj, "value", JS_NewStringLen(ctx, attrs[i].value.data(), attrs[i].value.size()));
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

JSValue ElementGetChildren(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  std::vector<dom::Element*> elements;
  for (dom::Node* child : element->ChildNodes()) {
    if (dom::Element* el = AsElement(child)) {
      elements.push_back(el);
    }
  }
  return impl->MakeElementArray(elements);
}

JSValue ElementGetFirstElementChild(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
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
  dom::Element* element = AsElement(UnwrapNode(this_val));
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
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  return impl->WrapNode(ElementSiblingOf(element, +1));
}

JSValue ElementGetPreviousElementSibling(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  return impl->WrapNode(ElementSiblingOf(element, -1));
}

JSValue ElementGetDataset(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
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
  dom::Element* element = AsElement(UnwrapNode(this_val));
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
  dom::Element* element = AsElement(UnwrapNode(this_val));
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
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr || node->parent() == nullptr) {
    return JS_UNDEFINED;
  }
  std::unique_ptr<dom::Node> removed = node->parent()->RemoveChild(node);
  impl->TakeOwnership(node, std::move(removed));
  return JS_UNDEFINED;
}

// DOMRect.toJSON(): a plain object with the rect fields (numbers).

JSValue ElementGetHidden(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  return JS_NewBool(ctx, element->HasAttribute("hidden"));
}

JSValue ElementSetHidden(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const int v = JS_ToBool(ctx, value);
  if (v < 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_EXCEPTION;
  }
  if (v != 0) {
    element->SetAttribute("hidden", "");
  } else {
    element->RemoveAttribute("hidden");
  }
  ImplFor(ctx, this_val)->MarkDomDirty();
  return JS_UNDEFINED;
}

JSValue ElementGetTitle(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::optional<std::string_view> title = element->GetAttribute("title");
  return title.has_value() ? JS_NewStringLen(ctx, title->data(), title->size())
                           : JS_NewStringLen(ctx, "", 0);
}

JSValue ElementSetTitle(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string title = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  if (title.empty()) {
    element->RemoveAttribute("title");
  } else {
    element->SetAttribute("title", title);
  }
  return JS_UNDEFINED;
}

JSValue ElementGetLang(JSContext* ctx, JSValueConst this_val)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  const std::optional<std::string_view> lang = element->GetAttribute("lang");
  return lang.has_value() ? JS_NewStringLen(ctx, lang->data(), lang->size())
                          : JS_NewStringLen(ctx, "", 0);
}

JSValue ElementSetLang(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string lang = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  if (lang.empty()) {
    element->RemoveAttribute("lang");
  } else {
    element->SetAttribute("lang", lang);
  }
  return JS_UNDEFINED;
}

JSValue ElementGetOuterHTML(JSContext* ctx, JSValueConst this_val)
{
  dom::Node* node = UnwrapNode(this_val);
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

JSValue ClassListAdd(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
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
  SetClassTokens(*element, tokens);
  return JS_UNDEFINED;
}

JSValue ClassListRemove(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
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
  SetClassTokens(*element, tokens);
  return JS_UNDEFINED;
}

JSValue ClassListContains(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
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
  dom::Element* element = AsElement(UnwrapNode(this_val));
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
    SetClassTokens(*element, tokens);
    return JS_NewBool(ctx, added);
  }
  if (present) {
    tokens.erase(std::remove(tokens.begin(), tokens.end(), token), tokens.end());
    SetClassTokens(*element, tokens);
    return JS_NewBool(ctx, false);
  }
  tokens.push_back(token);
  SetClassTokens(*element, tokens);
  return JS_NewBool(ctx, true);
}

JSValue ClassListReplace(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
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
      SetClassTokens(*element, tokens);
      return JS_NewBool(ctx, true);
    }
  }
  return JS_NewBool(ctx, false);
}

JSValue ClassListItem(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
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
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (element == nullptr) {
    return JS_ThrowTypeError(ctx, "classList: not an element");
  }
  return JS_NewInt32(ctx, static_cast<int32_t>(ClassTokens(*element).size()));
}

JSValue
ClassListToString(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
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

JSValue ElementGetClassList(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
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
  dom::Node* node = UnwrapNode(this_val);
  if (node == nullptr) {
    return JS_ThrowTypeError(ctx, "detached node");
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
  dom::Element* element = AsElement(UnwrapNode(this_val));
  if (impl == nullptr || element == nullptr) {
    return JS_ThrowTypeError(ctx, "not an element");
  }
  bool ok = false;
  const std::string html = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  // Parse the fragment through the full document parser and move the <body>
  // children into this element.  Scripts in the fragment are not executed.
  std::unique_ptr<dom::Document> parsed = html::Parser(html).Parse();
  dom::Element* body = nullptr;
  for (dom::Node* child : parsed->ChildNodes()) {
    if (dom::Element* el = AsElement(child)) {
      if (el->tag_name() == "body") {
        body = el;
        break;
      }
      for (dom::Node* inner : el->ChildNodes()) {
        if (dom::Element* in_el = AsElement(inner)) {
          if (in_el->tag_name() == "body") {
            body = in_el;
            break;
          }
        }
      }
      if (body != nullptr) {
        break;
      }
    }
  }
  while (element->first_child() != nullptr) {
    std::unique_ptr<dom::Node> removed = element->RemoveChild(element->first_child());
    impl->TakeOwnership(removed.get(), std::move(removed));
  }
  if (body != nullptr) {
    while (body->first_child() != nullptr) {
      std::unique_ptr<dom::Node> child = body->RemoveChild(body->first_child());
      element->AppendChild(std::move(child));
    }
  }
  impl->MarkDomDirty();
  return JS_UNDEFINED;
}

// Parses |html| as a fragment and moves the parsed <body> children into a
// fresh document, returning them in document order (scripts are not run).
std::vector<std::unique_ptr<dom::Node>> TakeFragmentChildren(std::string_view html)
{
  std::unique_ptr<dom::Document> parsed = html::Parser(html).Parse();
  std::vector<std::unique_ptr<dom::Node>> out;
  dom::Element* body = nullptr;
  for (dom::Node* child : parsed->ChildNodes()) {
    if (dom::Element* el = AsElement(child)) {
      if (el->tag_name() == "body") {
        body = el;
        break;
      }
      for (dom::Node* inner : el->ChildNodes()) {
        if (dom::Element* in_el = AsElement(inner)) {
          if (in_el->tag_name() == "body") {
            body = in_el;
            break;
          }
        }
      }
      if (body != nullptr) {
        break;
      }
    }
  }
  if (body != nullptr) {
    while (body->first_child() != nullptr) {
      out.push_back(body->RemoveChild(body->first_child()));
    }
  }
  return out;
}

// Element.insertAdjacentHTML(position, html): parses the fragment and inserts
// it relative to the element — beforebegin/afterend outside (sibling) and
// afterbegin/beforeend inside.
JSValue
ElementInsertAdjacentHTML(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
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
  std::vector<std::unique_ptr<dom::Node>> nodes = TakeFragmentChildren(html);
  const auto insert_before =
      [&](dom::Node* parent, std::unique_ptr<dom::Node>&& node, dom::Node* reference) {
        parent->InsertBefore(std::move(node), reference);
      };
  if (position == "beforeend") {
    for (auto& node : nodes) {
      element->AppendChild(std::move(node));
    }
  } else if (position == "afterbegin") {
    dom::Node* first = element->first_child();
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
      element->InsertBefore(std::move(*it), first);
    }
  } else if (position == "beforebegin" || position == "afterend") {
    dom::Node* parent = element->parent();
    if (parent == nullptr) {
      return JS_UNDEFINED; // detached element: nothing to do
    }
    dom::Node* reference = position == "beforebegin" ? element : SiblingOf(element, +1);
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
      insert_before(parent, std::move(*it), reference);
    }
  }
  impl->MarkDomDirty();
  return JS_UNDEFINED;
}

JSValue ElementGetStyle(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Element* element = AsElement(UnwrapNode(this_val));
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
  dom::Element* element = AsElement(UnwrapNode(this_val));
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

JSValue ElementSetAttribute(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
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
  element->SetAttribute(name, value);
  Impl* impl = ImplFor(ctx, this_val);
  impl->RecordAttributeMutation(element, name);
  impl->MarkDomDirty();
  return JS_UNDEFINED;
}

JSValue ElementRemoveAttribute(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
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
  element->RemoveAttribute(name);
  Impl* impl = ImplFor(ctx, this_val);
  impl->RecordAttributeMutation(element, name);
  impl->MarkDomDirty();
  return JS_UNDEFINED;
}

JSValue ElementHasAttribute(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  dom::Element* element = AsElement(UnwrapNode(this_val));
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

JSValue ElementQuerySelector(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
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
  dom::Node* node = UnwrapNode(this_val);
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
  dom::Node* node = UnwrapNode(this_val);
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
  std::vector<dom::Element*> out;
  CollectByTag(*node, name, out);
  return impl->MakeElementArray(out);
}

JSValue
ElementGetElementsByClassName(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
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
  std::vector<dom::Element*> out;
  CollectByClass(*node, classes, out);
  return impl->MakeElementArray(out);
}

// ---------------------------------------------------------------------------
// Document methods and accessors.
// ---------------------------------------------------------------------------

void DefineClassListPrototype(JSContext* ctx, Impl& impl)
{
  static const std::array<JSCFunctionListEntry, 7> kMethods = {{
      JS_CFUNC_DEF("add", 1, ClassListAdd),
      JS_CFUNC_DEF("remove", 1, ClassListRemove),
      JS_CFUNC_DEF("toggle", 1, ClassListToggle),
      JS_CFUNC_DEF("contains", 1, ClassListContains),
      JS_CFUNC_DEF("replace", 2, ClassListReplace),
      JS_CFUNC_DEF("item", 1, ClassListItem),
      JS_CFUNC_DEF("toString", 0, ClassListToString),
  }};
  JS_SetPropertyFunctionList(
      ctx, impl.class_list_proto, kMethods.data(), static_cast<int>(kMethods.size()));
  DefineGetter(ctx, impl.class_list_proto, "length", MakeGetter(ctx, "length", ClassListGetLength));
}

void DefineElementPrototype(JSContext* ctx, Impl& impl)
{
  static const std::array<JSCFunctionListEntry, 15> kMethods = {{
      JS_CFUNC_DEF("getAttribute", 1, ElementGetAttribute),
      JS_CFUNC_DEF("setAttribute", 2, ElementSetAttribute),
      JS_CFUNC_DEF("removeAttribute", 1, ElementRemoveAttribute),
      JS_CFUNC_DEF("hasAttribute", 1, ElementHasAttribute),
      JS_CFUNC_DEF("querySelector", 1, ElementQuerySelector),
      JS_CFUNC_DEF("querySelectorAll", 1, ElementQuerySelectorAll),
      JS_CFUNC_DEF("getElementsByTagName", 1, ElementGetElementsByTagName),
      JS_CFUNC_DEF("getElementsByClassName", 1, ElementGetElementsByClassName),
      JS_CFUNC_DEF("matches", 1, ElementMatches),
      JS_CFUNC_DEF("closest", 1, ElementClosest),
      JS_CFUNC_DEF("remove", 0, ElementRemove),
      JS_CFUNC_DEF("insertAdjacentHTML", 2, ElementInsertAdjacentHTML),
      JS_CFUNC_DEF("getBoundingClientRect", 0, ElementGetBoundingClientRect),
      JS_CFUNC_DEF("play", 0, ElementPlayVideo),
      JS_CFUNC_DEF("pause", 0, ElementPauseVideo),
  }};
  JS_SetPropertyFunctionList(
      ctx, impl.element_proto, kMethods.data(), static_cast<int>(kMethods.size()));

  DefineGetter(ctx, impl.element_proto, "tagName", MakeGetter(ctx, "tagName", ElementGetTagName));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "id",
                 MakeGetter(ctx, "id", ElementGetId),
                 MakeSetter(ctx, "id", ElementSetId));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "className",
                 MakeGetter(ctx, "className", ElementGetClassName),
                 MakeSetter(ctx, "className", ElementSetClassName));
  DefineGetter(
      ctx, impl.element_proto, "attributes", MakeGetter(ctx, "attributes", ElementGetAttributes));
  // Element layout geometry (getBoundingClientRect + offset/client series).
  DefineGetter(ctx,
               impl.element_proto,
               "offsetWidth",
               MakeGetter(ctx, "offsetWidth", ElementGetOffsetWidth));
  DefineGetter(ctx,
               impl.element_proto,
               "offsetHeight",
               MakeGetter(ctx, "offsetHeight", ElementGetOffsetHeight));
  DefineGetter(
      ctx, impl.element_proto, "offsetTop", MakeGetter(ctx, "offsetTop", ElementGetOffsetTop));
  DefineGetter(
      ctx, impl.element_proto, "offsetLeft", MakeGetter(ctx, "offsetLeft", ElementGetOffsetLeft));
  DefineGetter(ctx,
               impl.element_proto,
               "offsetParent",
               MakeGetter(ctx, "offsetParent", ElementGetOffsetParent));
  DefineGetter(ctx,
               impl.element_proto,
               "clientWidth",
               MakeGetter(ctx, "clientWidth", ElementGetClientWidth));
  DefineGetter(ctx,
               impl.element_proto,
               "clientHeight",
               MakeGetter(ctx, "clientHeight", ElementGetClientHeight));
  DefineGetter(
      ctx, impl.element_proto, "clientTop", MakeGetter(ctx, "clientTop", ElementGetClientTop));
  DefineGetter(
      ctx, impl.element_proto, "clientLeft", MakeGetter(ctx, "clientLeft", ElementGetClientLeft));
  // Element-level global event handler attributes (element.onclick = fn);
  // the table and accessors live in event_binding.cpp.
  DefineElementEventHandlers(ctx, impl);
  DefineGetter(
      ctx, impl.element_proto, "children", MakeGetter(ctx, "children", ElementGetChildren));
  DefineGetter(ctx,
               impl.element_proto,
               "firstElementChild",
               MakeGetter(ctx, "firstElementChild", ElementGetFirstElementChild));
  DefineGetter(ctx,
               impl.element_proto,
               "lastElementChild",
               MakeGetter(ctx, "lastElementChild", ElementGetLastElementChild));
  DefineGetter(ctx,
               impl.element_proto,
               "nextElementSibling",
               MakeGetter(ctx, "nextElementSibling", ElementGetNextElementSibling));
  DefineGetter(ctx,
               impl.element_proto,
               "previousElementSibling",
               MakeGetter(ctx, "previousElementSibling", ElementGetPreviousElementSibling));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "innerHTML",
                 MakeGetter(ctx, "innerHTML", ElementGetInnerHTML),
                 MakeSetter(ctx, "innerHTML", ElementSetInnerHTML));
  DefineGetter(
      ctx, impl.element_proto, "outerHTML", MakeGetter(ctx, "outerHTML", ElementGetOuterHTML));
  // innerText (read-only): an approximation returning the element's
  // textContent.  Real innerText reflects *rendered* text (hidden elements
  // excluded, whitespace normalized); the engine has no layout-backed innerText
  // yet, and bing reads innerText mainly to sniff page text during bootstrap.
  DefineGetter(
      ctx, impl.element_proto, "innerText", MakeGetter(ctx, "innerText", NodeGetTextContent));
  DefineGetter(ctx, impl.element_proto, "style", MakeGetter(ctx, "style", ElementGetStyle));
  DefineGetter(
      ctx, impl.element_proto, "classList", MakeGetter(ctx, "classList", ElementGetClassList));
  DefineGetter(ctx, impl.element_proto, "dataset", MakeGetter(ctx, "dataset", ElementGetDataset));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "hidden",
                 MakeGetter(ctx, "hidden", ElementGetHidden),
                 MakeSetter(ctx, "hidden", ElementSetHidden));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "title",
                 MakeGetter(ctx, "title", ElementGetTitle),
                 MakeSetter(ctx, "title", ElementSetTitle));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "lang",
                 MakeGetter(ctx, "lang", ElementGetLang),
                 MakeSetter(ctx, "lang", ElementSetLang));

  // Form controls (input/textarea/select/option/button).
  DefineAccessor(ctx,
                 impl.element_proto,
                 "value",
                 MakeGetter(ctx, "value", ElementGetValue),
                 MakeSetter(ctx, "value", ElementSetValue));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "checked",
                 MakeGetter(ctx, "checked", ElementGetChecked),
                 MakeSetter(ctx, "checked", ElementSetChecked));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "type",
                 MakeGetter(ctx, "type", ElementGetType),
                 MakeSetter(ctx, "type", ElementSetType));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "placeholder",
                 MakeGetter(ctx, "placeholder", ElementGetPlaceholder),
                 MakeSetter(ctx, "placeholder", ElementSetPlaceholder));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "disabled",
                 MakeGetter(ctx, "disabled", ElementGetDisabled),
                 MakeSetter(ctx, "disabled", ElementSetDisabled));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "name",
                 MakeGetter(ctx, "name", ElementGetName),
                 MakeSetter(ctx, "name", ElementSetName));
  // Links (<a>).
  DefineAccessor(ctx,
                 impl.element_proto,
                 "href",
                 MakeGetter(ctx, "href", ElementGetHref),
                 MakeSetter(ctx, "href", ElementSetHref));
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
  DefineAccessor(ctx,
                 impl.element_proto,
                 "target",
                 MakeGetter(ctx, "target", ElementGetTarget),
                 MakeSetter(ctx, "target", ElementSetTarget));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "rel",
                 MakeGetter(ctx, "rel", ElementGetRel),
                 MakeSetter(ctx, "rel", ElementSetRel));
  // Images (<img>) and script src.
  DefineAccessor(ctx,
                 impl.element_proto,
                 "src",
                 MakeGetter(ctx, "src", ElementGetSrc),
                 MakeSetter(ctx, "src", ElementSetSrc));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "currentSrc",
                 MakeGetter(ctx, "currentSrc", ElementGetCurrentSrc),
                 MakeSetter(ctx, "currentSrc", ElementSetSrc));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "alt",
                 MakeGetter(ctx, "alt", ElementGetAlt),
                 MakeSetter(ctx, "alt", ElementSetAlt));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "width",
                 MakeGetter(ctx, "width", ElementGetWidth),
                 MakeSetter(ctx, "width", ElementSetWidth));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "height",
                 MakeGetter(ctx, "height", ElementGetHeight),
                 MakeSetter(ctx, "height", ElementSetHeight));
  DefineGetter(ctx,
               impl.element_proto,
               "naturalWidth",
               MakeGetter(ctx, "naturalWidth", ElementGetNaturalWidth));
  DefineGetter(ctx,
               impl.element_proto,
               "naturalHeight",
               MakeGetter(ctx, "naturalHeight", ElementGetNaturalHeight));
  DefineGetter(
      ctx, impl.element_proto, "complete", MakeGetter(ctx, "complete", ElementGetComplete));
  // Media (<video>): duration/currentTime/paused + play()/pause() (methods
  // above).  currentTime is seekable; non-video elements report the media
  // defaults (NaN duration, paused = true).
  DefineGetter(
      ctx, impl.element_proto, "duration", MakeGetter(ctx, "duration", ElementGetVideoDuration));
  DefineAccessor(ctx,
                 impl.element_proto,
                 "currentTime",
                 MakeGetter(ctx, "currentTime", ElementGetVideoCurrentTime),
                 MakeSetter(ctx, "currentTime", ElementSetVideoCurrentTime));
  DefineGetter(ctx, impl.element_proto, "paused", MakeGetter(ctx, "paused", ElementGetVideoPaused));
}

void EnsureDatasetClassRegistered(JSRuntime* rt)
{
  std::lock_guard<std::mutex> lock(g_dataset_class_mutex);
  JS_NewClassID(rt, &g_dataset_class_id);
  if (g_dataset_class_registered.find(rt) == g_dataset_class_registered.end()) {
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
