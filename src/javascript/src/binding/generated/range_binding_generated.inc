// GENERATED FILE -- DO NOT EDIT.
// Source: src/javascript/webidl/range_binding.webidl
// Generator: tools/webidl/gen.py (WebIDL subset; see that file for the grammar).
// Regenerate: python3 tools/webidl/gen.py

// Included by the hand-written binding file, inside its namespace:
// the member implementations are file-local and must stay visible.

#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr JSCFunctionListEntry kRangeBindingRangeProtoMethods[] = {
    JS_CFUNC_DEF("setStart", 2, RangeSetStart),
    JS_CFUNC_DEF("setEnd", 2, RangeSetEnd),
    JS_CFUNC_DEF("setStartBefore", 1, RangeSetStartBefore),
    JS_CFUNC_DEF("setStartAfter", 1, RangeSetStartAfter),
    JS_CFUNC_DEF("setEndBefore", 1, RangeSetEndBefore),
    JS_CFUNC_DEF("setEndAfter", 1, RangeSetEndAfter),
    JS_CFUNC_DEF("collapse", 0, RangeCollapse),
    JS_CFUNC_DEF("selectNode", 1, RangeSelectNode),
    JS_CFUNC_DEF("selectNodeContents", 1, RangeSelectNodeContents),
    JS_CFUNC_DEF("compareBoundaryPoints", 2, RangeCompareBoundaryPoints),
    JS_CFUNC_DEF("deleteContents", 0, RangeDeleteContents),
    JS_CFUNC_DEF("extractContents", 0, RangeExtractContents),
    JS_CFUNC_DEF("cloneContents", 0, RangeCloneContents),
    JS_CFUNC_DEF("insertNode", 1, RangeInsertNode),
    JS_CFUNC_DEF("surroundContents", 1, RangeSurroundContents),
    JS_CFUNC_DEF("cloneRange", 0, RangeCloneRange),
    JS_CFUNC_DEF("detach", 0, RangeDetach),
    JS_CFUNC_DEF("isPointInRange", 2, RangeIsPointInRange),
    JS_CFUNC_DEF("comparePoint", 2, RangeComparePoint),
    JS_CFUNC_DEF("intersectsNode", 1, RangeIntersectsNode),
    JS_CFUNC_DEF("toString", 0, RangeToString),
    JS_CFUNC_DEF("getBoundingClientRect", 0, RangeGetBoundingClientRect),
    JS_CFUNC_DEF("getClientRects", 0, RangeGetClientRects),
};

} // namespace

void DefineGeneratedRangeBindingRangeProto(JSContext* ctx, Impl& impl)
{
  JS_SetPropertyFunctionList(ctx,
                             impl.range_proto,
                             kRangeBindingRangeProtoMethods,
                             static_cast<int>(std::size(kRangeBindingRangeProtoMethods)));
  DefineGetter(ctx,
               impl.range_proto,
               "startContainer",
               MakeGetter(ctx, "startContainer", RangeGetStartContainer));
  DefineGetter(
      ctx, impl.range_proto, "startOffset", MakeGetter(ctx, "startOffset", RangeGetStartOffset));
  DefineGetter(
      ctx, impl.range_proto, "endContainer", MakeGetter(ctx, "endContainer", RangeGetEndContainer));
  DefineGetter(ctx, impl.range_proto, "endOffset", MakeGetter(ctx, "endOffset", RangeGetEndOffset));
  DefineGetter(ctx, impl.range_proto, "collapsed", MakeGetter(ctx, "collapsed", RangeGetCollapsed));
  DefineGetter(ctx,
               impl.range_proto,
               "commonAncestorContainer",
               MakeGetter(ctx, "commonAncestorContainer", RangeGetCommonAncestorContainer));
}
