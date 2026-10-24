// BrowserController implementation.
//
// The controller is one class but several responsibilities; the method bodies
// live in .inc fragments included below so each subsystem is reviewable on its
// own: tabs, navigation/input, form submission, frame/timer production, view
// state (scroll/zoom/find), fetch/content routing, and persistence/DevTools.  They stay in this
// translation unit because member bodies call the file-local helpers in the anonymous namespace
// above; promoting them to separate TUs requires externalising those helpers first (same follow-up
// as layout.cpp).
#include "neko/base/logging.h"
#include "neko/base/memory.h"
#include "neko/base/string_util.h"
#include "neko/base/thread_pool.h"
#include "neko/browser/browser_controller.h"
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

// Shared declarations for the BrowserController implementation files.
// The helpers below used to be file-local in browser_controller.cpp; they
// are module-internal (no public header installs them).

namespace neko::browser {

// A submission request being executed (direct user activation or a scripted
// form.submit()/requestSubmit()).  |scripted| skips interactive validation and
// the submit event (HTMLFormElement.submit()).
struct FormSubmissionRequest
{
  dom::Element* form = nullptr;
  dom::Element* submitter = nullptr;
  bool scripted = false;
};

// The three form submission encodings (HTML §4.10.19.6 "enctype").
enum class FormEnctype
{
  kUrlEncoded,
  kMultipart,
  kTextPlain,
};

// Viewport used for a renderer session until the GUI reports its own.
inline constexpr int kDefaultRemoteViewportWidth = 800;

inline constexpr int kDefaultRemoteViewportHeight = 600;

// Frames are pulled at most this often for scroll-driven repaints; anything
// faster is coalesced into the next pump.
inline constexpr int64_t kRemoteFrameMinIntervalMs = 40;

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

std::string_view Trim(std::string_view s);
bool IsPrintableKey(std::string_view key);
std::string ResolveUrlAgainstBase(std::string_view ref, std::string_view base_url);
std::string NormalizeNewlines(std::string_view value);
FormEnctype ParseEnctype(std::string_view raw);
std::string AppendQuery(std::string url, std::string_view query);
std::string GenerateMultipartBoundary();
std::string EscapeMultipartName(std::string_view name);
std::string EncodeUrlEncoded(const std::vector<html::FormEntry>& entries);
std::string EncodePlainText(const std::vector<html::FormEntry>& entries);
std::string EncodeMultipart(const std::vector<html::FormEntry>& entries, std::string_view boundary);
void UncheckRadioGroup(dom::Element& element);
bool ToggleCheckableInput(dom::Element& element);
void DispatchToggleEvents(javascript::DomBinder* runtime, dom::Element& element);
dom::Element* NearestSummary(dom::Element& element);
dom::Element* DetailsOfSummary(const dom::Element& summary);
bool IsInteractiveContent(const dom::Element& element);
dom::Element* DetailsForClick(const dom::Element& target);
bool ToggleDetails(dom::Element& details);
bool IsAsciiSpace(char c);
std::optional<int> TabIndexAttribute(const dom::Element& element);
bool InInertSubtree(const dom::Element& element);
bool IsInDocument(const dom::Node& node, const dom::Document& document);
bool IsFocusable(const dom::Element& element);
bool IsTabbable(const dom::Element& element);
bool CanFocus(const dom::Element& element, renderer::Page* page);
void SetFocusState(Tab* tab, dom::Element* element, renderer::Page* page);
bool IsKeyboardEditableControl(const dom::Element& element);
void FocusTextControlSelection(dom::Element& element, bool select_contents);
void FocusControl(Tab* tab,
                  dom::Element* element,
                  renderer::Page* page = nullptr,
                  javascript::DomBinder* runtime = nullptr,
                  bool select_contents = true);
std::vector<dom::Element*> SequentialFocusOrder(dom::Element& from);
dom::Element* NextFocusable(Tab* tab, bool backwards);
void ApplyAutofocus(Tab& tab, renderer::Page& page);
dom::Element* FindSubmitButton(dom::Node* node);
int64_t NowUnix();
int64_t NowMillis();
bool IsRemoteTab(const Tab& tab);
std::string DefaultRendererExecutable();
std::vector<dom::Element*> CollectInputs(dom::Node& root);
std::string InputType(const dom::Element& input);
bool IsUsernameInputType(std::string_view type);
dom::Element* FindUsernameInput(const std::vector<dom::Element*>& inputs, dom::Element* password);

// True when |bytes| begin with '<' (after optional whitespace/BOM), which we
// take as a hint for HTML content when no Content-Type was provided.
bool LooksLikeHtml(std::string_view bytes);
TabSnapshot ToSnapshot(const Tab& tab);
bool ContainsNode(const dom::Node& root, const dom::Node* target);

} // namespace neko::browser
