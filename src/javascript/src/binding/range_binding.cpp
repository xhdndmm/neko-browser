// neko::javascript DOM bindings — Range (WHATWG DOM §5.5).
//
// Part of the dom_binding split (see binding/binding_internal.h).  This file
// owns the Range wrapper class, its prototype/constructor, and the boundary
// point algorithms (position, node length, common ancestor, containment).
//
// What is IMPLEMENTED (DOM Standard §5.5):
//   * `new Range()` and `document.createRange()` over the current document;
//   * the AbstractRange accessors (startContainer/startOffset/endContainer/
//     endOffset/collapsed) and commonAncestorContainer;
//   * setStart/setEnd/setStartBefore/setEndBefore/setStartAfter/setEndAfter
//     with doctype (InvalidNodeTypeError), foreign document
//     (WrongDocumentError) and out-of-range offset (IndexSizeError) checks;
//   * selectNode/selectNodeContents/collapse;
//   * cloneRange, cloneContents, extractContents, deleteContents, insertNode
//     (with text splitting) and surroundContents;
//   * compareBoundaryPoints (all four modes), comparePoint, isPointInRange,
//     intersectsNode and the tree stringifier (toString).
//
// What is deliberately NOT IMPLEMENTED:
//   * getBoundingClientRect()/getClientRects(): they need a layout-tree mapping
//     the javascript layer does not have (Range geometry belongs to CSSOM View).
//     The methods exist only to fail loudly with NotSupportedError; they never
//     return fake rectangles.
//   * live-range mutation adjustment (DOM §5.5 "live range pre-remove steps",
//     the insert/split/replace-data hooks).  The DOM has no document
//     back-pointer or mutation hook, so a Range is a static snapshot: ordinary
//     DOM mutations elsewhere do not move its boundary points.  Range's own
//     mutating methods (delete/extract/insert/surround) do update the range
//     they are called on, as their algorithms require.
//   * createContextualFragment() and StaticRange/AbstractRange are not part of
//     this package.
//
// Boundary offsets are in the DOM "node length": UTF-16 code units for
// Text/Comment data, the child count for other nodes (doctypes have length 0).
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
#include <memory>
#include <mutex>
#include <quickjs.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neko::javascript {

JSClassID g_range_class_id = 0;

namespace {

std::mutex g_range_class_mutex;

void RangeFinalizer(JSRuntime* /*rt*/, JSValue obj)
{
  auto* wrapper = static_cast<RangeWrapper*>(JS_GetOpaque(obj, g_range_class_id));
  delete wrapper;
}

// The two boundary points of a range, copied out of the JS wrapper while an
// algorithm runs (extract/delete need to move them around).
struct RangeState
{
  dom::Node* start_node = nullptr;
  std::size_t start_offset = 0;
  dom::Node* end_node = nullptr;
  std::size_t end_offset = 0;
};

RangeState StateOf(const RangeWrapper& wrapper)
{
  return {wrapper.start, wrapper.start_offset, wrapper.end, wrapper.end_offset};
}

void StoreState(RangeWrapper& wrapper, const RangeState& state)
{
  wrapper.start = state.start_node;
  wrapper.start_offset = state.start_offset;
  wrapper.end = state.end_node;
  wrapper.end_offset = state.end_offset;
}

bool IsCollapsed(const RangeState& range)
{
  return range.start_node == range.end_node && range.start_offset == range.end_offset;
}

bool IsCharacterData(const dom::Node& node)
{
  return node.node_type() == dom::NodeType::kText || node.node_type() == dom::NodeType::kComment;
}

bool IsInclusiveAncestor(const dom::Node* ancestor, const dom::Node* node)
{
  return ancestor == node || IsAncestorOf(ancestor, node);
}

dom::Node* RootOf(dom::Node* node)
{
  while (node->parent() != nullptr) {
    node = node->parent();
  }
  return node;
}

// The document a node belongs to.  Detached subtrees have no root Document to
// walk to; they are treated as belonging to the binder's document (the only
// document this javascript realm can create detached nodes for).
dom::Document* DocumentOf(Impl& impl, const dom::Node& node)
{
  dom::Node* root = RootOf(const_cast<dom::Node*>(&node));
  if (root->node_type() == dom::NodeType::kDocument) {
    return static_cast<dom::Document*>(root);
  }
  return &impl.document;
}

std::size_t IndexOf(const dom::Node& node)
{
  const dom::Node* parent = node.parent();
  if (parent == nullptr) {
    return 0;
  }
  std::size_t index = 0;
  for (dom::Node* child : parent->ChildNodes()) {
    if (child == &node) {
      return index;
    }
    ++index;
  }
  return index;
}

// DOM "node length": CharacterData length in UTF-16 code units, doctype 0,
// every other node its number of children.
std::size_t NodeLength(const dom::Node& node)
{
  switch (node.node_type()) {
  case dom::NodeType::kText:
    return dom::Utf16Length(static_cast<const dom::Text&>(node).data());
  case dom::NodeType::kComment:
    return dom::Utf16Length(static_cast<const dom::Comment&>(node).data());
  case dom::NodeType::kDocumentType:
    return 0;
  default:
    return node.child_count();
  }
}

std::string CharacterDataSlice(const dom::Node& node, std::size_t start, std::size_t end)
{
  const std::string* data = node.NodeValue();
  if (data == nullptr) {
    return {};
  }
  const std::size_t length = dom::Utf16Length(*data);
  start = std::min(start, length);
  end = std::min(std::max(end, start), length);
  const std::size_t byte_start = dom::Utf8OffsetForUtf16Offset(*data, start);
  const std::size_t byte_end = dom::Utf8OffsetForUtf16Offset(*data, end);
  return data->substr(byte_start, byte_end - byte_start);
}

void SetCharacterData(dom::Node& node, std::string_view data)
{
  if (node.node_type() == dom::NodeType::kText) {
    static_cast<dom::Text&>(node).SetData(data);
  } else if (node.node_type() == dom::NodeType::kComment) {
    static_cast<dom::Comment&>(node).SetData(data);
  }
}

// DOM "replace data" (the subset Range needs: no live ranges to adjust).
void ReplaceRangeData(Impl& impl,
                      dom::Node& node,
                      std::size_t offset,
                      std::size_t count,
                      std::string_view replacement)
{
  const std::string* data = node.NodeValue();
  if (data == nullptr) {
    return;
  }
  std::string updated = dom::ReplaceUtf16Range(*data, offset, offset + count, replacement);
  SetCharacterData(node, updated);
  impl.RecordCharacterDataMutation(&node);
  impl.MarkDomDirty(&node);
}

// Compares boundary points, returning -1 (a before b), 0 (equal) or 1 (a after
// b).  Both points must be in the same tree (callers check the roots);
// otherwise the result is meaningless and 0 is returned.
int ComparePoints(const dom::Node* a,
                  std::size_t a_offset,
                  const dom::Node* b,
                  std::size_t b_offset)
{
  if (a == b) {
    return a_offset < b_offset ? -1 : (a_offset > b_offset ? 1 : 0);
  }
  if (IsInclusiveAncestor(a, b)) {
    const dom::Node* child = b;
    while (child->parent() != a) {
      child = child->parent();
    }
    // a's point before/at the child precedes b's point inside it.
    return IndexOf(*child) < a_offset ? 1 : -1;
  }
  if (IsInclusiveAncestor(b, a)) {
    const dom::Node* child = a;
    while (child->parent() != b) {
      child = child->parent();
    }
    return IndexOf(*child) < b_offset ? -1 : 1;
  }

  const dom::Node* common = nullptr;
  for (const dom::Node* ancestor = a; ancestor != nullptr; ancestor = ancestor->parent()) {
    if (IsInclusiveAncestor(ancestor, b)) {
      common = ancestor;
      break;
    }
  }
  if (common == nullptr) {
    return 0;
  }
  const dom::Node* child_a = a;
  while (child_a->parent() != common) {
    child_a = child_a->parent();
  }
  const dom::Node* child_b = b;
  while (child_b->parent() != common) {
    child_b = child_b->parent();
  }
  return IndexOf(*child_a) < IndexOf(*child_b) ? -1 : 1;
}

dom::Node* GetCommonAncestor(dom::Node* start, dom::Node* end)
{
  for (dom::Node* ancestor = start; ancestor != nullptr; ancestor = ancestor->parent()) {
    if (IsInclusiveAncestor(ancestor, end)) {
      return ancestor;
    }
  }
  return nullptr;
}

bool IsContained(const RangeState& range, const dom::Node& node)
{
  if (RootOf(const_cast<dom::Node*>(&node)) != RootOf(range.start_node)) {
    return false;
  }
  return ComparePoints(&node, 0, range.start_node, range.start_offset) > 0 &&
         ComparePoints(&node, NodeLength(node), range.end_node, range.end_offset) < 0;
}

// The child of |common| that contains |start| and is partially contained in a
// range whose start node is not an inclusive ancestor of its end node.
dom::Node* FirstPartialChild(dom::Node* common, dom::Node* start, dom::Node* end)
{
  if (IsInclusiveAncestor(start, end)) {
    return nullptr;
  }
  dom::Node* child = start;
  while (child->parent() != common) {
    if (child->parent() == nullptr) {
      return nullptr;
    }
    child = child->parent();
  }
  return child;
}

dom::Node* LastPartialChild(dom::Node* common, dom::Node* end, dom::Node* start)
{
  if (IsInclusiveAncestor(end, start)) {
    return nullptr;
  }
  dom::Node* child = end;
  while (child->parent() != common) {
    if (child->parent() == nullptr) {
      return nullptr;
    }
    child = child->parent();
  }
  return child;
}

bool ContainsDoctype(const RangeState& range)
{
  dom::Node* common = GetCommonAncestor(range.start_node, range.end_node);
  if (common == nullptr) {
    return false;
  }
  for (dom::Node* child : common->ChildNodes()) {
    if (child->node_type() == dom::NodeType::kDocumentType && IsContained(range, *child)) {
      return true;
    }
  }
  return false;
}

void AppendOwned(dom::Node& parent, std::unique_ptr<dom::Node> child)
{
  if (child == nullptr) {
    return;
  }
  static_cast<void>(parent.AppendChild(std::move(child)));
}

void MoveChild(Impl& impl, dom::Node& from, dom::Node& to, dom::Node* child)
{
  std::unique_ptr<dom::Node> removed = from.RemoveChild(child);
  if (removed == nullptr) {
    return;
  }
  static_cast<void>(to.AppendChild(std::move(removed)));
  impl.RecordChildListMutation(&from, {}, {child});
  impl.RecordChildListMutation(&to, {child}, {});
}

// Recursively appends the data of Text nodes contained in |range| (DOM §5.5
// stringification step 4).
void AppendContainedText(const RangeState& range, const dom::Node& node, std::string& out)
{
  for (dom::Node* child : node.ChildNodes()) {
    if (child->node_type() == dom::NodeType::kText && IsContained(range, *child)) {
      out += static_cast<const dom::Text&>(*child).data();
    }
    AppendContainedText(range, *child, out);
  }
}

std::string StringifyRange(const RangeState& range)
{
  std::string out;
  if (range.start_node == range.end_node && range.start_node->node_type() == dom::NodeType::kText) {
    return CharacterDataSlice(*range.start_node, range.start_offset, range.end_offset);
  }
  if (range.start_node->node_type() == dom::NodeType::kText) {
    out += CharacterDataSlice(*range.start_node, range.start_offset, NodeLength(*range.start_node));
  }
  dom::Node* root = RootOf(range.start_node);
  AppendContainedText(range, *root, out);
  if (range.end_node->node_type() == dom::NodeType::kText) {
    out += CharacterDataSlice(*range.end_node, 0, range.end_offset);
  }
  return out;
}

// Collects the topmost contained nodes of |range| under |node| in tree order
// (the delete algorithm's nodesToRemove).
void CollectTopmostContained(const RangeState& range, dom::Node& node, std::vector<dom::Node*>& out)
{
  for (dom::Node* child : node.ChildNodes()) {
    if (IsContained(range, *child)) {
      out.push_back(child);
    } else {
      CollectTopmostContained(range, *child, out);
    }
  }
}

// DOM "split a Text node" (the subset Range needs).  Returns the new node
// holding the suffix, or nullptr when the insertion could not be performed.
dom::Node* SplitTextNode(Impl& impl, dom::Text& text, std::size_t offset)
{
  const std::string data = text.data();
  const std::size_t length = dom::Utf16Length(data);
  offset = std::min(offset, length);
  const std::size_t byte_offset = dom::Utf8OffsetForUtf16Offset(data, offset);

  auto new_text = std::make_unique<dom::Text>(data.substr(byte_offset));
  dom::Text* raw = new_text.get();
  dom::Node* parent = text.parent();
  if (parent != nullptr) {
    dom::Node* next = SiblingOf(&text, +1);
    if (!parent->InsertBefore(std::move(new_text), next)) {
      return nullptr;
    }
    impl.RecordChildListMutation(parent, {raw}, {});
    impl.MarkDomDirty(parent);
  } else {
    impl.TakeOwnership(raw, std::move(new_text));
  }
  text.SetData(data.substr(0, byte_offset));
  impl.RecordCharacterDataMutation(&text);
  impl.MarkDomDirty(&text);
  return raw;
}

std::unique_ptr<dom::DocumentFragment> CloneRangeContents(Impl& impl, const RangeState& range)
{
  auto fragment = std::make_unique<dom::DocumentFragment>();
  if (IsCollapsed(range)) {
    return fragment;
  }

  if (range.start_node == range.end_node && IsCharacterData(*range.start_node)) {
    auto clone = CloneNodeImpl(*range.start_node, false);
    if (clone != nullptr) {
      SetCharacterData(*clone,
                       CharacterDataSlice(*range.start_node, range.start_offset, range.end_offset));
      AppendOwned(*fragment, std::move(clone));
    }
    return fragment;
  }

  dom::Node* common = GetCommonAncestor(range.start_node, range.end_node);
  if (common == nullptr) {
    return fragment;
  }
  dom::Node* first = FirstPartialChild(common, range.start_node, range.end_node);
  dom::Node* last = LastPartialChild(common, range.end_node, range.start_node);

  if (first != nullptr && IsCharacterData(*first)) {
    auto clone = CloneNodeImpl(*range.start_node, false);
    if (clone != nullptr) {
      SetCharacterData(
          *clone,
          CharacterDataSlice(*range.start_node, range.start_offset, NodeLength(*range.start_node)));
      AppendOwned(*fragment, std::move(clone));
    }
  } else if (first != nullptr) {
    auto clone = CloneNodeImpl(*first, false);
    if (clone != nullptr) {
      dom::Node* clone_raw = clone.get();
      AppendOwned(*fragment, std::move(clone));
      RangeState subrange{range.start_node, range.start_offset, first, NodeLength(*first)};
      AppendOwned(*clone_raw, CloneRangeContents(impl, subrange));
    }
  }

  for (dom::Node* child : common->ChildNodes()) {
    if (!IsContained(range, *child)) {
      continue;
    }
    AppendOwned(*fragment, CloneNodeImpl(*child, true));
  }

  if (last != nullptr && IsCharacterData(*last)) {
    auto clone = CloneNodeImpl(*range.end_node, false);
    if (clone != nullptr) {
      SetCharacterData(*clone, CharacterDataSlice(*range.end_node, 0, range.end_offset));
      AppendOwned(*fragment, std::move(clone));
    }
  } else if (last != nullptr) {
    auto clone = CloneNodeImpl(*last, false);
    if (clone != nullptr) {
      dom::Node* clone_raw = clone.get();
      AppendOwned(*fragment, std::move(clone));
      RangeState subrange{last, 0, range.end_node, range.end_offset};
      AppendOwned(*clone_raw, CloneRangeContents(impl, subrange));
    }
  }

  return fragment;
}

// Computes where a range collapses after its contents are removed/extracted
// (the algorithm's newNode/newOffset).
void CollapsePointAfterRemoval(const RangeState& original, RangeState& range)
{
  if (IsInclusiveAncestor(original.start_node, original.end_node)) {
    range = {
        original.start_node, original.start_offset, original.start_node, original.start_offset};
    return;
  }
  dom::Node* reference = original.start_node;
  while (reference->parent() != nullptr &&
         !IsInclusiveAncestor(reference->parent(), original.end_node)) {
    reference = reference->parent();
  }
  if (reference->parent() == nullptr) {
    range = {
        original.start_node, original.start_offset, original.start_node, original.start_offset};
    return;
  }
  dom::Node* node = reference->parent();
  const std::size_t offset = IndexOf(*reference) + 1;
  range = {node, offset, node, offset};
}

std::unique_ptr<dom::DocumentFragment> ExtractRangeContents(Impl& impl, RangeState& range)
{
  auto fragment = std::make_unique<dom::DocumentFragment>();
  if (IsCollapsed(range)) {
    return fragment;
  }

  const RangeState original = range;

  if (original.start_node == original.end_node && IsCharacterData(*original.start_node)) {
    auto clone = CloneNodeImpl(*original.start_node, false);
    if (clone != nullptr) {
      SetCharacterData(
          *clone,
          CharacterDataSlice(*original.start_node, original.start_offset, original.end_offset));
      AppendOwned(*fragment, std::move(clone));
    }
    ReplaceRangeData(impl,
                     *original.start_node,
                     original.start_offset,
                     original.end_offset - original.start_offset,
                     "");
    range = {
        original.start_node, original.start_offset, original.start_node, original.start_offset};
    return fragment;
  }

  dom::Node* common = GetCommonAncestor(original.start_node, original.end_node);
  if (common == nullptr) {
    return fragment;
  }
  dom::Node* first = FirstPartialChild(common, original.start_node, original.end_node);
  dom::Node* last = LastPartialChild(common, original.end_node, original.start_node);

  std::vector<dom::Node*> contained;
  for (dom::Node* child : common->ChildNodes()) {
    if (IsContained(original, *child)) {
      contained.push_back(child);
    }
  }

  CollapsePointAfterRemoval(original, range);

  if (first != nullptr && IsCharacterData(*first)) {
    auto clone = CloneNodeImpl(*original.start_node, false);
    if (clone != nullptr) {
      SetCharacterData(*clone,
                       CharacterDataSlice(*original.start_node,
                                          original.start_offset,
                                          NodeLength(*original.start_node)));
      AppendOwned(*fragment, std::move(clone));
    }
    ReplaceRangeData(impl,
                     *original.start_node,
                     original.start_offset,
                     NodeLength(*original.start_node) - original.start_offset,
                     "");
  } else if (first != nullptr) {
    auto clone = CloneNodeImpl(*first, false);
    if (clone != nullptr) {
      dom::Node* clone_raw = clone.get();
      AppendOwned(*fragment, std::move(clone));
      RangeState subrange{original.start_node, original.start_offset, first, NodeLength(*first)};
      AppendOwned(*clone_raw, ExtractRangeContents(impl, subrange));
    }
  }

  for (dom::Node* child : contained) {
    MoveChild(impl, *common, *fragment, child);
  }

  if (last != nullptr && IsCharacterData(*last)) {
    auto clone = CloneNodeImpl(*original.end_node, false);
    if (clone != nullptr) {
      SetCharacterData(*clone, CharacterDataSlice(*original.end_node, 0, original.end_offset));
      AppendOwned(*fragment, std::move(clone));
    }
    ReplaceRangeData(impl, *original.end_node, 0, original.end_offset, "");
  } else if (last != nullptr) {
    auto clone = CloneNodeImpl(*last, false);
    if (clone != nullptr) {
      dom::Node* clone_raw = clone.get();
      AppendOwned(*fragment, std::move(clone));
      RangeState subrange{last, 0, original.end_node, original.end_offset};
      AppendOwned(*clone_raw, ExtractRangeContents(impl, subrange));
    }
  }

  impl.MarkDomDirty(common);
  return fragment;
}

void DeleteRangeContents(Impl& impl, RangeState& range)
{
  if (IsCollapsed(range)) {
    return;
  }
  const RangeState original = range;

  if (original.start_node == original.end_node && IsCharacterData(*original.start_node)) {
    ReplaceRangeData(impl,
                     *original.start_node,
                     original.start_offset,
                     original.end_offset - original.start_offset,
                     "");
    range = {
        original.start_node, original.start_offset, original.start_node, original.start_offset};
    return;
  }

  dom::Node* common = GetCommonAncestor(original.start_node, original.end_node);
  if (common == nullptr) {
    return;
  }

  std::vector<dom::Node*> nodes_to_remove;
  CollectTopmostContained(original, *common, nodes_to_remove);

  CollapsePointAfterRemoval(original, range);

  if (IsCharacterData(*original.start_node)) {
    ReplaceRangeData(impl,
                     *original.start_node,
                     original.start_offset,
                     NodeLength(*original.start_node) - original.start_offset,
                     "");
  }

  for (dom::Node* node : nodes_to_remove) {
    dom::Node* parent = node->parent();
    if (parent == nullptr) {
      continue;
    }
    std::unique_ptr<dom::Node> removed = parent->RemoveChild(node);
    if (removed == nullptr) {
      continue;
    }
    impl.TakeOwnership(node, std::move(removed));
    impl.RecordChildListMutation(parent, {}, {node});
  }

  if (IsCharacterData(*original.end_node)) {
    ReplaceRangeData(impl, *original.end_node, 0, original.end_offset, "");
  }

  impl.MarkDomDirty(common);
}

// DOM "ensure pre-insertion validity", restricted to the node types this DOM
// models (no ProcessingInstruction/CDATA/Attr nodes).  Returns true when the
// insertion is valid; otherwise throws the DOMException and returns false.
bool ChildTypeAllowed(dom::NodeType parent_type, dom::NodeType child_type)
{
  switch (parent_type) {
  case dom::NodeType::kDocument:
    return child_type == dom::NodeType::kElement || child_type == dom::NodeType::kDocumentType ||
           child_type == dom::NodeType::kDocumentFragment;
  case dom::NodeType::kElement:
  case dom::NodeType::kDocumentFragment:
    return child_type == dom::NodeType::kElement || child_type == dom::NodeType::kText ||
           child_type == dom::NodeType::kComment || child_type == dom::NodeType::kDocumentFragment;
  default:
    return false;
  }
}

std::size_t
ChildCountExcluding(const dom::Node& parent, dom::NodeType type, const dom::Node* exclude)
{
  std::size_t count = 0;
  for (dom::Node* child : parent.ChildNodes()) {
    if (child != exclude && child->node_type() == type) {
      ++count;
    }
  }
  return count;
}

bool CheckPreInsertion(JSContext* ctx, dom::Node* parent, dom::Node* node, dom::Node* reference)
{
  if (node == parent || node->node_type() == dom::NodeType::kDocument ||
      IsAncestorOf(node, parent)) {
    ThrowDomException(ctx, "HierarchyRequestError", "Range.insertNode: invalid insertion target");
    return false;
  }
  if (parent->WouldExceedMaximumTreeDepth(*node)) {
    ThrowDomException(
        ctx, "HierarchyRequestError", "Range.insertNode: maximum tree depth exceeded");
    return false;
  }
  if (!ChildTypeAllowed(parent->node_type(), node->node_type())) {
    ThrowDomException(ctx,
                      "HierarchyRequestError",
                      "Range.insertNode: a node of this type cannot be inserted here");
    return false;
  }
  if (reference != nullptr && reference->parent() != parent) {
    ThrowDomException(ctx,
                      "HierarchyRequestError",
                      "Range.insertNode: the reference node is not a child of the parent");
    return false;
  }

  if (parent->node_type() == dom::NodeType::kDocument) {
    std::size_t elements = ChildCountExcluding(*parent, dom::NodeType::kElement, node);
    std::size_t doctypes = ChildCountExcluding(*parent, dom::NodeType::kDocumentType, node);
    if (node->node_type() == dom::NodeType::kElement) {
      ++elements;
    } else if (node->node_type() == dom::NodeType::kDocumentType) {
      ++doctypes;
    } else if (node->node_type() == dom::NodeType::kDocumentFragment) {
      for (dom::Node* child : node->ChildNodes()) {
        if (!ChildTypeAllowed(dom::NodeType::kDocument, child->node_type())) {
          ThrowDomException(ctx,
                            "HierarchyRequestError",
                            "Range.insertNode: invalid fragment child for a Document");
          return false;
        }
        if (child->node_type() == dom::NodeType::kElement) {
          ++elements;
        } else if (child->node_type() == dom::NodeType::kDocumentType) {
          ++doctypes;
        }
      }
    }
    if (elements > 1 || doctypes > 1) {
      ThrowDomException(ctx,
                        "HierarchyRequestError",
                        "Range.insertNode: a Document can have only one element and one doctype");
      return false;
    }
  }
  return true;
}

bool ReadNodeAndOffset(
    JSContext* ctx, int argc, JSValueConst* argv, dom::Node** node, std::size_t* offset)
{
  if (argc < 2) {
    JS_ThrowTypeError(ctx, "Range method requires a node and an offset");
    return false;
  }
  *node = UnwrapNode(ctx, argv[0]);
  if (*node == nullptr) {
    JS_ThrowTypeError(ctx, "Range method argument is not a Node");
    return false;
  }
  std::uint32_t value = 0;
  if (JS_ToUint32(ctx, &value, argv[1]) < 0) {
    return false;
  }
  *offset = value;
  return true;
}

// Shared validation for setStart/setEnd/selectNode/selectNodeContents and the
// *Before/*After helpers.
bool ValidateBoundary(JSContext* ctx,
                      Impl& impl,
                      const dom::Node& node,
                      std::size_t offset,
                      dom::Document* document,
                      bool allow_doctype)
{
  if (!allow_doctype && node.node_type() == dom::NodeType::kDocumentType) {
    ThrowDomException(ctx, "InvalidNodeTypeError", "Range: a doctype cannot be a boundary point");
    return false;
  }
  if (DocumentOf(impl, node) != document) {
    ThrowDomException(ctx, "WrongDocumentError", "Range: the node belongs to a different document");
    return false;
  }
  if (offset > NodeLength(node)) {
    ThrowDomException(
        ctx, "IndexSizeError", "Range: boundary offset is larger than the node's length");
    return false;
  }
  return true;
}

void SetBoundary(RangeWrapper& wrapper, bool is_start, dom::Node* node, std::size_t offset)
{
  if (is_start) {
    const bool same_root = RootOf(node) == RootOf(wrapper.end);
    if (!same_root || ComparePoints(node, offset, wrapper.end, wrapper.end_offset) > 0) {
      wrapper.end = node;
      wrapper.end_offset = offset;
    }
    wrapper.start = node;
    wrapper.start_offset = offset;
  } else {
    const bool same_root = RootOf(node) == RootOf(wrapper.start);
    if (!same_root || ComparePoints(node, offset, wrapper.start, wrapper.start_offset) < 0) {
      wrapper.start = node;
      wrapper.start_offset = offset;
    }
    wrapper.end = node;
    wrapper.end_offset = offset;
  }
}

JSValue SetBoundaryFromArguments(
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, bool is_start)
{
  auto* wrapper = UnwrapRange(this_val);
  if (wrapper == nullptr || wrapper->impl == nullptr) {
    return JS_ThrowTypeError(ctx, "not a Range");
  }
  dom::Node* node = nullptr;
  std::size_t offset = 0;
  if (!ReadNodeAndOffset(ctx, argc, argv, &node, &offset)) {
    return JS_EXCEPTION;
  }
  if (!ValidateBoundary(ctx, *wrapper->impl, *node, offset, wrapper->document, false)) {
    return JS_EXCEPTION;
  }
  SetBoundary(*wrapper, is_start, node, offset);
  return JS_UNDEFINED;
}

JSValue SetBoundaryRelative(
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, bool is_start, bool after)
{
  auto* wrapper = UnwrapRange(this_val);
  if (wrapper == nullptr || wrapper->impl == nullptr) {
    return JS_ThrowTypeError(ctx, "not a Range");
  }
  dom::Node* node = argc >= 1 ? UnwrapNode(ctx, argv[0]) : nullptr;
  if (node == nullptr) {
    return JS_ThrowTypeError(ctx, "Range method requires a Node");
  }
  dom::Node* parent = node->parent();
  if (parent == nullptr) {
    return ThrowDomException(ctx, "InvalidNodeTypeError", "Range: the node has no parent");
  }
  const std::size_t offset = IndexOf(*node) + (after ? 1 : 0);
  if (!ValidateBoundary(ctx, *wrapper->impl, *parent, offset, wrapper->document, true)) {
    return JS_EXCEPTION;
  }
  SetBoundary(*wrapper, is_start, parent, offset);
  return JS_UNDEFINED;
}

JSValue InsertNodeIntoRange(JSContext* ctx, RangeWrapper& wrapper, dom::Node* node)
{
  Impl& impl = *wrapper.impl;
  dom::Node* start = wrapper.start;
  if (start == nullptr) {
    return ThrowDomException(ctx, "HierarchyRequestError", "Range: the range has no start node");
  }
  if (start->node_type() == dom::NodeType::kComment ||
      (start->node_type() == dom::NodeType::kText && start->parent() == nullptr) || start == node) {
    return ThrowDomException(
        ctx, "HierarchyRequestError", "Range.insertNode: the start node cannot receive a node");
  }

  dom::Node* reference = nullptr;
  if (start->node_type() == dom::NodeType::kText) {
    reference = start;
  } else {
    reference = start->child_at(wrapper.start_offset);
  }
  dom::Node* parent = reference != nullptr ? reference->parent() : start;
  if (parent == nullptr) {
    return ThrowDomException(
        ctx, "HierarchyRequestError", "Range.insertNode: the start node has no parent");
  }
  if (!CheckPreInsertion(ctx, parent, node, reference)) {
    return JS_EXCEPTION;
  }

  const bool was_collapsed =
      wrapper.start == wrapper.end && wrapper.start_offset == wrapper.end_offset;

  if (start->node_type() == dom::NodeType::kText) {
    dom::Node* split = SplitTextNode(impl, *static_cast<dom::Text*>(start), wrapper.start_offset);
    if (split == nullptr) {
      return ThrowDomException(
          ctx, "HierarchyRequestError", "Range.insertNode: splitting the start text node failed");
    }
    reference = split;
  }
  if (node == reference) {
    reference = SiblingOf(reference, +1);
  }
  if (node->parent() != nullptr) {
    dom::Node* previous_parent = node->parent();
    std::unique_ptr<dom::Node> removed = previous_parent->RemoveChild(node);
    if (removed != nullptr) {
      impl.TakeOwnership(node, std::move(removed));
      impl.RecordChildListMutation(previous_parent, {}, {node});
      impl.MarkDomDirty(previous_parent);
    }
  }

  std::size_t new_offset = reference == nullptr ? NodeLength(*parent) : IndexOf(*reference);
  new_offset += node->node_type() == dom::NodeType::kDocumentFragment ? NodeLength(*node) : 1;

  JSValue inserted = InsertNodeChecked(ctx, &impl, parent, node, reference, "Range.insertNode");
  if (JS_IsException(inserted)) {
    return inserted;
  }
  JS_FreeValue(ctx, inserted);

  if (was_collapsed) {
    wrapper.end = parent;
    wrapper.end_offset = new_offset;
  }
  return JS_UNDEFINED;
}

bool HasNonTextPartial(const RangeState& range)
{
  for (dom::Node* node = range.start_node; node != nullptr; node = node->parent()) {
    if (!IsInclusiveAncestor(node, range.end_node) && node->node_type() != dom::NodeType::kText) {
      return true;
    }
  }
  for (dom::Node* node = range.end_node; node != nullptr; node = node->parent()) {
    if (!IsInclusiveAncestor(node, range.start_node) && node->node_type() != dom::NodeType::kText) {
      return true;
    }
  }
  return false;
}

// --- Getters ---------------------------------------------------------------

JSValue RangeGetStartContainer(JSContext* ctx, JSValueConst this_val)
{
  auto* wrapper = UnwrapRange(this_val);
  if (wrapper == nullptr) {
    return JS_ThrowTypeError(ctx, "not a Range");
  }
  return wrapper->impl->WrapNode(wrapper->start);
}

JSValue RangeGetEndContainer(JSContext* ctx, JSValueConst this_val)
{
  auto* wrapper = UnwrapRange(this_val);
  if (wrapper == nullptr) {
    return JS_ThrowTypeError(ctx, "not a Range");
  }
  return wrapper->impl->WrapNode(wrapper->end);
}

JSValue RangeGetStartOffset(JSContext* ctx, JSValueConst this_val)
{
  auto* wrapper = UnwrapRange(this_val);
  if (wrapper == nullptr) {
    return JS_ThrowTypeError(ctx, "not a Range");
  }
  return JS_NewUint32(ctx, static_cast<std::uint32_t>(wrapper->start_offset));
}

JSValue RangeGetEndOffset(JSContext* ctx, JSValueConst this_val)
{
  auto* wrapper = UnwrapRange(this_val);
  if (wrapper == nullptr) {
    return JS_ThrowTypeError(ctx, "not a Range");
  }
  return JS_NewUint32(ctx, static_cast<std::uint32_t>(wrapper->end_offset));
}

JSValue RangeGetCollapsed(JSContext* ctx, JSValueConst this_val)
{
  auto* wrapper = UnwrapRange(this_val);
  if (wrapper == nullptr) {
    return JS_ThrowTypeError(ctx, "not a Range");
  }
  return JS_NewBool(ctx,
                    wrapper->start == wrapper->end && wrapper->start_offset == wrapper->end_offset);
}

JSValue RangeGetCommonAncestorContainer(JSContext* ctx, JSValueConst this_val)
{
  auto* wrapper = UnwrapRange(this_val);
  if (wrapper == nullptr || wrapper->impl == nullptr) {
    return JS_ThrowTypeError(ctx, "not a Range");
  }
  dom::Node* common = GetCommonAncestor(wrapper->start, wrapper->end);
  if (common == nullptr) {
    return JS_ThrowTypeError(ctx, "Range boundary containers have no common ancestor");
  }
  return wrapper->impl->WrapNode(common);
}

// --- Methods ---------------------------------------------------------------

JSValue RangeSetStart(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  return SetBoundaryFromArguments(ctx, this_val, argc, argv, true);
}

JSValue RangeSetEnd(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  return SetBoundaryFromArguments(ctx, this_val, argc, argv, false);
}

JSValue RangeSetStartBefore(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  return SetBoundaryRelative(ctx, this_val, argc, argv, true, false);
}

JSValue RangeSetStartAfter(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  return SetBoundaryRelative(ctx, this_val, argc, argv, true, true);
}

JSValue RangeSetEndBefore(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  return SetBoundaryRelative(ctx, this_val, argc, argv, false, false);
}

JSValue RangeSetEndAfter(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  return SetBoundaryRelative(ctx, this_val, argc, argv, false, true);
}

JSValue RangeCollapse(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  auto* wrapper = UnwrapRange(this_val);
  if (wrapper == nullptr) {
    return JS_ThrowTypeError(ctx, "not a Range");
  }
  int to_start = 0;
  if (argc >= 1) {
    to_start = JS_ToBool(ctx, argv[0]);
    if (to_start < 0) {
      return JS_EXCEPTION;
    }
  }
  if (to_start != 0) {
    wrapper->end = wrapper->start;
    wrapper->end_offset = wrapper->start_offset;
  } else {
    wrapper->start = wrapper->end;
    wrapper->start_offset = wrapper->end_offset;
  }
  return JS_UNDEFINED;
}

JSValue RangeSelectNode(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  auto* wrapper = UnwrapRange(this_val);
  if (wrapper == nullptr || wrapper->impl == nullptr) {
    return JS_ThrowTypeError(ctx, "not a Range");
  }
  dom::Node* node = argc >= 1 ? UnwrapNode(ctx, argv[0]) : nullptr;
  if (node == nullptr) {
    return JS_ThrowTypeError(ctx, "Range.selectNode requires a Node");
  }
  dom::Node* parent = node->parent();
  if (parent == nullptr) {
    return ThrowDomException(
        ctx, "InvalidNodeTypeError", "Range.selectNode: the node has no parent");
  }
  const std::size_t index = IndexOf(*node);
  if (!ValidateBoundary(ctx, *wrapper->impl, *parent, index, wrapper->document, true) ||
      !ValidateBoundary(ctx, *wrapper->impl, *parent, index + 1, wrapper->document, true)) {
    return JS_EXCEPTION;
  }
  wrapper->start = parent;
  wrapper->start_offset = index;
  wrapper->end = parent;
  wrapper->end_offset = index + 1;
  return JS_UNDEFINED;
}

JSValue RangeSelectNodeContents(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  auto* wrapper = UnwrapRange(this_val);
  if (wrapper == nullptr || wrapper->impl == nullptr) {
    return JS_ThrowTypeError(ctx, "not a Range");
  }
  dom::Node* node = argc >= 1 ? UnwrapNode(ctx, argv[0]) : nullptr;
  if (node == nullptr) {
    return JS_ThrowTypeError(ctx, "Range.selectNodeContents requires a Node");
  }
  if (node->node_type() == dom::NodeType::kDocumentType) {
    return ThrowDomException(
        ctx, "InvalidNodeTypeError", "Range.selectNodeContents: the node is a doctype");
  }
  if (!ValidateBoundary(ctx, *wrapper->impl, *node, 0, wrapper->document, false)) {
    return JS_EXCEPTION;
  }
  wrapper->start = node;
  wrapper->start_offset = 0;
  wrapper->end = node;
  wrapper->end_offset = NodeLength(*node);
  return JS_UNDEFINED;
}

JSValue RangeCloneRange(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  auto* wrapper = UnwrapRange(this_val);
  if (wrapper == nullptr || wrapper->impl == nullptr) {
    return JS_ThrowTypeError(ctx, "not a Range");
  }
  return MakeRangeObject(ctx,
                         *wrapper->impl,
                         wrapper->document,
                         wrapper->start,
                         wrapper->start_offset,
                         wrapper->end,
                         wrapper->end_offset,
                         wrapper->impl->range_proto);
}

JSValue
RangeCompareBoundaryPoints(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  auto* wrapper = UnwrapRange(this_val);
  if (wrapper == nullptr || wrapper->impl == nullptr) {
    return JS_ThrowTypeError(ctx, "not a Range");
  }
  if (argc < 2) {
    return JS_ThrowTypeError(ctx, "compareBoundaryPoints requires how and sourceRange");
  }
  std::uint32_t how = 0;
  if (JS_ToUint32(ctx, &how, argv[0]) < 0) {
    return JS_EXCEPTION;
  }
  if (how > 3) {
    return ThrowDomException(
        ctx, "NotSupportedError", "compareBoundaryPoints: unknown comparison mode");
  }
  auto* source = UnwrapRange(argv[1]);
  if (source == nullptr || source->impl == nullptr) {
    return JS_ThrowTypeError(ctx, "compareBoundaryPoints: sourceRange is not a Range");
  }
  if (RootOf(wrapper->start) != RootOf(source->start)) {
    return ThrowDomException(
        ctx, "WrongDocumentError", "compareBoundaryPoints: ranges are in different trees");
  }

  const dom::Node* this_node = nullptr;
  std::size_t this_offset = 0;
  const dom::Node* source_node = nullptr;
  std::size_t source_offset = 0;
  switch (how) {
  case 0: // START_TO_START
    this_node = wrapper->start;
    this_offset = wrapper->start_offset;
    source_node = source->start;
    source_offset = source->start_offset;
    break;
  case 1: // START_TO_END
    this_node = wrapper->end;
    this_offset = wrapper->end_offset;
    source_node = source->start;
    source_offset = source->start_offset;
    break;
  case 2: // END_TO_END
    this_node = wrapper->end;
    this_offset = wrapper->end_offset;
    source_node = source->end;
    source_offset = source->end_offset;
    break;
  default: // END_TO_START
    this_node = wrapper->start;
    this_offset = wrapper->start_offset;
    source_node = source->end;
    source_offset = source->end_offset;
    break;
  }
  return JS_NewInt32(ctx, ComparePoints(this_node, this_offset, source_node, source_offset));
}

JSValue RangeComparePoint(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  auto* wrapper = UnwrapRange(this_val);
  if (wrapper == nullptr || wrapper->impl == nullptr) {
    return JS_ThrowTypeError(ctx, "not a Range");
  }
  dom::Node* node = nullptr;
  std::size_t offset = 0;
  if (!ReadNodeAndOffset(ctx, argc, argv, &node, &offset)) {
    return JS_EXCEPTION;
  }
  if (RootOf(node) != RootOf(wrapper->start)) {
    return ThrowDomException(
        ctx, "WrongDocumentError", "comparePoint: the node is in a different tree");
  }
  if (node->node_type() == dom::NodeType::kDocumentType) {
    return ThrowDomException(ctx, "InvalidNodeTypeError", "comparePoint: the node is a doctype");
  }
  if (offset > NodeLength(*node)) {
    return ThrowDomException(
        ctx, "IndexSizeError", "comparePoint: offset is larger than the node's length");
  }
  if (ComparePoints(node, offset, wrapper->start, wrapper->start_offset) < 0) {
    return JS_NewInt32(ctx, -1);
  }
  if (ComparePoints(node, offset, wrapper->end, wrapper->end_offset) > 0) {
    return JS_NewInt32(ctx, 1);
  }
  return JS_NewInt32(ctx, 0);
}

JSValue RangeIsPointInRange(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  auto* wrapper = UnwrapRange(this_val);
  if (wrapper == nullptr) {
    return JS_ThrowTypeError(ctx, "not a Range");
  }
  dom::Node* node = nullptr;
  std::size_t offset = 0;
  if (!ReadNodeAndOffset(ctx, argc, argv, &node, &offset)) {
    return JS_EXCEPTION;
  }
  if (RootOf(node) != RootOf(wrapper->start)) {
    return JS_FALSE;
  }
  if (node->node_type() == dom::NodeType::kDocumentType) {
    return ThrowDomException(ctx, "InvalidNodeTypeError", "isPointInRange: the node is a doctype");
  }
  if (offset > NodeLength(*node)) {
    return ThrowDomException(
        ctx, "IndexSizeError", "isPointInRange: offset is larger than the node's length");
  }
  const bool inside = ComparePoints(node, offset, wrapper->start, wrapper->start_offset) >= 0 &&
                      ComparePoints(node, offset, wrapper->end, wrapper->end_offset) <= 0;
  return JS_NewBool(ctx, inside);
}

JSValue RangeIntersectsNode(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  auto* wrapper = UnwrapRange(this_val);
  if (wrapper == nullptr) {
    return JS_ThrowTypeError(ctx, "not a Range");
  }
  dom::Node* node = argc >= 1 ? UnwrapNode(ctx, argv[0]) : nullptr;
  if (node == nullptr) {
    return JS_ThrowTypeError(ctx, "intersectsNode requires a Node");
  }
  if (RootOf(node) != RootOf(wrapper->start)) {
    return JS_FALSE;
  }
  dom::Node* parent = node->parent();
  if (parent == nullptr) {
    return JS_TRUE;
  }
  const std::size_t offset = IndexOf(*node);
  const bool intersects =
      ComparePoints(parent, offset, wrapper->end, wrapper->end_offset) < 0 &&
      ComparePoints(parent, offset + 1, wrapper->start, wrapper->start_offset) > 0;
  return JS_NewBool(ctx, intersects);
}

JSValue RangeToString(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  auto* wrapper = UnwrapRange(this_val);
  if (wrapper == nullptr) {
    return JS_ThrowTypeError(ctx, "not a Range");
  }
  const std::string text = StringifyRange(StateOf(*wrapper));
  return JS_NewStringLen(ctx, text.data(), text.size());
}

JSValue
RangeCloneContents(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  auto* wrapper = UnwrapRange(this_val);
  if (wrapper == nullptr || wrapper->impl == nullptr) {
    return JS_ThrowTypeError(ctx, "not a Range");
  }
  const RangeState range = StateOf(*wrapper);
  if (ContainsDoctype(range)) {
    return ThrowDomException(
        ctx, "HierarchyRequestError", "cloneContents: the range contains a doctype");
  }
  std::unique_ptr<dom::DocumentFragment> fragment = CloneRangeContents(*wrapper->impl, range);
  dom::DocumentFragment* raw = fragment.get();
  wrapper->impl->TakeOwnership(raw, std::move(fragment));
  return wrapper->impl->WrapNode(raw);
}

JSValue
RangeExtractContents(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  auto* wrapper = UnwrapRange(this_val);
  if (wrapper == nullptr || wrapper->impl == nullptr) {
    return JS_ThrowTypeError(ctx, "not a Range");
  }
  RangeState range = StateOf(*wrapper);
  if (ContainsDoctype(range)) {
    return ThrowDomException(
        ctx, "HierarchyRequestError", "extractContents: the range contains a doctype");
  }
  std::unique_ptr<dom::DocumentFragment> fragment = ExtractRangeContents(*wrapper->impl, range);
  StoreState(*wrapper, range);
  dom::DocumentFragment* raw = fragment.get();
  wrapper->impl->TakeOwnership(raw, std::move(fragment));
  return wrapper->impl->WrapNode(raw);
}

JSValue
RangeDeleteContents(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  auto* wrapper = UnwrapRange(this_val);
  if (wrapper == nullptr || wrapper->impl == nullptr) {
    return JS_ThrowTypeError(ctx, "not a Range");
  }
  RangeState range = StateOf(*wrapper);
  DeleteRangeContents(*wrapper->impl, range);
  StoreState(*wrapper, range);
  return JS_UNDEFINED;
}

JSValue RangeInsertNode(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  auto* wrapper = UnwrapRange(this_val);
  if (wrapper == nullptr || wrapper->impl == nullptr) {
    return JS_ThrowTypeError(ctx, "not a Range");
  }
  dom::Node* node = argc >= 1 ? UnwrapNode(ctx, argv[0]) : nullptr;
  if (node == nullptr) {
    return JS_ThrowTypeError(ctx, "insertNode requires a Node");
  }
  return InsertNodeIntoRange(ctx, *wrapper, node);
}

JSValue RangeSurroundContents(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  auto* wrapper = UnwrapRange(this_val);
  if (wrapper == nullptr || wrapper->impl == nullptr) {
    return JS_ThrowTypeError(ctx, "not a Range");
  }
  dom::Node* new_parent = argc >= 1 ? UnwrapNode(ctx, argv[0]) : nullptr;
  if (new_parent == nullptr) {
    return JS_ThrowTypeError(ctx, "surroundContents requires a Node");
  }
  if (new_parent->node_type() == dom::NodeType::kDocument ||
      new_parent->node_type() == dom::NodeType::kDocumentType ||
      new_parent->node_type() == dom::NodeType::kDocumentFragment) {
    return ThrowDomException(ctx,
                             "InvalidNodeTypeError",
                             "surroundContents: the new parent cannot be a document, doctype "
                             "or fragment");
  }

  Impl& impl = *wrapper->impl;
  RangeState range = StateOf(*wrapper);
  if (HasNonTextPartial(range)) {
    return ThrowDomException(
        ctx, "InvalidStateError", "surroundContents: the range partially selects a non-Text node");
  }

  std::unique_ptr<dom::DocumentFragment> fragment = ExtractRangeContents(impl, range);
  StoreState(*wrapper, range);
  dom::DocumentFragment* fragment_raw = fragment.get();

  while (new_parent->first_child() != nullptr) {
    dom::Node* child = new_parent->first_child();
    std::unique_ptr<dom::Node> removed = new_parent->RemoveChild(child);
    if (removed != nullptr) {
      impl.TakeOwnership(child, std::move(removed));
      impl.RecordChildListMutation(new_parent, {}, {child});
    }
  }
  impl.MarkDomDirty(new_parent);

  JSValue inserted = InsertNodeIntoRange(ctx, *wrapper, new_parent);
  if (JS_IsException(inserted)) {
    return inserted;
  }
  JS_FreeValue(ctx, inserted);

  if (IsCharacterData(*new_parent)) {
    return ThrowDomException(ctx,
                             "HierarchyRequestError",
                             "surroundContents: a CharacterData node cannot receive the contents");
  }
  while (fragment_raw->first_child() != nullptr) {
    dom::Node* child = fragment_raw->first_child();
    std::unique_ptr<dom::Node> removed = fragment_raw->RemoveChild(child);
    if (removed == nullptr || !new_parent->AppendChild(std::move(removed))) {
      return ThrowDomException(
          ctx, "HierarchyRequestError", "surroundContents: the contents could not be moved");
    }
    impl.RecordChildListMutation(fragment_raw, {}, {child});
    impl.RecordChildListMutation(new_parent, {child}, {});
  }
  impl.MarkDomDirty(new_parent);

  dom::Node* parent = new_parent->parent();
  if (parent == nullptr) {
    return ThrowDomException(
        ctx, "InvalidNodeTypeError", "surroundContents: the new parent has no parent");
  }
  const std::size_t index = IndexOf(*new_parent);
  wrapper->start = parent;
  wrapper->start_offset = index;
  wrapper->end = parent;
  wrapper->end_offset = index + 1;
  return JS_UNDEFINED;
}

JSValue
RangeDetach(JSContext* /*ctx*/, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/)
{
  // DOM §5.5: detach() does nothing; the method is preserved for compatibility.
  return JS_UNDEFINED;
}

// Range geometry lives in CSSOM View and needs the layout tree.  The methods
// exist so pages fail loudly instead of receiving fabricated rectangles.
JSValue RangeGetBoundingClientRect(JSContext* ctx,
                                   JSValueConst /*this_val*/,
                                   int /*argc*/,
                                   JSValueConst* /*argv*/)
{
  return ThrowDomException(
      ctx,
      "NotSupportedError",
      "Range.getBoundingClientRect is not implemented: range geometry needs the layout tree");
}

JSValue
RangeGetClientRects(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/)
{
  return ThrowDomException(
      ctx,
      "NotSupportedError",
      "Range.getClientRects is not implemented: range geometry needs the layout tree");
}

// Range constructor: new Range() is initialised to (document, 0), where the
// document is the current global's associated Document (DOM §5.5).
JSValue
RangeConstructor(JSContext* ctx, JSValueConst new_target, int /*argc*/, JSValueConst* /*argv*/)
{
  Impl* impl = ImplFor(ctx, JS_UNDEFINED);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "Range: no associated document");
  }
  JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
  if (JS_IsException(prototype)) {
    return prototype;
  }
  JSValue range = MakeRangeObject(
      ctx, *impl, &impl->document, &impl->document, 0, &impl->document, 0, prototype);
  JS_FreeValue(ctx, prototype);
  return range;
}

} // namespace

void EnsureRangeClassRegistered(JSRuntime* rt)
{
  std::lock_guard<std::mutex> lock(g_range_class_mutex);
  JS_NewClassID(rt, &g_range_class_id);
  if (!JS_IsRegisteredClass(rt, g_range_class_id)) {
    JSClassDef def;
    std::memset(&def, 0, sizeof(def));
    def.class_name = "Range";
    def.finalizer = &RangeFinalizer;
    JS_NewClass(rt, g_range_class_id, &def);
  }
}

RangeWrapper* UnwrapRange(JSValueConst value)
{
  return static_cast<RangeWrapper*>(JS_GetOpaque(value, g_range_class_id));
}

JSValue MakeRangeObject(JSContext* ctx,
                        Impl& impl,
                        dom::Document* document,
                        dom::Node* start,
                        std::size_t start_offset,
                        dom::Node* end,
                        std::size_t end_offset,
                        JSValueConst prototype)
{
  auto* wrapper = new RangeWrapper{&impl, document, start, start_offset, end, end_offset};
  JSValue object = JS_NewObjectClass(ctx, g_range_class_id);
  if (JS_IsException(object)) {
    delete wrapper;
    return object;
  }
  JS_SetOpaque(object, wrapper);
  JS_SetPrototype(ctx, object, JS_IsUndefined(prototype) ? impl.range_proto : prototype);
  return object;
}

void DefineRangePrototype(JSContext* ctx, Impl& impl)
{
  impl.range_proto = JS_NewObject(ctx);

  static const std::array<JSCFunctionListEntry, 23> kMethods = {{
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
  }};
  JS_SetPropertyFunctionList(
      ctx, impl.range_proto, kMethods.data(), static_cast<int>(kMethods.size()));

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

  static constexpr std::array<std::pair<const char*, std::uint32_t>, 4> kConstants = {{
      {"START_TO_START", 0},
      {"START_TO_END", 1},
      {"END_TO_END", 2},
      {"END_TO_START", 3},
  }};

  JSValue constructor =
      JS_NewCFunction2(ctx, RangeConstructor, "Range", 0, JS_CFUNC_constructor, 0);
  JS_SetPropertyStr(ctx, constructor, "prototype", JS_DupValue(ctx, impl.range_proto));
  JS_SetPropertyStr(ctx, impl.range_proto, "constructor", JS_DupValue(ctx, constructor));
  for (const auto& [name, value] : kConstants) {
    JS_SetPropertyStr(ctx, impl.range_proto, name, JS_NewUint32(ctx, value));
    JS_SetPropertyStr(ctx, constructor, name, JS_NewUint32(ctx, value));
  }
  JSValue global = JS_GetGlobalObject(ctx);
  JS_SetPropertyStr(ctx, global, "Range", constructor);
  JS_FreeValue(ctx, global);
}

} // namespace neko::javascript
