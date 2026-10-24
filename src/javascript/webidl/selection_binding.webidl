// Binding surface extracted from src/javascript/src/binding/selection_binding.cpp.
// Function bodies stay hand-written; this file owns the registration.

[Proto=selection_proto]
interface SelectionBindingSelectionProto {
  [Getter=SelectionGetAnchorNode] readonly attribute any anchorNode;
  [Getter=SelectionGetAnchorOffset] readonly attribute any anchorOffset;
  [Getter=SelectionGetFocusNode] readonly attribute any focusNode;
  [Getter=SelectionGetFocusOffset] readonly attribute any focusOffset;
  [Getter=SelectionGetIsCollapsed] readonly attribute any isCollapsed;
  [Getter=SelectionGetRangeCount] readonly attribute any rangeCount;
  [Getter=SelectionGetType] readonly attribute any type;
  [CFunction=SelectionGetRangeAt] void getRangeAt(any a0);
  [CFunction=SelectionAddRange] void addRange(any a0);
  [CFunction=SelectionRemoveRange] void removeRange(any a0);
  [CFunction=SelectionRemoveAllRanges] void removeAllRanges();
  [CFunction=SelectionRemoveAllRanges] void empty();
  [CFunction=SelectionCollapse] void collapse();
  [CFunction=SelectionCollapseToStart] void collapseToStart();
  [CFunction=SelectionCollapseToEnd] void collapseToEnd();
  [CFunction=SelectionExtend] void extend(any a0);
  [CFunction=SelectionSetBaseAndExtent] void setBaseAndExtent(any a0, any a1, any a2, any a3);
  [CFunction=SelectionSelectAllChildren] void selectAllChildren(any a0);
  [CFunction=SelectionDeleteFromDocument] void deleteFromDocument();
  [CFunction=SelectionContainsNode] void containsNode(any a0);
  [CFunction=SelectionToString] void toString();
};
