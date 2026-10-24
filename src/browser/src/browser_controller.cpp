// BrowserController implementation.
//
// The controller is one class but several responsibilities; the method bodies
// live in .inc fragments included below so each subsystem is reviewable on its
// own: tabs, navigation (user interaction, forms, focus), fetch/content
// routing, and persistence/DevTools.  They stay in this translation unit
// because member bodies call the file-local helpers in the anonymous
// namespace above; promoting them to separate TUs requires externalising
// those helpers first (same follow-up as layout.cpp).
#include "neko/browser/browser_controller.h"

#include "neko/base/logging.h"
#include "neko/base/memory.h"
#include "neko/base/string_util.h"
#include "neko/base/thread_pool.h"
#include "neko/browser/hyperlink.h"
#include "neko/browser/page_scripts.h"
#include "neko/browser/preferences_keys.h"
#include "neko/browser/renderer_host.h"
#include "neko/browser/search_engines.h"
#include "neko/css/parser.h"
#include "neko/dom/element.h"
#include "neko/dom/query.h"
#include "neko/html/form_validation.h"
#include "neko/html/input_value.h"
#include "neko/image/image.h"
#include "neko/layout/form_control.h"
#include "neko/network/http.h"
#include "neko/paint/rasterizer.h"
#include "neko/platform/executable_path.h"
#include "neko/security/origin.h"
#include "neko/security/site.h"
#include "neko/storage/file_util.h"
#include "neko/url/url.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <future>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace neko::browser {
namespace {

std::string_view Trim(std::string_view s)
{
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
    s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
    s.remove_suffix(1);
  return s;
}

// True when |key| is text to insert into a focused text control.  DOM key
// values are either a single character / IME commit string or a named key such
// as "ArrowUp"; named keys must never be inserted literally.
bool IsPrintableKey(std::string_view key)
{
  if (key.empty() || static_cast<unsigned char>(key.front()) < 0x20U) {
    return false;
  }
  static constexpr std::string_view kNamedKeys[] = {
      "ArrowLeft",    "ArrowRight", "ArrowUp",    "ArrowDown", "Home",        "End",
      "PageUp",       "PageDown",   "Insert",     "Delete",    "Backspace",   "Enter",
      "Escape",       "Tab",        "Shift",      "Control",   "Alt",         "Meta",
      "CapsLock",     "NumLock",    "ScrollLock", "Pause",     "PrintScreen", "ContextMenu",
      "Unidentified", "F1",         "F2",         "F3",        "F4",          "F5",
      "F6",           "F7",         "F8",         "F9",        "F10",         "F11",
      "F12"};
  for (const std::string_view named : kNamedKeys) {
    if (named == key) {
      return false;
    }
  }
  return true;
}

// Resolves a possibly-relative reference against a base page URL.  Handles
// file:// / bare local path bases (which the URL parser rejects) by string
// concatenation, and leaves absolute references (those carrying a scheme)
// unchanged — see browser::ResolveReference.
std::string ResolveUrlAgainstBase(std::string_view ref, std::string_view base_url)
{
  if (ref.empty()) {
    return std::string(base_url);
  }
  const std::optional<std::string> resolved = ResolveReference(ref, base_url);
  return resolved.has_value() ? resolved.value() : std::string(ref);
}

// A submission request being executed (direct user activation or a scripted
// form.submit()/requestSubmit()).  |scripted| skips interactive validation and
// the submit event (HTMLFormElement.submit()).
struct FormSubmissionRequest
{
  dom::Element* form = nullptr;
  dom::Element* submitter = nullptr;
  bool scripted = false;
};

// Normalizes line breaks the way the form entry-list conversion does (WHATWG
// HTML §4.10.22.6): every CR not followed by LF, and every LF not preceded by
// CR, becomes CR LF.  Existing CR LF pairs stay a single pair.
std::string NormalizeNewlines(std::string_view value)
{
  std::string out;
  out.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    if (c == '\r') {
      out += "\r\n";
      if (i + 1 < value.size() && value[i + 1] == '\n') {
        ++i; // the LF is part of this CR LF pair
      }
    } else if (c == '\n') {
      out += "\r\n";
    } else {
      out += c;
    }
  }
  return out;
}

// The three form submission encodings (HTML §4.10.19.6 "enctype").
enum class FormEnctype
{
  kUrlEncoded,
  kMultipart,
  kTextPlain,
};

// Parses an enctype/formenctype attribute's state.  The missing value default
// and the invalid value default are both application/x-www-form-urlencoded
// (HTML §4.10.19.6).
FormEnctype ParseEnctype(std::string_view raw)
{
  const std::string value = base::ToLower(std::string(Trim(raw)));
  if (value == "multipart/form-data") {
    return FormEnctype::kMultipart;
  }
  if (value == "text/plain") {
    return FormEnctype::kTextPlain;
  }
  return FormEnctype::kUrlEncoded;
}

// Appends |query| to |url|'s query component, keeping any fragment last.
std::string AppendQuery(std::string url, std::string_view query)
{
  const std::size_t hash = url.find('#');
  std::string fragment;
  if (hash != std::string::npos) {
    fragment = url.substr(hash);
    url.resize(hash);
  }
  url += url.find('?') == std::string::npos ? '?' : '&';
  url += query;
  url += fragment;
  return url;
}

// A fresh multipart/form-data boundary.  The random part makes a collision
// with user data (which would corrupt the body) unpredictable; only RFC 2046
// boundary characters are used.
std::string GenerateMultipartBoundary()
{
  static std::atomic<std::uint64_t> counter{0};
  std::random_device random;
  const std::uint64_t noise =
      (static_cast<std::uint64_t>(random()) << 32) ^ static_cast<std::uint64_t>(random());
  const std::uint64_t now =
      static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  const std::uint64_t value = noise ^ (now << 16) ^ counter.fetch_add(1);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string boundary = "----NekoFormBoundary";
  for (int shift = 60; shift >= 0; shift -= 4) {
    boundary += kHex[(value >> static_cast<unsigned>(shift)) & 0xFU];
  }
  return boundary;
}

// Escapes a multipart field name (HTML §4.10.22.8): LF -> %0A, CR -> %0D,
// quote -> %22.  No other escaping is permitted.
std::string EscapeMultipartName(std::string_view name)
{
  std::string out;
  out.reserve(name.size());
  for (const char c : name) {
    switch (c) {
    case '\n':
      out += "%0A";
      break;
    case '\r':
      out += "%0D";
      break;
    case '"':
      out += "%22";
      break;
    default:
      out += c;
      break;
    }
  }
  return out;
}

// application/x-www-form-urlencoded serialization of an entry list (the
// engine's existing serializer: UTF-8 percent-encoding, spaces as %20 rather
// than the URL standard's '+').  File entries are dropped: without the File
// API they carry no file name or content.
std::string EncodeUrlEncoded(const std::vector<html::FormEntry>& entries)
{
  std::string out;
  for (const html::FormEntry& entry : entries) {
    if (entry.is_file) {
      continue;
    }
    if (!out.empty()) {
      out += '&';
    }
    out += url::PercentEncode(NormalizeNewlines(entry.name)) + '=' +
           url::PercentEncode(NormalizeNewlines(entry.value));
  }
  return out;
}

// text/plain encoding (HTML §4.10.22.9): name, '=', value, CR LF per entry.  No
// percent-encoding; line breaks in names and values are normalized first.
std::string EncodePlainText(const std::vector<html::FormEntry>& entries)
{
  std::string out;
  for (const html::FormEntry& entry : entries) {
    if (entry.is_file) {
      continue;
    }
    out += NormalizeNewlines(entry.name);
    out += '=';
    out += NormalizeNewlines(entry.value);
    out += "\r\n";
  }
  return out;
}

// multipart/form-data encoding (HTML §4.10.22.8 / RFC 7578).  Non-file parts
// carry a Content-Disposition header only; values and names are CRLF
// normalized, names additionally escaped per the spec.
std::string EncodeMultipart(const std::vector<html::FormEntry>& entries, std::string_view boundary)
{
  std::string out;
  for (const html::FormEntry& entry : entries) {
    if (entry.is_file) {
      continue;
    }
    out += "--";
    out += boundary;
    out += "\r\nContent-Disposition: form-data; name=\"";
    out += EscapeMultipartName(NormalizeNewlines(entry.name));
    out += "\"\r\n\r\n";
    out += NormalizeNewlines(entry.value);
    out += "\r\n";
  }
  out += "--";
  out += boundary;
  out += "--\r\n";
  return out;
}

// Unchecks every other radio with the same name in |element|'s document.
// The spec scopes a radio button group to the shared form owner; this engine
// scopes it to the document (documented approximation).
void UncheckRadioGroup(dom::Element& element)
{
  const std::string_view name = element.GetAttribute("name").value_or("");
  if (name.empty()) {
    return; // a nameless radio is its own group (HTML §4.10.5.3)
  }
  const dom::Node* root = &element;
  while (root->parent() != nullptr) {
    root = root->parent();
  }
  std::vector<dom::Node*> stack{const_cast<dom::Node*>(root)};
  while (!stack.empty()) {
    dom::Node* node = stack.back();
    stack.pop_back();
    if (node->node_type() == dom::NodeType::kElement) {
      auto* el = static_cast<dom::Element*>(node);
      if (el != &element && el->tag_name() == "input" &&
          layout::ClassifyFormControl(*el) == layout::FormControlKind::kRadio &&
          el->GetAttribute("name").value_or("") == name) {
        el->RemoveAttribute("checked");
      }
    }
    for (dom::Node* child : node->ChildNodes()) {
      stack.push_back(child);
    }
  }
}

// Toggles a checkable input's checkedness (WHATWG §4.10.5.3 activation
// behaviour).  Returns true when |element| is a checkable control — including
// the disabled case, which is handled without changing state.  Checkedness is
// the `checked` content attribute in this engine (the JS `checked` binding
// and :checked read the same source).  A checked radio stays checked on
// activation; checking one unchecks its same-name group.
bool ToggleCheckableInput(dom::Element& element)
{
  const layout::FormControlKind kind = layout::ClassifyFormControl(element);
  if (kind != layout::FormControlKind::kCheckbox && kind != layout::FormControlKind::kRadio) {
    return false;
  }
  if (element.HasAttribute("disabled")) {
    return true;
  }
  const bool was_checked = element.HasAttribute("checked");
  if (kind == layout::FormControlKind::kRadio) {
    if (!was_checked) {
      element.SetAttribute("checked", "checked");
      UncheckRadioGroup(element);
    }
    return true;
  }
  if (was_checked) {
    element.RemoveAttribute("checked");
  } else {
    element.SetAttribute("checked", "checked");
  }
  return true;
}

// Fires the post-activation events of a checkable control: bubbling "input"
// then "change" (change is dispatched through the cancelable helper, a
// documented approximation of its non-cancelable spec behaviour).
void DispatchToggleEvents(javascript::DomBinder* runtime, dom::Element& element)
{
  if (runtime != nullptr) {
    runtime->DispatchInputEvent(element);
    (void)runtime->DispatchCancelableEvent(element, "change");
  }
}

// ---- <details>/<summary> disclosure widget (WHATWG HTML §4.11.1) ---------

// The nearest <summary> ancestor-or-self of |element|, or nullptr.
dom::Element* NearestSummary(dom::Element& element)
{
  for (dom::Node* node = &element; node != nullptr; node = node->parent()) {
    if (node->node_type() != dom::NodeType::kElement) {
      continue;
    }
    auto& candidate = static_cast<dom::Element&>(*node);
    if (candidate.tag_name() == "summary") {
      return &candidate;
    }
  }
  return nullptr;
}

// The <details> whose disclosure control is |summary|, or nullptr.  The
// disclosure control is the *first* <summary> element child (§4.11.1); a
// second summary is not one.
dom::Element* DetailsOfSummary(const dom::Element& summary)
{
  if (summary.tag_name() != "summary") {
    return nullptr;
  }
  dom::Node* parent = summary.parent();
  if (parent == nullptr || parent->node_type() != dom::NodeType::kElement) {
    return nullptr;
  }
  auto& details = static_cast<dom::Element&>(*parent);
  if (details.tag_name() != "details") {
    return nullptr;
  }
  for (dom::Node* child : details.ChildNodes()) {
    if (child->node_type() != dom::NodeType::kElement) {
      continue;
    }
    auto& element = static_cast<dom::Element&>(*child);
    if (element.tag_name() != "summary") {
      continue;
    }
    return &element == &summary ? &details : nullptr;
  }
  return nullptr;
}

// True when |element| is interactive content with its own activation
// behaviour (HTML §3.2.5.2.7, the kinds this engine implements).  A click on
// such an element inside a <summary> must not also toggle the details.
bool IsInteractiveContent(const dom::Element& element)
{
  const std::string_view tag = element.tag_name();
  if (tag == "a" || tag == "area") {
    return element.HasAttribute("href");
  }
  return tag == "button" || tag == "input" || tag == "select" || tag == "textarea" ||
         tag == "iframe" || tag == "embed" || tag == "object" || tag == "label" || tag == "audio" ||
         tag == "video";
}

// The <details> that a click on |target| activates, or nullptr.  A click on
// the summary or a non-interactive descendant toggles; inside an interactive
// descendant the descendant's own default action wins (HTML §4.11.1: "if the
// target is interactive content, do nothing").
dom::Element* DetailsForClick(const dom::Element& target)
{
  dom::Element* summary = NearestSummary(const_cast<dom::Element&>(target));
  if (summary == nullptr) {
    return nullptr;
  }
  for (const dom::Node* node = &target; node != nullptr && node != summary; node = node->parent()) {
    if (node->node_type() == dom::NodeType::kElement &&
        IsInteractiveContent(static_cast<const dom::Element&>(*node))) {
      return nullptr;
    }
  }
  return DetailsOfSummary(*summary);
}

// Toggles a <details> open state.  The caller re-lays out and then fires the
// plain (non-bubbling) "toggle" event; the spec queues that event as a task, so
// dispatching it synchronously after the relayout is a documented
// approximation (handlers observe the new state, though not a later frame).
bool ToggleDetails(dom::Element& details)
{
  if (details.tag_name() != "details") {
    return false;
  }
  if (details.HasAttribute("open")) {
    details.RemoveAttribute("open");
  } else {
    details.SetAttribute("open", "");
  }
  return true;
}

// ---- Focus (WHATWG HTML §6.6) ---------------------------------------------

bool IsAsciiSpace(char c)
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

// Parses the tabindex content attribute with HTML's "rules for parsing
// integers": leading ASCII whitespace, an optional sign, then digits; trailing
// characters are ignored and a value without digits is invalid.  The parsed
// magnitude is clamped to 1e6 (far beyond any real tab order) so a huge literal
// cannot overflow; ordering of realistic values is preserved.
std::optional<int> TabIndexAttribute(const dom::Element& element)
{
  const std::optional<std::string_view> value = element.GetAttribute("tabindex");
  if (!value.has_value()) {
    return std::nullopt;
  }
  const std::string_view text = value.value();
  std::size_t i = 0;
  while (i < text.size() && IsAsciiSpace(text[i])) {
    ++i;
  }
  bool negative = false;
  if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
    negative = text[i] == '-';
    ++i;
  }
  if (i >= text.size() || text[i] < '0' || text[i] > '9') {
    return std::nullopt;
  }
  int parsed = 0;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
    if (parsed < 1000000) {
      parsed = (parsed * 10) + (text[i] - '0');
    }
    ++i;
  }
  return negative ? -parsed : parsed;
}

// True when |element| or one of its ancestors carries the inert attribute: an
// element in an inert subtree is neither focusable nor clickable (HTML
// §6.6.1).  Only the attribute is consulted; the engine has no CSS `inert`
// property, and a modal <dialog>'s implicit inertness is not modelled.
bool InInertSubtree(const dom::Element& element)
{
  for (const dom::Node* node = &element; node != nullptr; node = node->parent()) {
    if (node->node_type() == dom::NodeType::kElement &&
        static_cast<const dom::Element&>(*node).HasAttribute("inert")) {
      return true;
    }
  }
  return false;
}

// True when |node| is connected to |document| (walking parent() to the root).
bool IsInDocument(const dom::Node& node, const dom::Document& document)
{
  const dom::Node* root = &node;
  while (root->parent() != nullptr) {
    root = root->parent();
  }
  return root == &document;
}

// True when |element| is a focusable area (HTML §6.6.1): the naturally
// focusable controls and links this engine already supported, or any element
// with a valid tabindex attribute (which also makes a plain <div> focusable by
// script; a negative value only removes it from Tab navigation).  Excluded:
// disabled controls, input type=hidden, href-less anchors and inert subtrees.
//
// Approximation: layout is not consulted, so an element CSS hides
// (display:none) but that carries a tabindex stays focusable.  The browser
// layer does not own the layout tree, and the focus model here is DOM-based.
bool IsFocusable(const dom::Element& element)
{
  if (InInertSubtree(element) || element.HasAttribute("disabled")) {
    return false;
  }
  const std::string_view tag = element.tag_name();
  if (tag == "input") {
    // A hidden input never takes focus, even with a tabindex attribute.
    return layout::ClassifyFormControl(element) != layout::FormControlKind::kHidden;
  }
  if (tag == "textarea" || tag == "select" || tag == "button") {
    return true;
  }
  // A summary is focusable only when it is the disclosure control of a
  // <details> (HTML §4.11.1); a second summary is not.
  if (tag == "summary") {
    return DetailsOfSummary(element) != nullptr;
  }
  if ((tag == "a" || tag == "area") && element.HasAttribute("href")) {
    return true;
  }
  return TabIndexAttribute(element).has_value();
}

// True when |element| is a sequential focus navigation candidate (HTML
// §6.6.4): focusable, and its tabindex (if any) is not negative.
bool IsTabbable(const dom::Element& element)
{
  if (!IsFocusable(element)) {
    return false;
  }
  const std::optional<int> index = TabIndexAttribute(element);
  return !index.has_value() || index.value() >= 0;
}

// True when |element| can become the focused element of |page|'s document.
bool CanFocus(const dom::Element& element, renderer::Page* page)
{
  if (page == nullptr) {
    return false;
  }
  const dom::Document* document = page->document();
  return document != nullptr && IsFocusable(element) && IsInDocument(element, *document);
}

// Moves the tab's focus state to |element| (null clears it) without firing
// events: the caller runs the UI Events sequence through its binder (see
// javascript::DomBinder::PerformFocusUpdate).  Keeps the renderer's focused
// element in sync for caret painting and marks the frame dirty.
void SetFocusState(Tab* tab, dom::Element* element, renderer::Page* page)
{
  tab->focused_element = element;
  tab->frame_dirty = true;
  if (page != nullptr) {
    page->SetFocusedElement(element);
  }
}

// True when the keyboard edits the focused control's value.  The text-entry
// states support the full selection API; a number control accepts typed digits
// too (each intermediate value is a valid number, and the sanitizer rejects the
// rest).  The domain-specific date/time/colour widgets are segment-based in
// browsers and are not keyboard-editable in this engine yet.
bool IsKeyboardEditableControl(const dom::Element& element)
{
  if (dom::SupportsTextControlSelection(element)) {
    return true;
  }
  return element.tag_name() == "input" && html::InputTypeKeyword(element) == "number";
}

// Initializes a newly focused text control's selection: keyboard/script focus
// selects the whole value (browsers do this when tabbing into a field), while
// a pointer click leaves a collapsed caret at the end.  Click-position caret
// placement needs text hit-testing the browser layer does not have; the
// end-of-value caret preserves the engine's long-standing typing behavior.
void FocusTextControlSelection(dom::Element& element, bool select_contents)
{
  if (!IsKeyboardEditableControl(element)) {
    return;
  }
  const std::size_t length = dom::Utf16Length(html::InputValue(element));
  dom::TextControlState& state = element.EnsureTextControlState();
  state.selection_start = select_contents ? 0 : length;
  state.selection_end = length;
  state.selection_direction = dom::SelectionDirection::kNone;
}

// Moves keyboard focus to |element| (or clears it when null) and fires the
// focus event sequence -- blur, focusout on the old target; focus, focusin on
// the new one -- through the tab's script runtime, or through |runtime| when
// the caller is the page script that invoked focus() (the events must fire in
// the realm that called it).  Focusing a non-focusable or detached element is
// a no-op.  |page| overrides tab->page: a script can focus while a document is
// still loading and has not been published on the tab yet.
void FocusControl(Tab* tab,
                  dom::Element* element,
                  renderer::Page* page = nullptr,
                  javascript::DomBinder* runtime = nullptr,
                  bool select_contents = true)
{
  dom::Element* previous = tab->focused_element;
  if (previous == element) {
    return;
  }
  renderer::Page* target_page = page != nullptr ? page : tab->page.get();
  if (element != nullptr && !CanFocus(*element, target_page)) {
    return;
  }
  SetFocusState(tab, element, target_page);
  if (element != nullptr) {
    FocusTextControlSelection(*element, select_contents);
  }
  javascript::DomBinder* binder = runtime != nullptr ? runtime : tab->script_runtime.get();
  if (binder != nullptr) {
    binder->PerformFocusUpdate(previous, element);
  }
}

// The document's tabbable elements in sequential focus navigation order
// (HTML §6.6.4): positive tabindex values first, ascending by tabindex (ties
// keep tree order), then tabindex=0 / naturally focusable elements in tree
// order.  tabindex=-1 elements are excluded: script-focusable, not
// Tab-reachable.
std::vector<dom::Element*> SequentialFocusOrder(dom::Element& from)
{
  std::vector<dom::Element*> positive;
  std::vector<dom::Element*> plain;
  const dom::Node* root = &from;
  while (root->parent() != nullptr) {
    root = root->parent();
  }
  std::vector<dom::Node*> stack{const_cast<dom::Node*>(root)};
  while (!stack.empty()) {
    dom::Node* node = stack.back();
    stack.pop_back();
    if (node->node_type() == dom::NodeType::kElement) {
      auto& element = static_cast<dom::Element&>(*node);
      if (IsTabbable(element)) {
        const std::optional<int> index = TabIndexAttribute(element);
        if (index.has_value() && index.value() > 0) {
          positive.push_back(&element);
        } else {
          plain.push_back(&element);
        }
      }
    }
    // Reverse-push so children pop in document order (tree-order ties).
    const std::size_t count = node->child_count();
    for (std::size_t i = count; i > 0; --i) {
      stack.push_back(node->child_at(i - 1));
    }
  }
  // Stable: equal tabindex values keep the tree order they were collected in.
  std::stable_sort(
      positive.begin(), positive.end(), [](const dom::Element* a, const dom::Element* b) {
        return TabIndexAttribute(*a).value_or(0) < TabIndexAttribute(*b).value_or(0);
      });
  positive.insert(positive.end(), plain.begin(), plain.end());
  return positive;
}

// The next tabbable control after |tab|'s (walking |backwards| wraps at the
// ends), or nullptr when the document has none.
dom::Element* NextFocusable(Tab* tab, bool backwards)
{
  if (tab->page == nullptr) {
    return nullptr;
  }
  dom::Document* doc = tab->page->document();
  if (doc == nullptr || doc->document_element() == nullptr) {
    return nullptr;
  }
  const std::vector<dom::Element*> focusables = SequentialFocusOrder(*doc->document_element());
  if (focusables.empty()) {
    return nullptr;
  }
  const auto it = std::find(focusables.begin(), focusables.end(), tab->focused_element);
  const int count = static_cast<int>(focusables.size());
  int index = it == focusables.end() ? -1 : static_cast<int>(it - focusables.begin());
  if (backwards) {
    index = index <= 0 ? count - 1 : index - 1;
  } else {
    index = (index + 1) % count;
  }
  return focusables[static_cast<std::size_t>(index)];
}

// Autofocus processing for a freshly loaded document (HTML §6.6.1): focus the
// first element in tree order with an unprocessed autofocus attribute, if it
// is focusable.  A script that already moved focus during the load wins --
// autofocus never steals it.  Candidates are remembered on the tab, so one is
// applied at most once; adding the attribute after the load does not move
// focus (the spec only processes candidates when the document is loaded).
void ApplyAutofocus(Tab& tab, renderer::Page& page)
{
  if (tab.focused_element != nullptr) {
    return;
  }
  dom::Document* document = page.document();
  if (document == nullptr || document->document_element() == nullptr) {
    return;
  }
  std::vector<dom::Node*> stack{document->document_element()};
  while (!stack.empty()) {
    dom::Node* node = stack.back();
    stack.pop_back();
    if (node->node_type() == dom::NodeType::kElement) {
      auto& element = static_cast<dom::Element&>(*node);
      const auto processed =
          std::find(tab.autofocus_processed.begin(), tab.autofocus_processed.end(), &element);
      if (element.HasAttribute("autofocus") && processed == tab.autofocus_processed.end()) {
        tab.autofocus_processed.push_back(&element);
        if (IsFocusable(element)) {
          FocusControl(&tab, &element, &page);
          return;
        }
      }
    }
    const std::size_t count = node->child_count();
    for (std::size_t i = count; i > 0; --i) {
      stack.push_back(node->child_at(i - 1));
    }
  }
}

// Walks from |node| up to find a submit button (<button>, <input type=submit>
// or <input type=image>) and returns it, or nullptr.
dom::Element* FindSubmitButton(dom::Node* node)
{
  for (dom::Node* n = node; n != nullptr; n = n->parent()) {
    if (n->node_type() != dom::NodeType::kElement) {
      continue;
    }
    dom::Element& e = static_cast<dom::Element&>(*n);
    if (html::IsSubmitButton(e)) {
      return &e;
    }
  }
  return nullptr;
}

int64_t NowUnix()
{
  return static_cast<int64_t>(std::time(nullptr));
}

// Monotonic milliseconds (frame-pull throttling).
int64_t NowMillis()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Viewport used for a renderer session until the GUI reports its own.
inline constexpr int kDefaultRemoteViewportWidth = 800;
inline constexpr int kDefaultRemoteViewportHeight = 600;
// Frames are pulled at most this often for scroll-driven repaints; anything
// faster is coalesced into the next pump.
inline constexpr int64_t kRemoteFrameMinIntervalMs = 40;

// True when |tab| renders its document in a renderer child (renderer-process
// mode showing HTML).  After navigating to non-HTML content or after a
// session failure the tab falls back to the in-process paths.
bool IsRemoteTab(const Tab& tab)
{
  return tab.session != nullptr && tab.page == nullptr && tab.content_type == ContentType::kHtml;
}

// The CLI binary serves --renderer-session and ships next to the GUI
// executable in the build/install layout.
std::string DefaultRendererExecutable()
{
  const std::string self = platform::SelfExecutablePath();
  const std::size_t slash = self.find_last_of("/\\");
  if (self.empty() || slash == std::string::npos) {
    return {};
  }
#ifdef _WIN32
  const std::string candidate = self.substr(0, slash + 1) + "neko_browser.exe";
#else
  const std::string candidate = self.substr(0, slash + 1) + "neko_browser";
#endif
  std::error_code ec;
  if (std::filesystem::exists(candidate, ec) && !ec) {
    return candidate;
  }
  return {};
}

// ---------------------------------------------------------------------------
// Password-manager form inspection helpers.  The caller holds the page's DOM
// lock (or owns the document outright, as in LoadBytes before publication).
// ---------------------------------------------------------------------------

// The <input> elements under |root| in document order (pre-order walk).
std::vector<dom::Element*> CollectInputs(dom::Node& root)
{
  std::vector<dom::Element*> inputs;
  std::vector<dom::Node*> stack;
  auto push_children = [&stack](dom::Node& n) {
    std::vector<dom::Node*> children;
    for (dom::Node* c : n.ChildNodes()) {
      children.push_back(c);
    }
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
      stack.push_back(*it);
    }
  };
  push_children(root);
  while (!stack.empty()) {
    dom::Node* n = stack.back();
    stack.pop_back();
    if (n->node_type() != dom::NodeType::kElement) {
      continue;
    }
    dom::Element& e = static_cast<dom::Element&>(*n);
    push_children(e);
    if (e.tag_name() == "input") {
      inputs.push_back(&e);
    }
  }
  return inputs;
}

// Lowercased input type; a missing attribute means "text".
std::string InputType(const dom::Element& input)
{
  return base::ToLower(input.GetAttribute("type").value_or("text"));
}

// Text-entry controls that can hold a username.
bool IsUsernameInputType(std::string_view type)
{
  return type == "text" || type == "search" || type == "email" || type == "url" || type == "tel";
}

// The nearest preceding text-ish input of |password| within the same form
// (page-level controls count as one anonymous form).  |inputs| is the
// document-order input list |password| belongs to.
dom::Element* FindUsernameInput(const std::vector<dom::Element*>& inputs, dom::Element* password)
{
  const auto form_of = [](dom::Element* element) -> dom::Element* {
    for (dom::Node* n = element->parent(); n != nullptr; n = n->parent()) {
      if (n->node_type() != dom::NodeType::kElement) {
        continue;
      }
      dom::Element& e = static_cast<dom::Element&>(*n);
      if (e.tag_name() == "form") {
        return &e;
      }
    }
    return nullptr;
  };
  dom::Element* password_form = form_of(password);
  dom::Element* best = nullptr;
  for (dom::Element* input : inputs) {
    if (input == password) {
      break;
    }
    if (form_of(input) != password_form) {
      continue;
    }
    if (IsUsernameInputType(InputType(*input))) {
      best = input;
    }
  }
  return best;
}

// Guards the synchronous navigation chain against runaway JS-driven loops (a
// page that keeps assigning window.location, or two pages that redirect to
// each other).  Each hop is a real network fetch; the cap mirrors the HTTP
// redirect limit.  Thread-local because each worker uses its own chain.
class NavigationDepthGuard
{
public:
  explicit NavigationDepthGuard(int limit) : limit_(limit)
  {
    ++depth_;
  }
  ~NavigationDepthGuard()
  {
    --depth_;
  }
  bool Exceeded() const
  {
    return depth_ > limit_;
  }
  NavigationDepthGuard(const NavigationDepthGuard&) = delete;
  NavigationDepthGuard& operator=(const NavigationDepthGuard&) = delete;

private:
  int limit_;
  static thread_local int depth_;
};

thread_local int NavigationDepthGuard::depth_ = 0;

// True when |bytes| begin with '<' (after optional whitespace/BOM), which we
// take as a hint for HTML content when no Content-Type was provided.
bool LooksLikeHtml(std::string_view bytes)
{
  size_t i = 0;
  if (bytes.size() >= 3 && static_cast<uint8_t>(bytes[0]) == 0xEF &&
      static_cast<uint8_t>(bytes[1]) == 0xBB && static_cast<uint8_t>(bytes[2]) == 0xBF) {
    i = 3;
  }
  while (i < bytes.size() &&
         (bytes[i] == ' ' || bytes[i] == '\t' || bytes[i] == '\n' || bytes[i] == '\r')) {
    ++i;
  }
  return i < bytes.size() && bytes[i] == '<';
}

// Copies the GUI-visible state of |tab|.  Called with the controller mutex
// held; the shared payload handles keep the data alive for the caller.
TabSnapshot ToSnapshot(const Tab& tab)
{
  TabSnapshot s;
  s.id = tab.id;
  s.url = tab.url;
  s.title = tab.title;
  s.loading = tab.loading;
  s.content_type = tab.content_type;
  s.origin = tab.origin;
  s.page = tab.page;
  s.image = tab.image;
  s.image_frame = tab.image_frame;
  s.pdf = tab.pdf;
  s.audio = tab.audio;
  s.raw_text = tab.raw_text;
  s.error = tab.error;
  s.scroll_request_id = tab.scroll_request_id;
  s.pending_scroll_y = tab.pending_scroll_y;
  // "Remote" describes the CURRENT document: a tab whose site session is alive
  // but that currently shows an image / text / error page renders locally.
  s.remote = IsRemoteTab(tab);
  s.remote_frame = tab.remote_frame;
  s.remote_content_height = tab.remote_content_height;
  s.remote_hover_link = tab.remote_hover_link;
  // In-process frames (ADR 0020) and child-process frames share one snapshot
  // shape: the GUI draws |frame| and never touches the live page.
  s.frame = s.remote ? tab.remote_frame : tab.frame;
  s.frame_content_height = s.remote ? tab.remote_content_height : tab.frame_content_height;
  s.frame_hover_link = s.remote ? tab.remote_hover_link : tab.frame_hover_link;
  if (!s.remote) {
    s.has_caret = tab.has_caret;
    s.caret_x = tab.caret_x;
    s.caret_y = tab.caret_y;
    s.caret_height = tab.caret_height;
  }
  s.zoom = tab.zoom;
  s.find_query = tab.find_query;
  s.find_match_count = tab.find_match_count;
  s.find_current_index = tab.find_index;
  s.find_current_match = tab.find_match;
  if (tab.pending_credential.has_value()) {
    s.has_pending_credential = true;
    s.pending_credential_origin = tab.pending_credential->origin;
    s.pending_credential_username = tab.pending_credential->username;
  }
  return s;
}

// Returns the allocator's free pages to the OS when the enclosing operation
// ends (see base::ReleaseFreeMemory).  Wrapped around operations that replace
// or drop a tab's document: without it the previous DOM, style store and
// images stay resident in the glibc arenas even though they were already
// freed (measured in the GUI: a closed tab released ~1 MiB of its ~126 MiB).
class HeapTrimOnExit
{
public:
  HeapTrimOnExit() = default;
  HeapTrimOnExit(const HeapTrimOnExit&) = delete;
  HeapTrimOnExit& operator=(const HeapTrimOnExit&) = delete;
  ~HeapTrimOnExit()
  {
    base::ReleaseFreeMemory();
  }
};

} // namespace

std::string_view ToString(ContentType type)
{
  switch (type) {
  case ContentType::kHtml:
    return "html";
  case ContentType::kImage:
    return "image";
  case ContentType::kPdf:
    return "pdf";
  case ContentType::kAudio:
    return "audio";
  case ContentType::kText:
    return "text";
  case ContentType::kOther:
    return "other";
  case ContentType::kError:
    return "error";
  }
  return "unknown";
}

BrowserController::BrowserController(std::string profile_dir,
                                     FetchFn fetch,
                                     RendererOptions renderer,
                                     NetworkOptions network)
    : profile_dir_(std::move(profile_dir)), fetch_(std::move(fetch)),
      renderer_(std::move(renderer)), network_(std::move(network)), cookies_(profile_dir_),
      history_(profile_dir_), bookmarks_(profile_dir_), local_storage_(profile_dir_),
      indexed_db_(profile_dir_), preferences_(profile_dir_), passwords_(profile_dir_),
      downloads_(profile_dir_ + "/downloads")
{
  if (renderer_.enabled) {
    renderer_executable_ =
        renderer_.executable.empty() ? DefaultRendererExecutable() : renderer_.executable;
    if (renderer_executable_.empty()) {
      NEKO_LOG_WARNING("renderer process mode requested but no renderer executable was found; "
                       "falling back to in-process rendering");
      renderer_.enabled = false;
    }
  }
  pool_ = std::make_unique<base::ThreadPool>();
  // The raster pool: up to four workers (HardwareConcurrency() - 1), and one
  // worker means RasterizeParallel falls back to the serial path -- which is
  // exactly what a two-core machine wants, and never waits on the shared
  // pool's capacity.
  {
    const std::size_t hardware = base::ThreadPool::HardwareConcurrency();
    const std::size_t raster_threads = hardware <= 2 ? 1 : std::min<std::size_t>(4, hardware - 1);
    raster_pool_ = std::make_unique<base::ThreadPool>(raster_threads);
  }
  // Default fetch: compute cookies for each redirect hop from the controller's
  // cookie jar.  HttpGet invokes HeaderProvider with the current hop URL.
  // With network-process mode the whole fetch runs in a child; the child asks
  // back for each hop's cookies, so the jar (and HttpOnly values) stay here.
  if (!fetch_) {
    fetch_ = [this](const url::Url& u, std::string_view, const RequestContext& context) {
      // SameSite is decided per hop against the initiator's site (a redirect
      // target may differ from the original target).
      const auto same_site_for = [&context](const url::Url& target) {
        storage::CookieSameSiteContext same_site;
        same_site.top_level_navigation = context.top_level_navigation;
        same_site.cross_site = !context.initiator_site.empty() &&
                               security::SiteForCookies(target) != context.initiator_site;
        return same_site;
      };
      if (network_.enabled) {
        NetworkSession* session = EnsureNetworkSession();
        if (session != nullptr) {
          NetworkSession::CookieLookupFn lookup = [this, &same_site_for](const url::Url& target) {
            return CookieHeader(target, NowUnix(), same_site_for(target));
          };
          // NOTE: extra_headers do not cross the network-process protocol yet
          // (the request serializer carries url, cookie, method and body); the
          // CORS Origin header is therefore absent there.  That fails CLOSED
          // (an Origin-varying server grants no ACAO -> the response is
          // refused), so it is a compatibility gap, not a security hole.
          auto fetched = session->Fetch(
              u, /*cookie_header=*/{}, lookup, context.method, context.body, context.content_type);
          if (fetched.has_value()) {
            return fetched;
          }
          NEKO_LOG_WARNING("network process fetch failed; falling back in-process: " +
                           fetched.error().message());
        }
      }
      network::HeaderProvider provider;
      provider = [this, &context, &same_site_for](const url::Url& target) {
        std::vector<network::HttpHeader> headers;
        headers.reserve(context.extra_headers.size() + 1);
        for (const auto& [name, value] : context.extra_headers) {
          headers.push_back({name, value});
        }
        const std::string cookie = CookieHeader(target, NowUnix(), same_site_for(target));
        if (!cookie.empty()) {
          headers.push_back({"cookie", cookie});
        }
        return headers;
      };
      // Form submission posts an application/x-www-form-urlencoded body; every
      // other fetch in the engine is a GET (the default context method).
      if (context.method == "POST") {
        return network::HttpPost(u, context.body, context.content_type, 5, provider);
      }
      return network::HttpGet(u, 5, provider);
    };
  }
}

NetworkSession* BrowserController::EnsureNetworkSession()
{
  if (!network_.enabled) {
    return nullptr;
  }
  if (network_session_ && network_session_->alive()) {
    return network_session_.get();
  }
  const std::string executable =
      network_.executable.empty() ? DefaultRendererExecutable() : network_.executable;
  if (executable.empty()) {
    NEKO_LOG_WARNING("network process mode requested but no executable was found");
    return nullptr;
  }
  auto spawned = NetworkSession::Spawn(executable);
  if (!spawned.has_value()) {
    NEKO_LOG_WARNING("network process failed to start: " + spawned.error().message());
    network_session_.reset();
    return nullptr;
  }
  network_session_ = spawned.value();
  return network_session_.get();
}

BrowserController::~BrowserController()
{
  (void)Save();
}

#include "browser_controller_fetch.inc"
#include "browser_controller_navigation.inc"
#include "browser_controller_storage.inc"
#include "browser_controller_tabs.inc"
} // namespace neko::browser
