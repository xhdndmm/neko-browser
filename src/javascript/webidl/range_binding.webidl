// Binding surface extracted from src/javascript/src/binding/range_binding.cpp.
// Function bodies stay hand-written; this file owns the registration.

[Proto=range_proto]
interface RangeBindingRangeProto {
  [Getter=RangeGetStartContainer] readonly attribute any startContainer;
  [Getter=RangeGetStartOffset] readonly attribute any startOffset;
  [Getter=RangeGetEndContainer] readonly attribute any endContainer;
  [Getter=RangeGetEndOffset] readonly attribute any endOffset;
  [Getter=RangeGetCollapsed] readonly attribute any collapsed;
  [Getter=RangeGetCommonAncestorContainer] readonly attribute any commonAncestorContainer;
  [CFunction=RangeSetStart] void setStart(any a0, any a1);
  [CFunction=RangeSetEnd] void setEnd(any a0, any a1);
  [CFunction=RangeSetStartBefore] void setStartBefore(any a0);
  [CFunction=RangeSetStartAfter] void setStartAfter(any a0);
  [CFunction=RangeSetEndBefore] void setEndBefore(any a0);
  [CFunction=RangeSetEndAfter] void setEndAfter(any a0);
  [CFunction=RangeCollapse] void collapse();
  [CFunction=RangeSelectNode] void selectNode(any a0);
  [CFunction=RangeSelectNodeContents] void selectNodeContents(any a0);
  [CFunction=RangeCompareBoundaryPoints] void compareBoundaryPoints(any a0, any a1);
  [CFunction=RangeDeleteContents] void deleteContents();
  [CFunction=RangeExtractContents] void extractContents();
  [CFunction=RangeCloneContents] void cloneContents();
  [CFunction=RangeInsertNode] void insertNode(any a0);
  [CFunction=RangeSurroundContents] void surroundContents(any a0);
  [CFunction=RangeCloneRange] void cloneRange();
  [CFunction=RangeDetach] void detach();
  [CFunction=RangeIsPointInRange] void isPointInRange(any a0, any a1);
  [CFunction=RangeComparePoint] void comparePoint(any a0, any a1);
  [CFunction=RangeIntersectsNode] void intersectsNode(any a0);
  [CFunction=RangeToString] void toString();
  [CFunction=RangeGetBoundingClientRect] void getBoundingClientRect();
  [CFunction=RangeGetClientRects] void getClientRects();
};
