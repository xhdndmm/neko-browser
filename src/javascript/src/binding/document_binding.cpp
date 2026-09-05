// neko::javascript DOM bindings — Document family.
//
// Part of the dom_binding split (see binding/binding_internal.h): the
// document callbacks (element factory, query, collections, URL/cookie/
// metadata getters), DOMImplementation subset, and DefineDocumentPrototype.

#include "neko/dom/element.h"
#include "neko/dom/node.h"
#include "neko/dom/query.h"
#include "neko/html/parser.h"

#include "binding_internal.h"

#include <quickjs.h>
#include <string>
#include <string_view>
#include <vector>

namespace neko::javascript {

JSValue
DOMParserConstructor(JSContext* ctx, JSValueConst new_target, int /*argc*/, JSValueConst* /*argv*/)
{
  JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
  if (JS_IsException(prototype)) {
    return prototype;
  }
  JSValue parser = JS_NewObjectProto(ctx, prototype);
  JS_FreeValue(ctx, prototype);
  return parser;
}

JSValue
DOMParserParseFromString(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, JS_UNDEFINED);
  if (impl == nullptr || argc < 2) {
    return JS_ThrowTypeError(ctx, "parseFromString requires input and mimeType");
  }
  bool input_ok = false;
  bool mime_ok = false;
  const std::string input = ArgString(ctx, argv[0], &input_ok);
  const std::string mime_type = ToLower(ArgString(ctx, argv[1], &mime_ok));
  if (!input_ok || !mime_ok) {
    return JS_EXCEPTION;
  }
  if (mime_type != "text/html") {
    return ThrowDomException(
        ctx, "NotSupportedError", "DOMParser currently supports only text/html");
  }
  std::unique_ptr<dom::Document> parsed = html::Parser(input).Parse();
  dom::Document* document = parsed.get();
  impl->TakeOwnership(document, std::move(parsed));
  return impl->WrapNode(document);
}

JSValue XMLSerializerConstructor(JSContext* ctx,
                                 JSValueConst new_target,
                                 int /*argc*/,
                                 JSValueConst* /*argv*/)
{
  JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
  if (JS_IsException(prototype)) {
    return prototype;
  }
  JSValue serializer = JS_NewObjectProto(ctx, prototype);
  JS_FreeValue(ctx, prototype);
  return serializer;
}

JSValue XMLSerializerSerializeToString(JSContext* ctx,
                                       JSValueConst /*this_val*/,
                                       int argc,
                                       JSValueConst* argv)
{
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "serializeToString requires a Node");
  }
  dom::Node* node = UnwrapNode(argv[0]);
  if (node == nullptr) {
    return JS_ThrowTypeError(ctx, "serializeToString argument is not a Node");
  }
  const std::string serialized = node->ToString();
  return JS_NewStringLen(ctx, serialized.data(), serialized.size());
}

JSValue DocGetDocumentElement(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr || node->node_type() != dom::NodeType::kDocument) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  return impl->WrapNode(static_cast<dom::Document*>(node)->document_element());
}

JSValue DocGetDoctype(JSContext* ctx, JSValueConst this_val)
{
  dom::Node* node = UnwrapNode(this_val);
  if (node == nullptr || node->node_type() != dom::NodeType::kDocument) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  return JS_NULL;
}

JSValue DocGetBody(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr || node->node_type() != dom::NodeType::kDocument) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  for (dom::Node* child : node->ChildNodes()) {
    if (dom::Element* el = AsElement(child)) {
      if (el->tag_name() == "body") {
        return impl->WrapNode(el);
      }
      for (dom::Node* inner : el->ChildNodes()) {
        if (dom::Element* in_el = AsElement(inner)) {
          if (in_el->tag_name() == "body") {
            return impl->WrapNode(in_el);
          }
        }
      }
    }
  }
  return JS_NULL;
}

JSValue DocGetHead(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr || node->node_type() != dom::NodeType::kDocument) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  dom::Element* head = FindElementByTag(*node, "head");
  return head != nullptr ? impl->WrapNode(head) : JS_NULL;
}

JSValue DocGetReadyState(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr || node->node_type() != dom::NodeType::kDocument) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  // Scripts run after parsing completes, so the document is always fully
  // loaded by the time any page script can observe it.
  return JS_NewString(ctx, "complete");
}

JSValue DocGetTitle(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr || node->node_type() != dom::NodeType::kDocument) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  const std::string title = static_cast<dom::Document*>(node)->Title();
  return JS_NewStringLen(ctx, title.data(), title.size());
}

JSValue DocumentImplementationHasFeature(JSContext* ctx,
                                         JSValueConst /*this_val*/,
                                         int argc,
                                         JSValueConst* argv)
{
  if (argc < 1) {
    return JS_FALSE;
  }
  bool ok = false;
  const std::string feature = ToLower(ArgString(ctx, argv[0], &ok));
  if (!ok) {
    return JS_EXCEPTION;
  }
  if (argc >= 2) {
    // |version| is accepted but not yet used to differentiate partial support.
    bool version_ok = false;
    (void)ArgString(ctx, argv[1], &version_ok);
  }
  return feature == "html" || feature == "dom" || feature == "core" || feature == "xml" ||
                 feature == "css" || feature == "css2" || feature == "html5"
             ? JS_TRUE
             : JS_FALSE;
}

JSValue DocumentImplementationCreateHTMLDocument(JSContext* ctx,
                                                 JSValueConst this_val,
                                                 int argc,
                                                 JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  bool ok = false;
  const std::string title = argc >= 1 ? ArgString(ctx, argv[0], &ok) : std::string();
  if (argc >= 1 && !ok) {
    return JS_EXCEPTION;
  }

  auto document = std::make_unique<dom::Document>();
  auto html = std::make_unique<dom::Element>("html");
  auto head = std::make_unique<dom::Element>("head");
  auto body = std::make_unique<dom::Element>("body");
  if (!title.empty()) {
    auto title_el = std::make_unique<dom::Element>("title");
    title_el->AppendChild(std::make_unique<dom::Text>(title));
    head->AppendChild(std::move(title_el));
  }
  html->AppendChild(std::move(head));
  html->AppendChild(std::move(body));
  document->AppendChild(std::move(html));

  dom::Document* raw = document.get();
  impl->created[raw] = std::move(document);
  return impl->WrapNode(raw);
}

JSValue DocGetImplementation(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr || node->node_type() != dom::NodeType::kDocument) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  if (JS_IsUndefined(impl->document_implementation)) {
    impl->document_implementation = JS_NewObjectProto(ctx, impl->dom_implementation_proto);
  }
  return JS_DupValue(ctx, impl->document_implementation);
}

JSValue DocSetTitle(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr || node->node_type() != dom::NodeType::kDocument) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  bool ok = false;
  const std::string title = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  // Find the existing <title> element (any depth); create one in <head> when
  // missing (or leave it detached when the document has no <head> yet).
  dom::Element* title_el = FindElementByTag(*node, "title");
  if (title_el == nullptr) {
    auto new_title = std::make_unique<dom::Element>("title");
    title_el = new_title.get();
    dom::Element* head = FindElementByTag(*node, "head");
    if (head != nullptr) {
      head->AppendChild(std::move(new_title));
    } else {
      // No head: keep a detached owned element (title still reports).
      impl->created[title_el] = std::move(new_title);
    }
  }
  while (title_el->first_child() != nullptr) {
    std::unique_ptr<dom::Node> removed = title_el->RemoveChild(title_el->first_child());
    impl->TakeOwnership(removed.get(), std::move(removed));
  }
  if (!title.empty()) {
    title_el->AppendChild(std::make_unique<dom::Text>(title));
  }
  return JS_UNDEFINED;
}

dom::Element* FindById(const dom::Node& root, std::string_view id)
{
  for (dom::Node* child : root.ChildNodes()) {
    if (dom::Element* el = AsElement(child)) {
      const std::optional<std::string_view> el_id = el->Id();
      if (el_id.has_value() && el_id.value() == id) {
        return el;
      }
      if (dom::Element* found = FindById(*el, id)) {
        return found;
      }
    }
  }
  return nullptr;
}

// First element with the given tag name in pre-order, or nullptr.
dom::Element* FindElementByTag(const dom::Node& root, std::string_view tag)
{
  for (dom::Node* child : root.ChildNodes()) {
    if (dom::Element* el = AsElement(child)) {
      if (el->tag_name() == tag) {
        return el;
      }
      if (dom::Element* found = FindElementByTag(*el, tag)) {
        return found;
      }
    }
  }
  return nullptr;
}

JSValue DocGetElementById(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  if (argc < 1) {
    return JS_NULL;
  }
  bool ok = false;
  const std::string id = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  return impl->WrapNode(FindById(*node, id));
}

JSValue DocCreateElement(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "createElement requires a tag name");
  }
  bool ok = false;
  const std::string tag = ToLower(ArgString(ctx, argv[0], &ok));
  if (!ok) {
    return JS_EXCEPTION;
  }
  if (tag.empty()) {
    return JS_ThrowTypeError(ctx, "createElement: empty tag name");
  }
  auto element = std::make_unique<dom::Element>(tag);
  dom::Element* raw = element.get();
  impl->created[raw] = std::move(element);
  return impl->WrapNode(raw);
}

JSValue DocCreateTextNode(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  bool ok = false;
  const std::string text = argc >= 1 ? ArgString(ctx, argv[0], &ok) : std::string();
  if (!ok) {
    return JS_EXCEPTION;
  }
  auto text_node = std::make_unique<dom::Text>(text);
  dom::Text* raw = text_node.get();
  impl->created[raw] = std::move(text_node);
  return impl->WrapNode(raw);
}

JSValue DocQuerySelector(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
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

JSValue DocQuerySelectorAll(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
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

JSValue DocCreateDocumentFragment(JSContext* ctx,
                                  JSValueConst this_val,
                                  int /*argc*/,
                                  JSValueConst* /*argv*/)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  auto fragment = std::make_unique<dom::DocumentFragment>();
  dom::DocumentFragment* raw = fragment.get();
  impl->created[raw] = std::move(fragment);
  return impl->WrapNode(raw);
}

uint32_t NodeShowMask(dom::NodeType type)
{
  switch (type) {
  case dom::NodeType::kElement:
    return 0x1;
  case dom::NodeType::kText:
    return 0x4;
  case dom::NodeType::kComment:
    return 0x80;
  case dom::NodeType::kDocument:
    return 0x100;
  case dom::NodeType::kDocumentFragment:
    return 0x400;
  }
  return 0;
}

dom::Node* NextTreeNode(dom::Node* node, dom::Node* root)
{
  const auto children = node->ChildNodes();
  if (children.begin() != children.end()) {
    return *children.begin();
  }
  while (node != root) {
    if (dom::Node* sibling = SiblingOf(node, +1)) {
      return sibling;
    }
    node = node->parent();
  }
  return nullptr;
}

JSValue DocCreateTreeWalker(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* root = argc >= 1 ? UnwrapNode(argv[0]) : nullptr;
  if (impl == nullptr || root == nullptr) {
    return JS_ThrowTypeError(ctx, "createTreeWalker requires a root Node");
  }
  uint32_t what_to_show = 0xFFFFFFFFU;
  if (argc >= 2 && !JS_IsUndefined(argv[1]) && JS_ToUint32(ctx, &what_to_show, argv[1]) < 0) {
    return JS_EXCEPTION;
  }
  JSValue walker = JS_NewObjectProto(ctx, impl->tree_walker_proto);
  JS_SetPropertyStr(ctx, walker, "root", impl->WrapNode(root));
  JS_SetPropertyStr(ctx, walker, "currentNode", impl->WrapNode(root));
  JS_SetPropertyStr(ctx, walker, "_whatToShow", JS_NewUint32(ctx, what_to_show));
  return walker;
}

JSValue RangeGetBoundaryContainer(JSContext* ctx, JSValueConst this_val, const char* property)
{
  JSValue container = JS_GetPropertyStr(ctx, this_val, property);
  if (JS_IsUndefined(container)) {
    JS_FreeValue(ctx, container);
    return JS_ThrowTypeError(ctx, "not a Range");
  }
  return container;
}

JSValue RangeGetStartContainer(JSContext* ctx, JSValueConst this_val)
{
  return RangeGetBoundaryContainer(ctx, this_val, "__nekoRangeStartContainer");
}

JSValue RangeGetEndContainer(JSContext* ctx, JSValueConst this_val)
{
  return RangeGetBoundaryContainer(ctx, this_val, "__nekoRangeEndContainer");
}

JSValue RangeGetBoundaryOffset(JSContext* ctx, JSValueConst this_val, const char* property)
{
  JSValue offset = JS_GetPropertyStr(ctx, this_val, property);
  if (JS_IsUndefined(offset)) {
    JS_FreeValue(ctx, offset);
    return JS_ThrowTypeError(ctx, "not a Range");
  }
  return offset;
}

JSValue RangeGetStartOffset(JSContext* ctx, JSValueConst this_val)
{
  return RangeGetBoundaryOffset(ctx, this_val, "__nekoRangeStartOffset");
}

JSValue RangeGetEndOffset(JSContext* ctx, JSValueConst this_val)
{
  return RangeGetBoundaryOffset(ctx, this_val, "__nekoRangeEndOffset");
}

JSValue RangeGetCommonAncestorContainer(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  JSValue start_value = JS_GetPropertyStr(ctx, this_val, "__nekoRangeStartContainer");
  JSValue end_value = JS_GetPropertyStr(ctx, this_val, "__nekoRangeEndContainer");
  dom::Node* start = UnwrapNode(start_value);
  dom::Node* end = UnwrapNode(end_value);
  JS_FreeValue(ctx, start_value);
  JS_FreeValue(ctx, end_value);
  if (impl == nullptr || start == nullptr || end == nullptr) {
    return JS_ThrowTypeError(ctx, "not a Range");
  }

  for (dom::Node* ancestor = start; ancestor != nullptr; ancestor = ancestor->parent()) {
    for (dom::Node* candidate = end; candidate != nullptr; candidate = candidate->parent()) {
      if (ancestor == candidate) {
        return impl->WrapNode(ancestor);
      }
    }
  }
  return JS_ThrowTypeError(ctx, "Range boundary containers have no common ancestor");
}

JSValue DocCreateRange(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* document = UnwrapNode(this_val);
  if (impl == nullptr || document == nullptr || document->node_type() != dom::NodeType::kDocument) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  JSValue range = JS_NewObjectProto(ctx, impl->range_proto);
  JS_DefinePropertyValueStr(
      ctx, range, "__nekoRangeStartContainer", JS_DupValue(ctx, this_val), JS_PROP_CONFIGURABLE);
  JS_DefinePropertyValueStr(
      ctx, range, "__nekoRangeEndContainer", JS_DupValue(ctx, this_val), JS_PROP_CONFIGURABLE);
  JS_DefinePropertyValueStr(
      ctx, range, "__nekoRangeStartOffset", JS_NewInt32(ctx, 0), JS_PROP_CONFIGURABLE);
  JS_DefinePropertyValueStr(
      ctx, range, "__nekoRangeEndOffset", JS_NewInt32(ctx, 0), JS_PROP_CONFIGURABLE);
  return range;
}

JSValue
TreeWalkerNextNode(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  JSValue root_value = JS_GetPropertyStr(ctx, this_val, "root");
  JSValue current_value = JS_GetPropertyStr(ctx, this_val, "currentNode");
  JSValue mask_value = JS_GetPropertyStr(ctx, this_val, "_whatToShow");
  dom::Node* root = UnwrapNode(root_value);
  dom::Node* current = UnwrapNode(current_value);
  uint32_t what_to_show = 0;
  const int mask_result = JS_ToUint32(ctx, &what_to_show, mask_value);
  JS_FreeValue(ctx, mask_value);
  JS_FreeValue(ctx, current_value);
  JS_FreeValue(ctx, root_value);
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || root == nullptr || current == nullptr || mask_result < 0) {
    return JS_ThrowTypeError(ctx, "invalid TreeWalker");
  }
  for (dom::Node* candidate = NextTreeNode(current, root); candidate != nullptr;
       candidate = NextTreeNode(candidate, root)) {
    if ((what_to_show & NodeShowMask(candidate->node_type())) == 0) {
      continue;
    }
    JSValue wrapped = impl->WrapNode(candidate);
    JSValue walker = JS_DupValue(ctx, this_val);
    JS_SetPropertyStr(ctx, walker, "currentNode", JS_DupValue(ctx, wrapped));
    JS_FreeValue(ctx, walker);
    return wrapped;
  }
  return JS_NULL;
}

JSValue DocCreateComment(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  bool ok = false;
  const std::string text = argc >= 1 ? ArgString(ctx, argv[0], &ok) : std::string();
  if (!ok) {
    return JS_EXCEPTION;
  }
  auto comment = std::make_unique<dom::Comment>(text);
  dom::Comment* raw = comment.get();
  impl->created[raw] = std::move(comment);
  return impl->WrapNode(raw);
}

JSValue DocCreateElementNS(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  if (argc < 2) {
    return JS_ThrowTypeError(ctx, "createElementNS requires (namespace, tagName)");
  }
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  bool ok = false;
  const std::string namespace_uri = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  const std::string tag = ArgString(ctx, argv[1], &ok);
  if (!ok || tag.empty()) {
    return JS_ThrowTypeError(ctx, "createElementNS requires a tag name");
  }
  auto element = std::make_unique<dom::Element>(tag, namespace_uri);
  dom::Element* raw = element.get();
  impl->created[raw] = std::move(element);
  return impl->WrapNode(raw);
}

// All elements with the given tag name, in document order.
std::vector<dom::Element*> CollectByTag(const dom::Node& root, std::string_view tag)
{
  std::vector<dom::Element*> out;
  for (dom::Node* child : root.ChildNodes()) {
    if (dom::Element* el = AsElement(child)) {
      if (el->tag_name() == tag) {
        out.push_back(el);
      }
      std::vector<dom::Element*> nested = CollectByTag(*el, tag);
      out.insert(out.end(), nested.begin(), nested.end());
    }
  }
  return out;
}

// All elements carrying |cls| (exact token match) in document order.
std::vector<dom::Element*> CollectByClass(const dom::Node& root, std::string_view cls)
{
  std::vector<dom::Element*> out;
  for (dom::Node* child : root.ChildNodes()) {
    if (dom::Element* el = AsElement(child)) {
      for (const std::string_view token : el->ClassList()) {
        if (token == cls) {
          out.push_back(el);
          break;
        }
      }
      std::vector<dom::Element*> nested = CollectByClass(*el, cls);
      out.insert(out.end(), nested.begin(), nested.end());
    }
  }
  return out;
}

JSValue DocGetElementsByTagName(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  if (argc < 1) {
    return impl->MakeElementArray({});
  }
  bool ok = false;
  const std::string tag = ToLower(ArgString(ctx, argv[0], &ok));
  if (!ok) {
    return JS_EXCEPTION;
  }
  // Live: re-queries the whole document on every JS DOM mutation.
  return impl->MakeLiveCollection(node, Impl::LiveKind::kTagName, tag);
}

JSValue
DocGetElementsByClassName(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  if (argc < 1) {
    return impl->MakeElementArray({});
  }
  bool ok = false;
  const std::string cls = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  // Live: re-queries the whole document on every JS DOM mutation.
  return impl->MakeLiveCollection(node, Impl::LiveKind::kClassName, cls);
}

// The current document URL (from the PageApis location callback when wired).
std::string DocumentUrl(const Impl& impl)
{
  if (impl.apis.location_href) {
    const std::string url = impl.apis.location_href();
    if (!url.empty()) {
      return url;
    }
  }
  return std::string();
}

JSValue DocGetURL(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  const std::string url = DocumentUrl(*impl);
  return JS_NewStringLen(ctx, url.data(), url.size());
}

JSValue DocGetCookie(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.cookie_get) {
    return JS_NewStringLen(ctx, "", 0);
  }
  const std::string cookies = impl->apis.cookie_get();
  return JS_NewStringLen(ctx, cookies.data(), cookies.size());
}

JSValue DocSetCookie(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.cookie_set) {
    return JS_UNDEFINED;
  }
  bool ok = false;
  const std::string assignment = ArgString(ctx, value, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  impl->apis.cookie_set(assignment);
  return JS_UNDEFINED;
}

JSValue DocGetBaseURI(JSContext* ctx, JSValueConst this_val)
{
  return DocGetURL(ctx, this_val);
}

JSValue DocGetDefaultView(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  return JS_DupValue(ctx, impl->window);
}

JSValue DocGetDocumentURI(JSContext* ctx, JSValueConst this_val)
{
  return DocGetURL(ctx, this_val);
}

JSValue DocGetCharacterSet(JSContext* ctx, JSValueConst this_val)
{
  if (ImplFor(ctx, this_val) == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  return JS_NewString(ctx, "UTF-8");
}

JSValue DocGetContentType(JSContext* ctx, JSValueConst this_val)
{
  if (ImplFor(ctx, this_val) == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  return JS_NewString(ctx, "text/html");
}

JSValue DocGetReferrer(JSContext* ctx, JSValueConst this_val)
{
  if (ImplFor(ctx, this_val) == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  return JS_NewString(ctx, "");
}

JSValue DocGetForms(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  return impl->MakeLiveCollection(node, Impl::LiveKind::kForms, "");
}

JSValue DocGetImages(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  return impl->MakeLiveCollection(node, Impl::LiveKind::kImages, "");
}

JSValue DocGetScripts(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  return impl->MakeLiveCollection(node, Impl::LiveKind::kScripts, "");
}

JSValue DocGetCurrentScript(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  // The element whose classic script body is currently executing (WHATWG
  // HTML §4.12.1), or null outside of script execution.  Bundlers (e.g.
  // umi/utoo) read this at entry time to locate their chunk manifest.
  return impl->WrapNode(impl->current_script);
}

JSValue DocGetLinks(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(this_val);
  if (impl == nullptr || node == nullptr) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  // document.links: live collection of <a>/<area> elements with an href.
  return impl->MakeLiveCollection(node, Impl::LiveKind::kLinks, "");
}

JSValue DocWrite(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  // Scripts run after parsing, so document.write cannot replace the input
  // stream here.  Keep the legacy entry point callable until parser-time
  // document rewriting is implemented.
  for (int i = 0; i < argc; ++i) {
    bool ok = false;
    (void)ArgString(ctx, argv[i], &ok);
    if (!ok) {
      return JS_EXCEPTION;
    }
  }
  return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// CSSStyleDeclaration methods and accessors.

void DefineDocumentPrototype(JSContext* ctx, Impl& impl)
{
  static const std::array<JSCFunctionListEntry, 14> kMethods = {{
      JS_CFUNC_DEF("getElementById", 1, DocGetElementById),
      JS_CFUNC_DEF("createElement", 1, DocCreateElement),
      JS_CFUNC_DEF("createElementNS", 2, DocCreateElementNS),
      JS_CFUNC_DEF("createTextNode", 1, DocCreateTextNode),
      JS_CFUNC_DEF("createDocumentFragment", 0, DocCreateDocumentFragment),
      JS_CFUNC_DEF("createTreeWalker", 1, DocCreateTreeWalker),
      JS_CFUNC_DEF("createRange", 0, DocCreateRange),
      JS_CFUNC_DEF("createComment", 1, DocCreateComment),
      JS_CFUNC_DEF("querySelector", 1, DocQuerySelector),
      JS_CFUNC_DEF("querySelectorAll", 1, DocQuerySelectorAll),
      JS_CFUNC_DEF("getElementsByTagName", 1, DocGetElementsByTagName),
      JS_CFUNC_DEF("getElementsByClassName", 1, DocGetElementsByClassName),
      JS_CFUNC_DEF("write", 1, DocWrite),
      JS_CFUNC_DEF("writeln", 1, DocWrite),
  }};
  JS_SetPropertyFunctionList(
      ctx, impl.document_proto, kMethods.data(), static_cast<int>(kMethods.size()));

  DefineGetter(ctx,
               impl.range_proto,
               "startContainer",
               MakeGetter(ctx, "startContainer", RangeGetStartContainer));
  DefineGetter(
      ctx, impl.range_proto, "startOffset", MakeGetter(ctx, "startOffset", RangeGetStartOffset));
  DefineGetter(
      ctx, impl.range_proto, "endContainer", MakeGetter(ctx, "endContainer", RangeGetEndContainer));
  DefineGetter(ctx, impl.range_proto, "endOffset", MakeGetter(ctx, "endOffset", RangeGetEndOffset));
  DefineGetter(ctx,
               impl.range_proto,
               "commonAncestorContainer",
               MakeGetter(ctx, "commonAncestorContainer", RangeGetCommonAncestorContainer));

  DefineGetter(ctx,
               impl.document_proto,
               "implementation",
               MakeGetter(ctx, "implementation", DocGetImplementation));
  DefineGetter(ctx,
               impl.document_proto,
               "documentElement",
               MakeGetter(ctx, "documentElement", DocGetDocumentElement));
  DefineGetter(ctx, impl.document_proto, "doctype", MakeGetter(ctx, "doctype", DocGetDoctype));
  DefineGetter(ctx, impl.document_proto, "body", MakeGetter(ctx, "body", DocGetBody));
  DefineGetter(ctx, impl.document_proto, "head", MakeGetter(ctx, "head", DocGetHead));
  DefineGetter(
      ctx, impl.document_proto, "readyState", MakeGetter(ctx, "readyState", DocGetReadyState));
  DefineAccessor(ctx,
                 impl.document_proto,
                 "title",
                 MakeGetter(ctx, "title", DocGetTitle),
                 MakeSetter(ctx, "title", DocSetTitle));
  DefineGetter(ctx, impl.document_proto, "URL", MakeGetter(ctx, "URL", DocGetURL));
  DefineAccessor(ctx,
                 impl.document_proto,
                 "cookie",
                 MakeGetter(ctx, "cookie", DocGetCookie),
                 MakeSetter(ctx, "cookie", DocSetCookie));
  DefineGetter(
      ctx, impl.document_proto, "defaultView", MakeGetter(ctx, "defaultView", DocGetDefaultView));
  DefineGetter(ctx, impl.document_proto, "baseURI", MakeGetter(ctx, "baseURI", DocGetBaseURI));
  DefineGetter(
      ctx, impl.document_proto, "documentURI", MakeGetter(ctx, "documentURI", DocGetDocumentURI));
  DefineGetter(ctx,
               impl.document_proto,
               "characterSet",
               MakeGetter(ctx, "characterSet", DocGetCharacterSet));
  DefineGetter(
      ctx, impl.document_proto, "contentType", MakeGetter(ctx, "contentType", DocGetContentType));
  DefineGetter(ctx, impl.document_proto, "referrer", MakeGetter(ctx, "referrer", DocGetReferrer));
  DefineGetter(ctx, impl.document_proto, "forms", MakeGetter(ctx, "forms", DocGetForms));
  DefineGetter(ctx, impl.document_proto, "images", MakeGetter(ctx, "images", DocGetImages));
  DefineGetter(ctx, impl.document_proto, "links", MakeGetter(ctx, "links", DocGetLinks));
  DefineGetter(ctx, impl.document_proto, "scripts", MakeGetter(ctx, "scripts", DocGetScripts));
  DefineGetter(ctx,
               impl.document_proto,
               "currentScript",
               MakeGetter(ctx, "currentScript", DocGetCurrentScript));
}

} // namespace neko::javascript
