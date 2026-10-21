// neko::javascript DOM bindings — Selection (Selection API).
//
// The Selection API (https://w3c.github.io/selection-api/, the spec WHATWG HTML
// references for editing) layered on the engine's DOM Range (DOM §5.5).  A
// binder has one Selection per document: window.getSelection() and
// document.getSelection() both return the same cached object (Impl::selection),
// and a document without a browsing context reports null, as the spec requires.
//
// What is IMPLEMENTED (Selection API):
//   * anchorNode/anchorOffset/focusNode/focusOffset/isCollapsed/rangeCount/type
//     over an explicitly stored anchor/focus boundary pair;
//   * getRangeAt, addRange, removeRange, removeAllRanges/empty, collapse (and
//     its historical no-argument form), collapseToStart, collapseToEnd, extend,
//     setBaseAndExtent, selectAllChildren, deleteFromDocument, containsNode and
//     the stringifier;
//   * boundary validation: InvalidNodeTypeError for a doctype,
//     WrongDocumentError for a node owned by another Document, IndexSizeError
//     for an offset past the node's length, InvalidStateError for
//     collapseToStart/collapseToEnd/extend on an empty selection, NotFoundError
//     for removeRange of a range that is not in the range list;
//   * a synchronously dispatched, non-bubbling `selectionchange` event at the
//     document whenever the stored selection actually changes.
//
// Deviations from the spec, all deliberate:
//
//   * Live ranges are NOT IMPLEMENTED.  The engine's Range is a static snapshot
//     (the DOM has no mutation hook; see range_binding.cpp), so Selection does
//     not keep JS Range objects as its state: it stores the anchor/focus
//     boundary points and the range list as snapshots, then builds a fresh
//     Range on every getRangeAt()/stringifier call.  Consequences:
//       - getRangeAt(0) reflects the *current* selection (later DOM mutations
//         and later Selection calls are visible), which a stale snapshot would
//         not;
//       - getRangeAt(0) === getRangeAt(0) is false (the spec promises the same
//         live Range object).  Mutating a returned Range therefore does not
//         change the selection, and adding a Range by reference
//         (addRange's "same object" rule) is impossible: the boundaries are
//         copied.  Pages that keep a Selection-derived Range around and expect
//         it to stay attached will observe the difference.
//   * The range list may hold more than one range (addRange on a non-empty
//     selection appends a snapshot).  The Selection API restricts a selection
//     to at most one range and browsers ignore such an addRange, but this
//     package implements the multi-range shape the interface was designed for
//     (the task's addRange/removeRange contract); anchor/focus keep describing
//     the first range, and removeRange() promotes the next surviving range.
//     removeRange() matches a range by its boundary points because the range
//     list holds snapshots, not object identity.
//   * selectionchange is dispatched synchronously at the end of each mutating
//     call instead of being queued as a task.  The spec coalesces several
//     changes made before the task runs into one event; with this
//     approximation a synchronous batch of N changes fires up to N events, and
//     a listener that mutates the selection again does not re-enter the
//     dispatch (the change it made is visible to later readers but its own
//     event is skipped).
//   * Boundary validation follows the task contract (and the older draft)
//     rather than the current spec's silent "abort these steps": a node whose
//     tree root is another Document throws WrongDocumentError.  A detached node
//     (no Document root — only this realm can create those) is accepted and
//     stored, mirroring range_binding.cpp's DocumentOf(); the getters then
//     report the selection as empty until the node is re-inserted.
//   * collapse() called with no arguments collapses to the end of the current
//     selection (the historical pre-standard behaviour) instead of requiring
//     the node argument; collapse(null)/collapse(undefined) removes all ranges,
//     as the current spec says.  collapse() on an empty selection is a no-op.
//   * When a stored boundary node leaves the document tree (removed by script),
//     the selection is reported as empty (anchorNode/focusNode null, rangeCount
//     0, type "None") instead of crashing or silently rebasing onto a different
//     point, which is the spec's own rule for boundaries outside the document
//     tree; isCollapsed keeps comparing the stored points.  Stored offsets are
//     clamped to the node's current length on every read, so a Text node
//     shortened by script cannot produce an out-of-range Range.  Because there
//     is no DOM mutation hook, such a change is only observed on the next read
//     and does NOT fire selectionchange (the spec's live-range mutation rules
//     would).
//   * Text-control selections are NOT IMPLEMENTED: window/document
//     getSelection() never returns the selection of an <input>/<textarea>
//     (HTML §4.10.20); those controls expose selectionStart/selectionEnd/
//     setSelectionRange separately (ui_binding.cpp), and a unified control
//     Selection integration is future work.
//   * `direction`, `modify()`, `getComposedRanges()`, `setPosition()` and
//     selectstart are not implemented; shadow trees do not exist in this DOM,
//     so the shadow-including root checks reduce to the document tree.
//
// Threading: thread-confined, like the rest of the binder.

#include "neko/dom/element.h"
#include "neko/dom/node.h"

#include "binding_internal.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <quickjs.h>
#include <string>
#include <utility>
#include <vector>

namespace neko::javascript {

JSClassID g_selection_class_id = 0;

namespace {

std::mutex g_selection_class_mutex;

// One entry of the selection's range list.  A snapshot of two boundary points
// (no live adjustment; see the file comment).
struct SelectionRange
{
  dom::Node* start = nullptr;
  std::size_t start_offset = 0;
  dom::Node* end = nullptr;
  std::size_t end_offset = 0;
};

// Opaque state of a Selection object: the owning binder plus the selection
// state.  Node pointers stay valid for the binder's lifetime (removed nodes are
// retained by Impl::retained), exactly like RangeWrapper's.
struct SelectionWrapper
{
  Impl* impl = nullptr;
  dom::Document* document = nullptr;
  // The Selection API's anchor/focus.  Both null when the selection is empty.
  // anchor may be after focus (backward selection).
  dom::Node* anchor = nullptr;
  std::size_t anchor_offset = 0;
  dom::Node* focus = nullptr;
  std::size_t focus_offset = 0;
  // ranges[0] mirrors anchor/focus in document order; only addRange() on a
  // non-empty selection appends further entries.
  std::vector<SelectionRange> ranges;
};

void SelectionFinalizer(JSRuntime* /*rt*/, JSValue obj)
{
  delete static_cast<SelectionWrapper*>(JS_GetOpaque(obj, g_selection_class_id));
}

SelectionWrapper* UnwrapSelection(JSValueConst value)
{
  return static_cast<SelectionWrapper*>(JS_GetOpaque(value, g_selection_class_id));
}

// Resolves the receiver, throwing the TypeError WebIDL raises for a call on a
// non-Selection object.
SelectionWrapper* SelectionOf(JSContext* ctx, JSValueConst value)
{
  SelectionWrapper* selection = UnwrapSelection(value);
  if (selection == nullptr || selection->impl == nullptr) {
    JS_ThrowTypeError(ctx, "not a Selection");
    return nullptr;
  }
  return selection;
}

// The document tree (Selection API "in the document tree"): the node's tree
// root is the selection's document.  The document itself is in its own tree.
bool InDocumentTree(const dom::Node* node, const dom::Document* document)
{
  return node != nullptr && RangeRootNode(const_cast<dom::Node*>(node)) == document;
}

// Whether |node| may be used as a boundary of |document|'s selection.  A
// detached subtree has no Document root; only this realm can create such nodes,
// so it is treated as belonging to the binder's document (the same rule as
// range_binding.cpp's DocumentOf).  A node whose root is a *different*
// Document is rejected.
bool BelongsToDocument(const dom::Node& node, const dom::Document& document)
{
  dom::Node* root = RangeRootNode(const_cast<dom::Node*>(&node));
  return root->node_type() != dom::NodeType::kDocument || root == &document;
}

// DOM mutation can shorten a stored container (text replaced, children
// removed); reads clamp instead of constructing an out-of-range Range.
std::size_t ClampOffset(const dom::Node* node, std::size_t offset)
{
  return node == nullptr ? 0 : std::min(offset, RangeNodeLength(*node));
}

bool SamePoint(const dom::Node* a, std::size_t a_offset, const dom::Node* b, std::size_t b_offset)
{
  return a == b && a_offset == b_offset;
}

bool SameRange(const SelectionRange& a, const SelectionRange& b)
{
  return SamePoint(a.start, a.start_offset, b.start, b.start_offset) &&
         SamePoint(a.end, a.end_offset, b.end, b.end_offset);
}

// The two boundary points in document order (start before end).  Points in
// different trees are not comparable and are kept in the given order.
SelectionRange OrderedRange(dom::Node* anchor,
                            std::size_t anchor_offset,
                            dom::Node* focus,
                            std::size_t focus_offset)
{
  SelectionRange range{anchor, anchor_offset, focus, focus_offset};
  if (anchor == focus) {
    if (anchor_offset > focus_offset) {
      range.start_offset = focus_offset;
      range.end_offset = anchor_offset;
    }
    return range;
  }
  if (RangeRootNode(anchor) == RangeRootNode(focus) &&
      CompareRangeBoundaryPoints(anchor, anchor_offset, focus, focus_offset) > 0) {
    range = {focus, focus_offset, anchor, anchor_offset};
  }
  return range;
}

// Clamps both offsets and restores the start-before-end invariant that a DOM
// mutation may have broken (e.g. the end text node was shortened).
SelectionRange ClampRange(SelectionRange range)
{
  range.start_offset = ClampOffset(range.start, range.start_offset);
  range.end_offset = ClampOffset(range.end, range.end_offset);
  if (range.start != nullptr && range.end != nullptr &&
      RangeRootNode(range.start) == RangeRootNode(range.end) &&
      CompareRangeBoundaryPoints(range.start, range.start_offset, range.end, range.end_offset) >
          0) {
    std::swap(range.start, range.end);
    std::swap(range.start_offset, range.end_offset);
  }
  return range;
}

// The selection's first range, as it currently reads.  Derived from
// anchor/focus on demand so that getRangeAt(0) and the stringifier observe the
// current selection (and clamped offsets) rather than a stale snapshot.
SelectionRange EffectivePrimary(const SelectionWrapper& selection)
{
  return ClampRange(OrderedRange(
      selection.anchor, selection.anchor_offset, selection.focus, selection.focus_offset));
}

// The Selection API's "empty" predicate for the reported state: no anchor/focus
// or a boundary outside the document tree means rangeCount 0 and type "None".
bool IsEmpty(const SelectionWrapper& selection)
{
  return selection.anchor == nullptr || selection.focus == nullptr ||
         !InDocumentTree(selection.anchor, selection.document) ||
         !InDocumentTree(selection.focus, selection.document);
}

bool SameSelectionState(const SelectionWrapper& selection,
                        dom::Node* anchor,
                        std::size_t anchor_offset,
                        dom::Node* focus,
                        std::size_t focus_offset,
                        const std::vector<SelectionRange>& ranges)
{
  if (!SamePoint(selection.anchor, selection.anchor_offset, anchor, anchor_offset) ||
      !SamePoint(selection.focus, selection.focus_offset, focus, focus_offset) ||
      selection.ranges.size() != ranges.size()) {
    return false;
  }
  for (std::size_t i = 0; i < ranges.size(); ++i) {
    if (!SameRange(selection.ranges[i], ranges[i])) {
      return false;
    }
  }
  return true;
}

// Fires the non-bubbling `selectionchange` event at the document (Selection API
// "firing selectionchange event"; a document target does not bubble).  The
// dispatch is synchronous, i.e. it happens before the mutating call returns.
void FireSelectionChange(SelectionWrapper& selection)
{
  Impl& impl = *selection.impl;
  if (selection.document == nullptr || impl.dispatching_selection_change) {
    return;
  }
  impl.dispatching_selection_change = true;
  impl.DispatchNonBubblingToNode(selection.document, "selectionchange");
  impl.dispatching_selection_change = false;
}

// Applies a new selection state and fires selectionchange when anything
// changed (anchor, focus or the range list).  Passing null anchor/focus empties
// the selection.
void CommitSelection(SelectionWrapper& selection,
                     dom::Node* anchor,
                     std::size_t anchor_offset,
                     dom::Node* focus,
                     std::size_t focus_offset,
                     std::vector<SelectionRange> ranges)
{
  if (anchor == nullptr || focus == nullptr) {
    ranges.clear();
  } else if (ranges.empty()) {
    ranges.push_back(OrderedRange(anchor, anchor_offset, focus, focus_offset));
  }
  if (SameSelectionState(selection, anchor, anchor_offset, focus, focus_offset, ranges)) {
    return;
  }
  selection.anchor = anchor;
  selection.anchor_offset = anchor_offset;
  selection.focus = focus;
  selection.focus_offset = focus_offset;
  selection.ranges = std::move(ranges);
  FireSelectionChange(selection);
}

void ClearSelection(SelectionWrapper& selection)
{
  CommitSelection(selection, nullptr, 0, nullptr, 0, {});
}

// Validates a node argument used as a selection boundary.  A doctype is never a
// legal Selection boundary point (the associated range could not represent it),
// and a node from another Document throws WrongDocumentError (see the file
// comment for why this is stricter than the current spec's silent abort).
bool ReadBoundaryNode(
    JSContext* ctx, Impl& impl, JSValueConst value, const char* operation, dom::Node** node)
{
  *node = UnwrapNode(ctx, value);
  if (*node == nullptr) {
    JS_ThrowTypeError(ctx, "%s: argument is not a Node", operation);
    return false;
  }
  if ((*node)->node_type() == dom::NodeType::kDocumentType) {
    ThrowDomException(ctx,
                      "InvalidNodeTypeError",
                      std::string(operation) + ": a doctype cannot be a selection boundary");
    return false;
  }
  if (!BelongsToDocument(**node, impl.document)) {
    ThrowDomException(ctx,
                      "WrongDocumentError",
                      std::string(operation) + ": the node belongs to a different document");
    return false;
  }
  return true;
}

// Reads and validates a (node, offset) boundary argument pair.  |offset_value|
// may be JS_UNDEFINED for the optional offset (Web IDL default 0).
bool ReadBoundary(JSContext* ctx,
                  Impl& impl,
                  JSValueConst node_value,
                  JSValueConst offset_value,
                  const char* operation,
                  dom::Node** node,
                  std::size_t* offset)
{
  if (!ReadBoundaryNode(ctx, impl, node_value, operation, node)) {
    return false;
  }
  std::uint32_t value = 0;
  if (JS_ToUint32(ctx, &value, offset_value) < 0) {
    return false;
  }
  *offset = value;
  if (*offset > RangeNodeLength(**node)) {
    ThrowDomException(ctx,
                      "IndexSizeError",
                      std::string(operation) + ": offset is larger than the node's length");
    return false;
  }
  return true;
}

// --- Getters ---------------------------------------------------------------

JSValue SelectionGetAnchorNode(JSContext* ctx, JSValueConst this_val)
{
  SelectionWrapper* selection = SelectionOf(ctx, this_val);
  if (selection == nullptr) {
    return JS_EXCEPTION;
  }
  if (!InDocumentTree(selection->anchor, selection->document)) {
    return JS_NULL;
  }
  return selection->impl->WrapNode(selection->anchor);
}

JSValue SelectionGetAnchorOffset(JSContext* ctx, JSValueConst this_val)
{
  SelectionWrapper* selection = SelectionOf(ctx, this_val);
  if (selection == nullptr) {
    return JS_EXCEPTION;
  }
  if (!InDocumentTree(selection->anchor, selection->document)) {
    return JS_NewUint32(ctx, 0);
  }
  return JS_NewUint32(
      ctx, static_cast<std::uint32_t>(ClampOffset(selection->anchor, selection->anchor_offset)));
}

JSValue SelectionGetFocusNode(JSContext* ctx, JSValueConst this_val)
{
  SelectionWrapper* selection = SelectionOf(ctx, this_val);
  if (selection == nullptr) {
    return JS_EXCEPTION;
  }
  if (!InDocumentTree(selection->focus, selection->document)) {
    return JS_NULL;
  }
  return selection->impl->WrapNode(selection->focus);
}

JSValue SelectionGetFocusOffset(JSContext* ctx, JSValueConst this_val)
{
  SelectionWrapper* selection = SelectionOf(ctx, this_val);
  if (selection == nullptr) {
    return JS_EXCEPTION;
  }
  if (!InDocumentTree(selection->focus, selection->document)) {
    return JS_NewUint32(ctx, 0);
  }
  return JS_NewUint32(
      ctx, static_cast<std::uint32_t>(ClampOffset(selection->focus, selection->focus_offset)));
}

JSValue SelectionGetIsCollapsed(JSContext* ctx, JSValueConst this_val)
{
  SelectionWrapper* selection = SelectionOf(ctx, this_val);
  if (selection == nullptr) {
    return JS_EXCEPTION;
  }
  // The spec compares the anchor and focus boundary points (null == null is
  // collapsed), independently of whether they are still in the document tree.
  return JS_NewBool(
      ctx,
      SamePoint(
          selection->anchor, selection->anchor_offset, selection->focus, selection->focus_offset));
}

JSValue SelectionGetRangeCount(JSContext* ctx, JSValueConst this_val)
{
  SelectionWrapper* selection = SelectionOf(ctx, this_val);
  if (selection == nullptr) {
    return JS_EXCEPTION;
  }
  const std::size_t count = IsEmpty(*selection) ? 0 : selection->ranges.size();
  return JS_NewUint32(ctx, static_cast<std::uint32_t>(count));
}

JSValue SelectionGetType(JSContext* ctx, JSValueConst this_val)
{
  SelectionWrapper* selection = SelectionOf(ctx, this_val);
  if (selection == nullptr) {
    return JS_EXCEPTION;
  }
  const char* type = "None";
  if (!IsEmpty(*selection)) {
    type =
        SamePoint(
            selection->anchor, selection->anchor_offset, selection->focus, selection->focus_offset)
            ? "Caret"
            : "Range";
  }
  return JS_NewString(ctx, type);
}

// --- Methods ---------------------------------------------------------------

JSValue SelectionGetRangeAt(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  SelectionWrapper* selection = SelectionOf(ctx, this_val);
  if (selection == nullptr) {
    return JS_EXCEPTION;
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "getRangeAt requires an index");
  }
  std::uint32_t index = 0;
  if (JS_ToUint32(ctx, &index, argv[0]) < 0) {
    return JS_EXCEPTION;
  }
  if (IsEmpty(*selection) || index >= selection->ranges.size()) {
    return ThrowDomException(
        ctx, "IndexSizeError", "getRangeAt: the index is outside the selection's range list");
  }
  const SelectionRange range =
      index == 0 ? EffectivePrimary(*selection) : ClampRange(selection->ranges[index]);
  return MakeRangeObject(ctx,
                         *selection->impl,
                         selection->document,
                         range.start,
                         range.start_offset,
                         range.end,
                         range.end_offset,
                         selection->impl->range_proto);
}

JSValue SelectionAddRange(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  SelectionWrapper* selection = SelectionOf(ctx, this_val);
  if (selection == nullptr) {
    return JS_EXCEPTION;
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "addRange requires a Range");
  }
  RangeWrapper* range = UnwrapRange(argv[0]);
  if (range == nullptr || range->impl == nullptr) {
    return JS_ThrowTypeError(ctx, "addRange: argument is not a Range");
  }
  if (range->document != selection->document) {
    return ThrowDomException(
        ctx, "WrongDocumentError", "addRange: the range belongs to a different document");
  }
  const SelectionRange snapshot =
      ClampRange({range->start, range->start_offset, range->end, range->end_offset});
  std::vector<SelectionRange> ranges = selection->ranges;
  for (const SelectionRange& existing : ranges) {
    if (SameRange(existing, snapshot)) {
      return JS_UNDEFINED; // already in the range list
    }
  }
  if (IsEmpty(*selection)) {
    // rangeCount is 0: the range becomes the selection and defines anchor and
    // focus (forward direction).
    CommitSelection(*selection,
                    snapshot.start,
                    snapshot.start_offset,
                    snapshot.end,
                    snapshot.end_offset,
                    {snapshot});
    return JS_UNDEFINED;
  }
  ranges.push_back(snapshot);
  CommitSelection(*selection,
                  selection->anchor,
                  selection->anchor_offset,
                  selection->focus,
                  selection->focus_offset,
                  std::move(ranges));
  return JS_UNDEFINED;
}

JSValue SelectionRemoveRange(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  SelectionWrapper* selection = SelectionOf(ctx, this_val);
  if (selection == nullptr) {
    return JS_EXCEPTION;
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "removeRange requires a Range");
  }
  RangeWrapper* range = UnwrapRange(argv[0]);
  if (range == nullptr || range->impl == nullptr) {
    return JS_ThrowTypeError(ctx, "removeRange: argument is not a Range");
  }
  const SelectionRange snapshot{range->start, range->start_offset, range->end, range->end_offset};
  std::vector<SelectionRange> ranges = selection->ranges;
  const auto it = std::find_if(ranges.begin(), ranges.end(), [&](const SelectionRange& candidate) {
    return SameRange(candidate, snapshot);
  });
  if (it == ranges.end()) {
    return ThrowDomException(
        ctx, "NotFoundError", "removeRange: the range is not in the selection's range list");
  }
  ranges.erase(it);
  if (ranges.empty()) {
    ClearSelection(*selection);
    return JS_UNDEFINED;
  }
  // The first surviving range describes the selection (forward direction).
  const SelectionRange primary = ranges.front();
  CommitSelection(*selection,
                  primary.start,
                  primary.start_offset,
                  primary.end,
                  primary.end_offset,
                  std::move(ranges));
  return JS_UNDEFINED;
}

JSValue SelectionRemoveAllRanges(JSContext* ctx,
                                 JSValueConst this_val,
                                 int /*argc*/,
                                 JSValueConst* /*argv*/)
{
  SelectionWrapper* selection = SelectionOf(ctx, this_val);
  if (selection == nullptr) {
    return JS_EXCEPTION;
  }
  ClearSelection(*selection);
  return JS_UNDEFINED;
}

// collapseToStart/collapseToEnd share this: the selection's first range is
// replaced by a collapsed range at one of its ends (the spec replaces the
// range, it does not mutate it).
JSValue CollapseToEdge(JSContext* ctx, SelectionWrapper& selection, bool to_start)
{
  if (IsEmpty(selection)) {
    return ThrowDomException(ctx,
                             "InvalidStateError",
                             to_start ? "collapseToStart: the selection is empty"
                                      : "collapseToEnd: the selection is empty");
  }
  const SelectionRange range = EffectivePrimary(selection);
  dom::Node* node = to_start ? range.start : range.end;
  const std::size_t offset = to_start ? range.start_offset : range.end_offset;
  CommitSelection(selection, node, offset, node, offset, {{node, offset, node, offset}});
  return JS_UNDEFINED;
}

JSValue SelectionCollapse(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  SelectionWrapper* selection = SelectionOf(ctx, this_val);
  if (selection == nullptr) {
    return JS_EXCEPTION;
  }
  // The historical no-argument form collapses to the end of the selection.
  if (argc == 0) {
    if (IsEmpty(*selection)) {
      return JS_UNDEFINED;
    }
    return CollapseToEdge(ctx, *selection, /*to_start=*/false);
  }
  // collapse(null) / collapse(undefined) remove all ranges (spec step 1).
  if (JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
    ClearSelection(*selection);
    return JS_UNDEFINED;
  }
  dom::Node* node = nullptr;
  std::size_t offset = 0;
  if (!ReadBoundary(ctx,
                    *selection->impl,
                    argv[0],
                    argc >= 2 ? argv[1] : JS_UNDEFINED,
                    "collapse",
                    &node,
                    &offset)) {
    return JS_EXCEPTION;
  }
  CommitSelection(*selection, node, offset, node, offset, {{node, offset, node, offset}});
  return JS_UNDEFINED;
}

JSValue SelectionCollapseToStart(JSContext* ctx,
                                 JSValueConst this_val,
                                 int /*argc*/,
                                 JSValueConst* /*argv*/)
{
  SelectionWrapper* selection = SelectionOf(ctx, this_val);
  if (selection == nullptr) {
    return JS_EXCEPTION;
  }
  return CollapseToEdge(ctx, *selection, /*to_start=*/true);
}

JSValue
SelectionCollapseToEnd(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  SelectionWrapper* selection = SelectionOf(ctx, this_val);
  if (selection == nullptr) {
    return JS_EXCEPTION;
  }
  return CollapseToEdge(ctx, *selection, /*to_start=*/false);
}

JSValue SelectionExtend(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  SelectionWrapper* selection = SelectionOf(ctx, this_val);
  if (selection == nullptr) {
    return JS_EXCEPTION;
  }
  dom::Node* node = nullptr;
  std::size_t offset = 0;
  if (!ReadBoundary(ctx,
                    *selection->impl,
                    argc >= 1 ? argv[0] : JS_UNDEFINED,
                    argc >= 2 ? argv[1] : JS_UNDEFINED,
                    "extend",
                    &node,
                    &offset)) {
    return JS_EXCEPTION;
  }
  if (IsEmpty(*selection)) {
    return ThrowDomException(ctx, "InvalidStateError", "extend: the selection is empty");
  }
  // The anchor stays where it was; the focus moves.  A focus before the anchor
  // makes the selection backward (anchor after focus), which the getters and
  // type report faithfully.
  dom::Node* anchor = selection->anchor;
  const std::size_t anchor_offset = selection->anchor_offset;
  const SelectionRange range = OrderedRange(anchor, anchor_offset, node, offset);
  CommitSelection(*selection, anchor, anchor_offset, node, offset, {range});
  return JS_UNDEFINED;
}

JSValue
SelectionSetBaseAndExtent(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  SelectionWrapper* selection = SelectionOf(ctx, this_val);
  if (selection == nullptr) {
    return JS_EXCEPTION;
  }
  if (argc < 4) {
    return JS_ThrowTypeError(ctx, "setBaseAndExtent requires four arguments");
  }
  dom::Node* anchor = nullptr;
  dom::Node* focus = nullptr;
  std::size_t anchor_offset = 0;
  std::size_t focus_offset = 0;
  if (!ReadBoundary(
          ctx, *selection->impl, argv[0], argv[1], "setBaseAndExtent", &anchor, &anchor_offset) ||
      !ReadBoundary(
          ctx, *selection->impl, argv[2], argv[3], "setBaseAndExtent", &focus, &focus_offset)) {
    return JS_EXCEPTION;
  }
  const SelectionRange range = OrderedRange(anchor, anchor_offset, focus, focus_offset);
  CommitSelection(*selection, anchor, anchor_offset, focus, focus_offset, {range});
  return JS_UNDEFINED;
}

JSValue
SelectionSelectAllChildren(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  SelectionWrapper* selection = SelectionOf(ctx, this_val);
  if (selection == nullptr) {
    return JS_EXCEPTION;
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "selectAllChildren requires a Node");
  }
  dom::Node* node = nullptr;
  if (!ReadBoundaryNode(ctx, *selection->impl, argv[0], "selectAllChildren", &node)) {
    return JS_EXCEPTION;
  }
  const std::size_t length = RangeNodeLength(*node);
  CommitSelection(*selection, node, 0, node, length, {{node, 0, node, length}});
  return JS_UNDEFINED;
}

JSValue SelectionDeleteFromDocument(JSContext* ctx,
                                    JSValueConst this_val,
                                    int /*argc*/,
                                    JSValueConst* /*argv*/)
{
  SelectionWrapper* selection = SelectionOf(ctx, this_val);
  if (selection == nullptr) {
    return JS_EXCEPTION;
  }
  if (IsEmpty(*selection)) {
    return JS_UNDEFINED;
  }
  const SelectionRange range = EffectivePrimary(*selection);
  dom::Node* collapsed_node = nullptr;
  std::size_t collapsed_offset = 0;
  if (!DeleteRangeContentsBetween(*selection->impl,
                                  range.start,
                                  range.start_offset,
                                  range.end,
                                  range.end_offset,
                                  &collapsed_node,
                                  &collapsed_offset)) {
    return JS_UNDEFINED;
  }
  // deleteContents() collapses the deleted range; the selection follows it.
  CommitSelection(*selection,
                  collapsed_node,
                  collapsed_offset,
                  collapsed_node,
                  collapsed_offset,
                  {{collapsed_node, collapsed_offset, collapsed_node, collapsed_offset}});
  return JS_UNDEFINED;
}

JSValue SelectionContainsNode(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  SelectionWrapper* selection = SelectionOf(ctx, this_val);
  if (selection == nullptr) {
    return JS_EXCEPTION;
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "containsNode requires a Node");
  }
  dom::Node* node = UnwrapNode(ctx, argv[0]);
  if (node == nullptr) {
    return JS_ThrowTypeError(ctx, "containsNode: argument is not a Node");
  }
  int partial = 0;
  if (argc >= 2) {
    partial = JS_ToBool(ctx, argv[1]);
    if (partial < 0) {
      return JS_EXCEPTION;
    }
  }
  if (IsEmpty(*selection) || RangeRootNode(node) != selection->document) {
    return JS_FALSE;
  }
  const SelectionRange range = EffectivePrimary(*selection);
  const std::size_t length = RangeNodeLength(*node);
  const bool contained =
      partial != 0
          ? CompareRangeBoundaryPoints(range.start, range.start_offset, node, length) <= 0 &&
                CompareRangeBoundaryPoints(range.end, range.end_offset, node, 0) >= 0
          : CompareRangeBoundaryPoints(range.start, range.start_offset, node, 0) <= 0 &&
                CompareRangeBoundaryPoints(range.end, range.end_offset, node, length) >= 0;
  return JS_NewBool(ctx, contained);
}

JSValue
SelectionToString(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  SelectionWrapper* selection = SelectionOf(ctx, this_val);
  if (selection == nullptr) {
    return JS_EXCEPTION;
  }
  if (IsEmpty(*selection)) {
    return JS_NewStringLen(ctx, "", 0);
  }
  const SelectionRange range = EffectivePrimary(*selection);
  const std::string text =
      StringifyRangeBoundaries(range.start, range.start_offset, range.end, range.end_offset);
  return JS_NewStringLen(ctx, text.data(), text.size());
}

} // namespace

namespace {

// The cached per-document singleton, created on first getSelection().
JSValue MakeSelectionObject(JSContext* ctx, Impl& impl)
{
  if (!JS_IsUndefined(impl.selection)) {
    return JS_DupValue(ctx, impl.selection);
  }
  auto* wrapper = new SelectionWrapper();
  wrapper->impl = &impl;
  wrapper->document = &impl.document;
  JSValue object = JS_NewObjectClass(ctx, g_selection_class_id);
  if (JS_IsException(object)) {
    delete wrapper;
    return object;
  }
  JS_SetOpaque(object, wrapper);
  JS_SetPrototype(ctx, object, impl.selection_proto);
  impl.selection = JS_DupValue(ctx, object);
  return object;
}

// window.getSelection(): the selection of this Window's document.
JSValue
WindowGetSelection(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/)
{
  Impl* impl = ImplFor(ctx, JS_UNDEFINED);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  return MakeSelectionObject(ctx, *impl);
}

} // namespace

// document.getSelection() (Selection API extensions to Document).
JSValue DocGetSelection(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  Impl* impl = ImplFor(ctx, this_val);
  dom::Node* node = UnwrapNode(ctx, this_val);
  if (impl == nullptr || node == nullptr || node->node_type() != dom::NodeType::kDocument) {
    return JS_ThrowTypeError(ctx, "not a document");
  }
  // "return the selection associated with this if this has an associated
  // browsing context, null otherwise": only the binder's main document is
  // loaded by a browsing context in this engine.
  if (node != &impl->document) {
    return JS_NULL;
  }
  return MakeSelectionObject(ctx, *impl);
}

void DefineSelectionPrototype(JSContext* ctx, Impl& impl)
{
  impl.selection_proto = JS_NewObject(ctx);

  static const std::array<JSCFunctionListEntry, 14> kMethods = {{
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
  }};
  JS_SetPropertyFunctionList(
      ctx, impl.selection_proto, kMethods.data(), static_cast<int>(kMethods.size()));

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

  JSValue global = JS_GetGlobalObject(ctx);
  // Selection is not constructable from script (WebIDL [Exposed=Window]
  // interface with no constructor): new Selection() is an illegal constructor,
  // but `selection instanceof Selection` and prototype extension work.
  DefineInterface(ctx, global, "Selection", impl.selection_proto);
  JS_SetPropertyStr(
      ctx, global, "getSelection", JS_NewCFunction(ctx, WindowGetSelection, "getSelection", 0));
  JS_FreeValue(ctx, global);
}

void EnsureSelectionClassRegistered(JSRuntime* rt)
{
  std::lock_guard<std::mutex> lock(g_selection_class_mutex);
  JS_NewClassID(rt, &g_selection_class_id);
  if (!JS_IsRegisteredClass(rt, g_selection_class_id)) {
    JSClassDef def;
    std::memset(&def, 0, sizeof(def));
    def.class_name = "Selection";
    def.finalizer = &SelectionFinalizer;
    JS_NewClass(rt, g_selection_class_id, &def);
  }
}

} // namespace neko::javascript
