// GENERATED FILE -- DO NOT EDIT.
// Source: src/javascript/webidl/selection_binding.webidl
// Generator: tools/webidl/gen.py (WebIDL subset; see that file for the grammar).
// Regenerate: python3 tools/webidl/gen.py

// Included by the hand-written binding file, inside its namespace:
// the member implementations are file-local and must stay visible.

#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr JSCFunctionListEntry kSelectionBindingSelectionProtoMethods[] = {
    JS_CFUNC_DEF("getRangeAt", 1, SelectionGetRangeAt),
    JS_CFUNC_DEF("addRange", 1, SelectionAddRange),
    JS_CFUNC_DEF("removeRange", 1, SelectionRemoveRange),
    JS_CFUNC_DEF("removeAllRanges", 0, SelectionRemoveAllRanges),
    JS_CFUNC_DEF("empty", 0, SelectionRemoveAllRanges),
    JS_CFUNC_DEF("collapse", 0, SelectionCollapse),
    JS_CFUNC_DEF("collapseToStart", 0, SelectionCollapseToStart),
    JS_CFUNC_DEF("collapseToEnd", 0, SelectionCollapseToEnd),
    JS_CFUNC_DEF("extend", 1, SelectionExtend),
    JS_CFUNC_DEF("setBaseAndExtent", 4, SelectionSetBaseAndExtent),
    JS_CFUNC_DEF("selectAllChildren", 1, SelectionSelectAllChildren),
    JS_CFUNC_DEF("deleteFromDocument", 0, SelectionDeleteFromDocument),
    JS_CFUNC_DEF("containsNode", 1, SelectionContainsNode),
    JS_CFUNC_DEF("toString", 0, SelectionToString),
};

} // namespace

void DefineGeneratedSelectionBindingSelectionProto(JSContext* ctx, Impl& impl)
{
  JS_SetPropertyFunctionList(ctx,
                             impl.selection_proto,
                             kSelectionBindingSelectionProtoMethods,
                             static_cast<int>(std::size(kSelectionBindingSelectionProtoMethods)));
  DefineGetter(ctx,
               impl.selection_proto,
               "anchorNode",
               MakeGetter(ctx, "anchorNode", SelectionGetAnchorNode));
  DefineGetter(ctx,
               impl.selection_proto,
               "anchorOffset",
               MakeGetter(ctx, "anchorOffset", SelectionGetAnchorOffset));
  DefineGetter(
      ctx, impl.selection_proto, "focusNode", MakeGetter(ctx, "focusNode", SelectionGetFocusNode));
  DefineGetter(ctx,
               impl.selection_proto,
               "focusOffset",
               MakeGetter(ctx, "focusOffset", SelectionGetFocusOffset));
  DefineGetter(ctx,
               impl.selection_proto,
               "isCollapsed",
               MakeGetter(ctx, "isCollapsed", SelectionGetIsCollapsed));
  DefineGetter(ctx,
               impl.selection_proto,
               "rangeCount",
               MakeGetter(ctx, "rangeCount", SelectionGetRangeCount));
  DefineGetter(ctx, impl.selection_proto, "type", MakeGetter(ctx, "type", SelectionGetType));
}
