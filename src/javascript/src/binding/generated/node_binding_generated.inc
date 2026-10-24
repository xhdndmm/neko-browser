// GENERATED FILE -- DO NOT EDIT.
// Source: src/javascript/webidl/node_binding.webidl
// Generator: tools/webidl/gen.py (WebIDL subset; see that file for the grammar).
// Regenerate: python3 tools/webidl/gen.py

// Included by the hand-written binding file, inside its namespace:
// the member implementations are file-local and must stay visible.

#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr JSCFunctionListEntry kNodeBindingNodeProtoMethods[] = {
    JS_CFUNC_DEF("appendChild", 1, NodeAppendChild),
    JS_CFUNC_DEF("append", 0, NodeAppend),
    JS_CFUNC_DEF("prepend", 0, NodePrepend),
    JS_CFUNC_DEF("replaceChildren", 0, NodeReplaceChildren),
    JS_CFUNC_DEF("before", 0, NodeBefore),
    JS_CFUNC_DEF("after", 0, NodeAfter),
    JS_CFUNC_DEF("replaceWith", 0, NodeReplaceWith),
    JS_CFUNC_DEF("insertBefore", 2, NodeInsertBefore),
    JS_CFUNC_DEF("replaceChild", 2, NodeReplaceChild),
    JS_CFUNC_DEF("removeChild", 1, NodeRemoveChild),
    JS_CFUNC_DEF("hasChildNodes", 0, NodeHasChildNodes),
    JS_CFUNC_DEF("cloneNode", 1, NodeCloneNode),
    JS_CFUNC_DEF("contains", 1, NodeContains),
    JS_CFUNC_DEF("normalize", 0, NodeNormalize),
    JS_CFUNC_DEF("addEventListener", 2, NodeAddEventListener),
    JS_CFUNC_DEF("removeEventListener", 2, NodeRemoveEventListener),
    JS_CFUNC_DEF("dispatchEvent", 1, NodeDispatchEvent),
};

} // namespace

void DefineGeneratedNodeBindingNodeProto(JSContext* ctx, Impl& impl)
{
  JS_SetPropertyFunctionList(ctx,
                             impl.node_proto,
                             kNodeBindingNodeProtoMethods,
                             static_cast<int>(std::size(kNodeBindingNodeProtoMethods)));
  DefineGetter(ctx, impl.node_proto, "nodeType", MakeGetter(ctx, "nodeType", NodeGetNodeType));
  DefineGetter(ctx, impl.node_proto, "nodeName", MakeGetter(ctx, "nodeName", NodeGetNodeName));
  DefineGetter(ctx, impl.node_proto, "baseURI", MakeGetter(ctx, "baseURI", NodeGetBaseURI));
  DefineAccessor(ctx,
                 impl.node_proto,
                 "textContent",
                 MakeGetter(ctx, "textContent", NodeGetTextContent),
                 MakeSetter(ctx, "textContent", NodeSetTextContent));
  DefineGetter(
      ctx, impl.node_proto, "parentNode", MakeGetter(ctx, "parentNode", NodeGetParentNode));
  DefineGetter(ctx,
               impl.node_proto,
               "parentElement",
               MakeGetter(ctx, "parentElement", NodeGetParentElement));
  DefineGetter(
      ctx, impl.node_proto, "firstChild", MakeGetter(ctx, "firstChild", NodeGetFirstChild));
  DefineGetter(ctx, impl.node_proto, "lastChild", MakeGetter(ctx, "lastChild", NodeGetLastChild));
  DefineGetter(
      ctx, impl.node_proto, "childNodes", MakeGetter(ctx, "childNodes", NodeGetChildNodes));
  DefineGetter(
      ctx, impl.node_proto, "nextSibling", MakeGetter(ctx, "nextSibling", NodeGetNextSibling));
  DefineGetter(ctx,
               impl.node_proto,
               "previousSibling",
               MakeGetter(ctx, "previousSibling", NodeGetPreviousSibling));
  DefineGetter(ctx,
               impl.node_proto,
               "ownerDocument",
               MakeGetter(ctx, "ownerDocument", NodeGetOwnerDocument));
  DefineGetter(
      ctx, impl.node_proto, "isConnected", MakeGetter(ctx, "isConnected", NodeGetIsConnected));
  DefineAccessor(ctx,
                 impl.node_proto,
                 "nodeValue",
                 MakeGetter(ctx, "nodeValue", NodeGetNodeValue),
                 MakeSetter(ctx, "nodeValue", NodeSetNodeValue));
}
