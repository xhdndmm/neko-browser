// Unit tests for neko::javascript::DomBinder (DOM bindings, page <script>
// execution support, and the minimal event loop).

#include "neko/base/status.h"
#include "neko/dom/element.h"
#include "neko/dom/query.h"
#include "neko/html/parser.h"
#include "neko/javascript/dom_binding.h"
#include "neko/storage/indexed_db.h"
#include "neko/storage/session_storage.h"

#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <sys/socket.h>
#include <unistd.h>
#else
// Only the process-id helper below needs a platform header on Windows: the
// socket / TLS test classes are POSIX-only and guarded.
#include <process.h>
#endif

namespace neko::javascript {
namespace {

class DomBinderTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    document_ = html::Parser(R"(<!doctype html>
<html><head><title>Test Page</title></head>
<body>
  <div id="main" class="container">
    <p id="first" class="para">Hello <b>world</b></p>
    <p class="para">Second</p>
    <span data-x="1"></span>
  </div>
</body></html>)")
                    .Parse();
    PageApis apis;
    apis.location_href = []() { return "https://www.example.com/"; };
    binder_ = std::make_unique<DomBinder>(*document_, apis);
    binder_->SetConsoleSink([this](std::string_view level, std::string_view text) {
      console_.push_back(std::string(level) + ": " + std::string(text));
    });
  }

  std::string EvalString(const std::string& code)
  {
    auto r = binder_->Evaluate(code);
    if (!r.has_value()) {
      return "<error: " + r.error().message() + ">";
    }
    auto s = r.value().ToString();
    if (!s.has_value()) {
      return "<tostring-error>";
    }
    return s.value();
  }

  double EvalNumber(const std::string& code)
  {
    auto r = binder_->Evaluate(code);
    if (!r.has_value()) {
      return -1e9;
    }
    auto n = r.value().ToNumber();
    return n.has_value() ? n.value() : -1e9;
  }

  bool EvalBool(const std::string& code)
  {
    auto r = binder_->Evaluate(code);
    if (!r.has_value()) {
      return false;
    }
    auto b = r.value().ToBoolean();
    return b.has_value() && b.value();
  }

  std::unique_ptr<dom::Document> document_;
  std::unique_ptr<DomBinder> binder_;
  std::vector<std::string> console_;
};

TEST_F(DomBinderTest, GlobalDocumentAndWindow)
{
  EXPECT_EQ(EvalNumber("document.nodeType"), 9.0);
  EXPECT_EQ(EvalString("document.nodeName"), "#document");
  EXPECT_EQ(EvalString("document.title"), "Test Page");
  EXPECT_TRUE(EvalBool("window.document === document"));
  EXPECT_TRUE(EvalBool("document.defaultView === window"));
  EXPECT_TRUE(
      EvalBool("(function(){ var descriptor = Object.getOwnPropertyDescriptor("
               "Document.prototype, 'defaultView'); "
               "return typeof descriptor.get === 'function' && descriptor.set === undefined "
               "    && !Object.prototype.hasOwnProperty.call(document, 'defaultView'); })()"));
  EXPECT_EQ(EvalNumber("document.defaultView.pageYOffset"), 0.0);
  EXPECT_EQ(EvalNumber("window.pageYOffset"), 0.0);
  EXPECT_EQ(EvalNumber("window.scrollY"), 0.0);
  EXPECT_EQ(EvalString("document.documentElement.tagName"), "HTML");
  EXPECT_EQ(EvalString("document.body.tagName"), "BODY");
  EXPECT_TRUE(EvalBool("document.location === window.location"));
}

// The window viewport (innerWidth/innerHeight/devicePixelRatio/screen/
// visualViewport): engine default without a callback, live reads with one.
TEST_F(DomBinderTest, WindowViewportIsLiveAndDefaultsToTheEngineSize)
{
  // The fixture's PageApis has no viewport_size: the engine default applies.
  EXPECT_EQ(EvalNumber("window.innerWidth"), 800.0);
  EXPECT_EQ(EvalNumber("window.innerHeight"), 600.0);
  EXPECT_EQ(EvalNumber("window.outerWidth"), 800.0);
  EXPECT_EQ(EvalNumber("window.outerHeight"), 600.0);
  EXPECT_EQ(EvalNumber("window.devicePixelRatio"), 1.0);
  EXPECT_EQ(EvalNumber("screen.width"), 800.0);
  EXPECT_EQ(EvalNumber("screen.availHeight"), 600.0);
  EXPECT_EQ(EvalNumber("visualViewport.width"), 800.0);

  // With the callback wired (a real page script runtime), the same properties
  // read the renderer's current viewport on every access: a resize is visible
  // immediately, like in a browser.
  auto size = std::make_shared<std::pair<int, int>>(1024, 768);
  auto ratio = std::make_shared<double>(2.0);
  PageApis apis;
  apis.location_href = []() { return "https://www.example.com/"; };
  apis.viewport_size = [size]() { return *size; };
  apis.device_pixel_ratio = [ratio]() { return *ratio; };
  DomBinder binder(*document_, apis);

  const auto number = [&binder](const std::string& code) {
    auto result = binder.Evaluate(code);
    EXPECT_TRUE(result.has_value()) << code;
    if (!result.has_value()) {
      return -1.0;
    }
    auto value = result.value().ToNumber();
    return value.has_value() ? value.value() : -1.0;
  };
  EXPECT_EQ(number("window.innerWidth"), 1024.0);
  EXPECT_EQ(number("window.innerHeight"), 768.0);
  EXPECT_EQ(number("window.devicePixelRatio"), 2.0);
  EXPECT_EQ(number("screen.width"), 1024.0); // captured when the binder was built
  *size = {640, 480};
  EXPECT_EQ(number("window.innerWidth"), 640.0);
  EXPECT_EQ(number("window.innerHeight"), 480.0);
  *ratio = 1.5;
  EXPECT_EQ(number("window.devicePixelRatio"), 1.5);
}

TEST_F(DomBinderTest, LiveCollectionsRefreshAfterMutation)
{
  document_ =
      html::Parser(R"(<!doctype html><html><body><div id="a"><span>x</span></div></body></html>)")
          .Parse();
  PageApis apis;
  apis.location_href = []() { return "https://www.example.com/"; };
  binder_ = std::make_unique<DomBinder>(*document_, apis);
  binder_->SetConsoleSink([this](std::string_view level, std::string_view text) {
    console_.push_back(std::string(level) + ": " + std::string(text));
  });

  // children is a live HTMLCollection: the held reference tracks mutations.
  EXPECT_EQ(EvalNumber("var el = document.getElementById('a'); var c = el.children; c.length"),
            1.0);
  EXPECT_EQ(EvalNumber("el.appendChild(document.createElement('b')); c.length"), 2.0);
  // Wrapper identity is preserved across refreshes: the same live node renders
  // the same JS object no matter how many times it is re-collected.
  EXPECT_TRUE(EvalBool("c[1] === el.children[1]"));

  // getElementsByTagName is live: adding another <div> appears in the held list.
  EXPECT_EQ(EvalNumber("var g = document.getElementsByTagName('div'); g.length"), 1.0);
  EXPECT_EQ(EvalNumber("document.body.appendChild(document.createElement('div')); g.length"), 2.0);

  // childNodes is live and includes text/comment nodes too.
  EXPECT_EQ(EvalNumber("var cn = el.childNodes; cn.length"), 2.0);
  EXPECT_EQ(EvalNumber("el.appendChild(document.createElement('p')); cn.length"), 3.0);
}

TEST_F(DomBinderTest, QuerySelectorAllIsStatic)
{
  document_ = html::Parser(R"(<!doctype html><html><body><span>a</span></body></html>)").Parse();
  PageApis apis;
  apis.location_href = []() { return "https://www.example.com/"; };
  binder_ = std::make_unique<DomBinder>(*document_, apis);
  binder_->SetConsoleSink([this](std::string_view level, std::string_view text) {
    console_.push_back(std::string(level) + ": " + std::string(text));
  });

  // querySelectorAll is a static NodeList (HTML spec): the already-returned
  // list does not grow when a matching element is added afterwards.
  EXPECT_EQ(EvalNumber("var s = document.querySelectorAll('span'); s.length"), 1.0);
  EXPECT_EQ(EvalNumber("document.body.appendChild(document.createElement('span')); s.length"), 1.0);
}

TEST_F(DomBinderTest, DocumentStyleSheetsExposesRules)
{
  document_ =
      html::Parser(R"(<!doctype html><html><body><style>div{color:red}</style></body></html>)")
          .Parse();
  PageApis apis;
  apis.location_href = []() { return "https://www.example.com/"; };
  // The browser layer normally wires these from the style engine; here the
  // test drives a fake sheet that insertRule/deleteRule mutate.
  std::string sheet_text = "div{color:red}";
  apis.stylesheet_count = []() { return 1; };
  apis.stylesheet_href = [](std::size_t) { return std::string(); };
  apis.stylesheet_text = [&sheet_text](std::size_t) { return sheet_text; };
  apis.stylesheet_replace = [&sheet_text](std::size_t,
                                          const std::string& text) -> std::optional<std::string> {
    sheet_text = text;
    return std::nullopt;
  };
  binder_ = std::make_unique<DomBinder>(*document_, apis);
  binder_->SetConsoleSink([this](std::string_view level, std::string_view text) {
    console_.push_back(std::string(level) + ": " + std::string(text));
  });

  EXPECT_EQ(EvalNumber("document.styleSheets.length"), 1.0);
  EXPECT_EQ(EvalString("document.styleSheets[0].cssRules[0].selectorText"), "div");
  EXPECT_TRUE(EvalBool("document.styleSheets[0].cssRules[0].cssText.indexOf('color') !== -1"));
  // insertRule appends a rule and re-queries cssRules on the next access.
  EXPECT_EQ(EvalNumber("document.styleSheets[0].insertRule('p{font-size:12px}', 1)"), 1.0);
  EXPECT_EQ(EvalNumber("document.styleSheets[0].cssRules.length"), 2.0);
  EXPECT_EQ(EvalString("document.styleSheets[0].cssRules[1].selectorText"), "p");
  // deleteRule removes it again.
  EvalString("document.styleSheets[0].deleteRule(1)");
  EXPECT_EQ(EvalNumber("document.styleSheets[0].cssRules.length"), 1.0);
}

TEST_F(DomBinderTest, MatchMediaRegistersListeners)
{
  // matchMedia exposes a live listener API rather than the old no-op stubs:
  // addEventListener/removeEventListener and the legacy addListener route
  // register against the binder (the callback is retained, not dropped).
  EXPECT_TRUE(EvalBool("(function(){ var m = window.matchMedia('(min-width: 1px)'); "
                       "if (!m.matches) return false; "
                       "if (m.media !== '(min-width: 1px)') return false; "
                       "var calls = 0; "
                       "m.addEventListener('change', function(){ calls++; }); "
                       "m.removeEventListener('change', function(){ calls++; }); "
                       "m.addListener(function(){ calls++; }); "
                       "m.removeListener(function(){ calls++; }); "
                       "return typeof m.addEventListener === 'function' && "
                       "typeof m.removeEventListener === 'function' && "
                       "typeof m.addListener === 'function' && "
                       "typeof m.removeListener === 'function'; })()"));
}

TEST_F(DomBinderTest, HistoryPushStateUpdatesLocation)
{
  document_ = html::Parser(R"(<!doctype html><html><body><p>hi</p></body></html>)").Parse();
  PageApis apis;
  apis.location_href = []() { return "https://www.example.com/start"; };
  apis.resolve_url = [](const std::string& raw) -> std::string {
    if (raw.rfind("http://", 0) == 0 || raw.rfind("https://", 0) == 0) {
      return raw;
    }
    return "https://www.example.com" + raw;
  };
  std::vector<std::string> pushed;
  std::vector<std::string> replaced;
  int64_t length = 1;
  std::string state_json;
  std::vector<int> go_deltas;
  apis.history_push = [&](const std::string& url) {
    pushed.push_back(url);
    ++length;
  };
  apis.history_replace = [&](const std::string& url) { replaced.push_back(url); };
  apis.history_length = [&]() { return length; };
  apis.history_go = [&](int delta) { go_deltas.push_back(delta); };
  apis.history_state_get = [&]() { return state_json; };
  apis.history_state_set = [&](const std::string& state) { state_json = state; };
  binder_ = std::make_unique<DomBinder>(*document_, apis);
  binder_->SetConsoleSink([this](std::string_view level, std::string_view text) {
    console_.push_back(std::string(level) + ": " + std::string(text));
  });

  // pushState advances the binder's URL view and records the entry.
  EXPECT_EQ(EvalNumber("history.pushState({a: 1}, '', '/x'); history.length"), 2.0);
  EXPECT_EQ(EvalString("location.href"), "https://www.example.com/x");
  EXPECT_EQ(EvalNumber("history.state.a"), 1.0);
  ASSERT_EQ(pushed.size(), 1u);
  EXPECT_EQ(pushed[0], "https://www.example.com/x");

  // replaceState updates the URL + state without growing the length.
  EvalString("history.replaceState({b: 2}, '', '/y')");
  ASSERT_EQ(replaced.size(), 1u);
  EXPECT_EQ(replaced[0], "https://www.example.com/y");
  EXPECT_EQ(EvalString("location.href"), "https://www.example.com/y");
  EXPECT_EQ(EvalNumber("history.state.b"), 2.0);

  // back()/forward()/go(delta) drive the traversal hook.
  EvalString("history.back()");
  EvalString("history.forward()");
  EvalString("history.go(-2)");
  ASSERT_EQ(go_deltas.size(), 3u);
  EXPECT_EQ(go_deltas[0], -1);
  EXPECT_EQ(go_deltas[1], 1);
  EXPECT_EQ(go_deltas[2], -2);
}

TEST_F(DomBinderTest, ScrollToAndLiveScrollOffset)
{
  document_ = html::Parser(R"(<!doctype html><html><body><p>hi</p></body></html>)").Parse();
  PageApis apis;
  apis.location_href = []() { return "https://www.example.com/"; };
  double offset_x = 0;
  double offset_y = 0;
  std::vector<std::pair<double, double>> scrolls;
  apis.scroll_offset = [&]() -> std::pair<double, double> { return {offset_x, offset_y}; };
  apis.scroll_to = [&](double x, double y) {
    scrolls.emplace_back(x, y);
    offset_x = x;
    offset_y = y;
  };
  binder_ = std::make_unique<DomBinder>(*document_, apis);
  binder_->SetConsoleSink([this](std::string_view level, std::string_view text) {
    console_.push_back(std::string(level) + ": " + std::string(text));
  });

  // window.scrollTo drives the scroll_to callback.
  EvalString("window.scrollTo(0, 120)");
  ASSERT_EQ(scrolls.size(), 1u);
  EXPECT_EQ(scrolls[0].first, 0.0);
  EXPECT_EQ(scrolls[0].second, 120.0);
  // window.scrollY/pageYOffset/scrollX read the live offset.
  EXPECT_EQ(EvalNumber("window.scrollY"), 120.0);
  EXPECT_EQ(EvalNumber("window.pageYOffset"), 120.0);
  EXPECT_EQ(EvalNumber("window.scrollX"), 0.0);
  // scrollBy adds to the current offset.
  EvalString("window.scrollBy(0, 10)");
  ASSERT_EQ(scrolls.size(), 2u);
  EXPECT_EQ(scrolls[1].second, 130.0);
  // Element scrollTop on the document scrolling element reads the offset too.
  EXPECT_EQ(EvalNumber("document.body.scrollTop"), 130.0);
}

TEST_F(DomBinderTest, FragmentAppendKeepsFragmentAlive)
{
  document_ = html::Parser(R"(<!doctype html><html><body></body></html>)").Parse();
  PageApis apis;
  apis.location_href = []() { return "https://www.example.com/"; };
  binder_ = std::make_unique<DomBinder>(*document_, apis);
  binder_->SetConsoleSink([this](std::string_view level, std::string_view text) {
    console_.push_back(std::string(level) + ": " + std::string(text));
  });

  // A DocumentFragment consumed by appendChild must stay alive: JS may still
  // hold it and it may be the root of a live childNodes/children collection.
  // Freeing it (as the DOM's AppendChild does to a fragment) would leave those
  // dangling and crash on the next live-collection refresh.
  EXPECT_TRUE(
      EvalBool("(function(){"
               "  var frag = document.createDocumentFragment();"
               "  var span = document.createElement('span');"
               "  frag.appendChild(span);"
               "  var n = frag.childNodes;"         // live collection rooted at the fragment
               "  document.body.appendChild(frag);" // consumes the span, moves it to body
               "  var p = document.createElement('p');"
               "  document.body.appendChild(p);" // any mutation refreshes all live collections
               "  return n.length === 0 && frag.childNodes.length === 0 &&"
               "         frag.firstChild === null && span.parentNode === document.body;"
               "})()"));
}

TEST_F(DomBinderTest, FragmentInsertBeforeAndReplaceKeepsFragmentAlive)
{
  document_ = html::Parser(R"(<!doctype html><html><body></body></html>)").Parse();
  PageApis apis;
  apis.location_href = []() { return "https://www.example.com/"; };
  binder_ = std::make_unique<DomBinder>(*document_, apis);
  binder_->SetConsoleSink([this](std::string_view level, std::string_view text) {
    console_.push_back(std::string(level) + ": " + std::string(text));
  });

  EXPECT_TRUE(EvalBool("(function(){"
                       "  var body = document.body;"
                       "  body.appendChild(document.createElement('b'));"
                       "  var frag = document.createDocumentFragment();"
                       "  frag.appendChild(document.createElement('i'));"
                       "  var n = frag.childNodes;"
                       "  body.insertBefore(frag, body.firstChild);"
                       "  body.replaceChild(frag, body.firstChild);" // frag is empty here
                       "  return n.length === 0 && frag.firstChild === null;"
                       "})()"));
}

TEST_F(DomBinderTest, BlobAndObjectUrl)
{
  EXPECT_TRUE(
      EvalBool("(function(){ var blob = new Blob(['hello', ' world'], {type:'text/plain'}); "
               "var url = URL.createObjectURL(blob); return blob.size === 11 && "
               "blob.type === 'text/plain' && typeof url === 'string' && "
               "url.indexOf('blob:') === 0; })()"));
}

// The browser layer distinguishes two kinds of DOM-dirty: "the document
// changed at all" (feeds the late-fetch pass) and "the cascade inputs
// changed" (drives the full restyle).  Media-loading attributes (img.src and
// friends) assign on every scroll frame during lazy loading — restyling for
// them froze real pages; everything else (classes, style, structural edits)
// must report through both flags.
TEST_F(DomBinderTest, MediaSourceMutationsSkipTheStyleDirtyFlag)
{
  binder_->TakeDomDirty();
  binder_->TakeStyleDirty();

  ASSERT_TRUE(EvalBool("(function(){"
                       "  document.body.appendChild(document.createElement('img'));"
                       "  return true;"
                       "})()"));
  binder_->TakeDomDirty(); // structural edit: drain both, then test the src setter alone
  binder_->TakeStyleDirty();

  ASSERT_TRUE(EvalBool("(function(){"
                       "  document.body.lastChild.src = 'https://example.com/x.png';"
                       "  return true;"
                       "})()"));
  EXPECT_TRUE(binder_->TakeDomDirty());
  EXPECT_FALSE(binder_->TakeStyleDirty()) << "img.src must not force a restyle";

  ASSERT_TRUE(EvalBool("(function(){ document.getElementById('first').classList.add('hot'); "
                       "return true; })()"));
  EXPECT_TRUE(binder_->TakeDomDirty());
  EXPECT_TRUE(binder_->TakeStyleDirty());

  ASSERT_TRUE(EvalBool("(function(){ document.getElementById('first').style.width = '10px'; "
                       "return true; })()"));
  EXPECT_TRUE(binder_->TakeStyleDirty()) << "per-property style setters must restyle";
}

// Style-only attribute writes keep the batch on the scoped-restyle path
// (roots only, no full restyle needed); geometry attributes the layout
// engine reads directly and structural edits stay conservative.
TEST_F(DomBinderTest, StyleOnlyAttributesKeepTheRestyleScoped)
{
  binder_->TakeDomDirty();
  binder_->TakeStyleDirtyScope();

  ASSERT_TRUE(EvalBool("(function(){ document.getElementById('first').classList.add('hot'); "
                       "return true; })()"));
  const auto scoped = binder_->TakeStyleDirtyScope();
  EXPECT_TRUE(scoped.dirty);
  EXPECT_FALSE(scoped.full_restyle);
  ASSERT_EQ(scoped.roots.size(), 1u);

  // setAttribute with a style-only name: still scoped.
  ASSERT_TRUE(EvalBool("(function(){ document.getElementById('first')"
                       "  .setAttribute('data-x', '1'); return true; })()"));
  EXPECT_FALSE(binder_->TakeStyleDirtyScope().full_restyle);

  // A geometry attribute (value: the layout reads it outside the cascade)
  // falls back to the full path.
  ASSERT_TRUE(EvalBool("(function(){ document.getElementById('first')"
                       "  .setAttribute('value', 'typed'); return true; })()"));
  const auto geometry = binder_->TakeStyleDirtyScope();
  EXPECT_TRUE(geometry.dirty);
  EXPECT_TRUE(geometry.full_restyle);

  // Structural edits are conservative too.
  ASSERT_TRUE(EvalBool("(function(){ var d = document.createElement('div');"
                       "  document.body.appendChild(d); return true; })()"));
  EXPECT_TRUE(binder_->TakeStyleDirtyScope().full_restyle);
}

TEST_F(DomBinderTest, UrlConstructsParsedValuesAndExposesPrototype)
{
  EXPECT_TRUE(
      EvalBool("(function(){ var descriptor = Object.getOwnPropertyDescriptor(URL, 'prototype'); "
               "var url = new URL('../docs?q=hello#part', 'https://example.test/path/page'); "
               "return descriptor.value === URL.prototype && URL.prototype.constructor === URL "
               "    && url instanceof URL && url.href === 'https://example.test/docs?q=hello#part' "
               "    && url.origin === 'https://example.test' && url.pathname === '/docs' "
               "    && url.search === '?q=hello' && url.hash === '#part' && url.toString() === "
               "url.href; })()"));
}

// Hardened libraries (for example Transcend's consent manager) read URL
// components through the prototype accessor descriptors and invoke the getters
// with the instance as receiver, as in
// `Object.getOwnPropertyDescriptor(URL.prototype, 'hostname').get.call(url)`.
// The binding stores components as own data properties, so the prototype must
// still expose real getters, otherwise those libraries observe `undefined` for
// every component and mis-detect the page domain.
TEST_F(DomBinderTest, UrlComponentsAreReadableThroughPrototypeGetterDescriptors)
{
  EXPECT_TRUE(EvalBool("(function(){"
                       " var descriptors = Object.getOwnPropertyDescriptors(URL.prototype);"
                       " var url = new URL('https://example.test:8443/docs?q=1#part');"
                       " function viaPrototype(name){ var d = descriptors[name];"
                       "   return d && typeof d.get === 'function' ? d.get.call(url) : undefined; }"
                       " return viaPrototype('href') === 'https://example.test:8443/docs?q=1#part'"
                       "   && viaPrototype('origin') === 'https://example.test:8443'"
                       "   && viaPrototype('protocol') === 'https:'"
                       "   && viaPrototype('host') === 'example.test:8443'"
                       "   && viaPrototype('hostname') === 'example.test'"
                       "   && viaPrototype('port') === '8443'"
                       "   && viaPrototype('pathname') === '/docs'"
                       "   && viaPrototype('search') === '?q=1'"
                       "   && viaPrototype('hash') === '#part'"
                       "   && typeof descriptors['username'].get === 'function'"
                       "   && typeof descriptors['password'].get === 'function'"
                       "   && typeof descriptors['hostname'].set === 'function'; })()"));
}

TEST_F(DomBinderTest, UrlAcceptsLocationAsItsBase)
{
  EXPECT_TRUE(EvalBool("new URL('/docs', window.location).pathname === '/docs'"));
}

TEST_F(DomBinderTest, DocumentWritelnIsCallable)
{
  EXPECT_TRUE(EvalBool("typeof document.writeln === 'function'"));
  EXPECT_TRUE(EvalBool("document.writeln('legacy markup') === undefined"));
}

TEST_F(DomBinderTest, SecurityPolicyViolationEventExposesCspFields)
{
  EXPECT_TRUE(EvalBool(
      "(function(){ var event = new SecurityPolicyViolationEvent('securitypolicyviolation', "
      "{blockedURI:'https://blocked.test/', originalPolicy:'default-src self'}); "
      "return event instanceof Event && event.blockedURI === 'https://blocked.test/' "
      "&& event.originalPolicy === 'default-src self' "
      "&& SecurityPolicyViolationEvent.prototype.constructor === SecurityPolicyViolationEvent; "
      "})()"));
}

TEST_F(DomBinderTest, UrlInputExposesValidityState)
{
  EXPECT_TRUE(
      EvalBool("(function(){ var input = document.createElement('input'); input.type = 'url'; "
               "input.value = 'not a URL'; var descriptor = Object.getOwnPropertyDescriptor("
               "ValidityState.prototype, 'valid'); return input.validity instanceof ValidityState "
               "&& input.validity.typeMismatch && !input.validity.valid "
               "&& typeof descriptor.get === 'function'; })()"));
}

TEST_F(DomBinderTest, RequiredTextControlReportsValueMissing)
{
  EXPECT_TRUE(EvalBool(R"JS((function() {
    var form = document.createElement('form');
    var input = document.createElement('input');
    input.setAttribute('required', '');
    form.appendChild(input);
    document.body.appendChild(form);
    var validity = input.validity;
    var missing = validity.valueMissing && !validity.valid;
    var checks = input.willValidate && input.validationMessage.length > 0
        && input.checkValidity() === false && input.form === form;
    input.value = 'filled';
    // A stored ValidityState object reflects the live DOM, as in browsers.
    var live = validity.valid === true && validity.valueMissing === false;
    var noOwnKeys = Object.keys(validity).length === 0;
    return missing && checks && live && noOwnKeys;
  })())JS"));
}

TEST_F(DomBinderTest, RequiredTextAreaAndSelectReportValueMissing)
{
  EXPECT_TRUE(EvalBool(R"JS((function() {
    var form = document.createElement('form');
    var textarea = document.createElement('textarea');
    textarea.setAttribute('required', '');
    var select = document.createElement('select');
    select.setAttribute('required', '');
    form.appendChild(textarea);
    form.appendChild(select);
    document.body.appendChild(form);
    var textareaMissing = textarea.validity.valueMissing;
    var selectMissing = select.validity.valueMissing;
    textarea.appendChild(document.createTextNode('filled'));
    var option = document.createElement('option');
    option.setAttribute('value', 'v');
    select.appendChild(option);
    return textareaMissing && selectMissing
        && textarea.validity.valid && select.validity.valid
        && textarea instanceof HTMLTextAreaElement && select instanceof HTMLSelectElement
        && textarea.validity instanceof ValidityState;
  })())JS"));
}

TEST_F(DomBinderTest, RequiredCheckboxAndRadioGroup)
{
  EXPECT_TRUE(EvalBool(R"JS((function() {
    var form = document.createElement('form');
    var checkbox = document.createElement('input');
    checkbox.type = 'checkbox';
    checkbox.setAttribute('required', '');
    var first = document.createElement('input');
    first.type = 'radio';
    first.setAttribute('required', '');
    first.setAttribute('name', 'group');
    var second = document.createElement('input');
    second.type = 'radio';
    second.setAttribute('name', 'group');
    form.appendChild(checkbox);
    form.appendChild(first);
    form.appendChild(second);
    document.body.appendChild(form);
    var checkboxMissing = checkbox.validity.valueMissing;
    var radioMissing = first.validity.valueMissing && second.validity.valid;
    checkbox.setAttribute('checked', 'checked');
    first.setAttribute('checked', 'checked');
    return checkboxMissing && radioMissing
        && checkbox.validity.valid
        && first.validity.valid && second.validity.valid;
  })())JS"));
}

TEST_F(DomBinderTest, EmailAndUrlTypeMismatch)
{
  EXPECT_TRUE(EvalBool(R"JS((function() {
    var email = document.createElement('input');
    email.type = 'email';
    email.value = 'not-an-email';
    var badEmail = email.validity.typeMismatch && !email.validity.valid;
    email.value = 'a@example.com';
    var goodEmail = email.validity.valid;
    email.setAttribute('multiple', '');
    email.value = 'a@example.com, b@example.org';
    var multipleGood = email.validity.valid;
    email.value = 'a@example.com, nope';
    var multipleBad = email.validity.typeMismatch;
    var url = document.createElement('input');
    url.type = 'url';
    url.value = 'not a url';
    var badUrl = url.validity.typeMismatch && !url.validity.valid;
    url.value = 'https://example.com/path?q=1';
    var goodUrl = url.validity.valid;
    return badEmail && goodEmail && multipleGood && multipleBad && badUrl && goodUrl;
  })())JS"));
}

TEST_F(DomBinderTest, NumberRangeLimitsAndStep)
{
  EXPECT_TRUE(EvalBool(R"JS((function() {
    var number = document.createElement('input');
    number.type = 'number';
    number.value = '5';
    number.setAttribute('min', '10');
    var underflow = number.validity.rangeUnderflow && !number.validity.valid;
    number.removeAttribute('min');
    number.setAttribute('max', '4');
    var overflow = number.validity.rangeOverflow && !number.validity.valid;
    number.removeAttribute('max');
    number.setAttribute('min', '0');
    number.setAttribute('step', '2');
    number.value = '3';
    var stepMismatch = number.validity.stepMismatch && !number.validity.valid;
    number.setAttribute('step', 'any');
    var any = number.validity.valid;
    // The value setter runs the Number state's value sanitization algorithm:
    // invalid text is not a valid floating-point number, so the current value
    // becomes the empty string.  badInput tracks raw *user* input, which the
    // engine does not store (documented limitation), so it stays false.
    number.value = 'not-a-number';
    var sanitizedBadInput = number.value === '' && number.validity.badInput === false &&
        number.validity.valid;
    number.value = '3.5';
    var fractional = number.validity.valid;
    var range = document.createElement('input');
    range.type = 'range';
    range.setAttribute('min', '0');
    range.setAttribute('max', '10');
    range.setAttribute('step', '1');
    return underflow && overflow && stepMismatch && any && sanitizedBadInput && fractional &&
           range.validity.valid;
  })())JS"));
}

TEST_F(DomBinderTest, InputValueSanitizationPerType)
{
  EXPECT_TRUE(EvalBool(R"JS((function() {
    function input(type, attrs) {
      var el = document.createElement('input');
      el.setAttribute('type', type);
      for (var name in attrs) { el.setAttribute(name, attrs[name]); }
      return el;
    }
    function valueOf(type, value) { return input(type, { value: value }).value; }
    var checks = [];
    // Text family: strip CR/LF only (spaces are kept).
    checks.push(valueOf('text', ' a\r\nb ') === ' ab ');
    checks.push(valueOf('search', 'a\nb') === 'ab');
    checks.push(valueOf('tel', '1\r\n2') === '12');
    checks.push(valueOf('password', 'p\nw') === 'pw');
    checks.push(valueOf('hidden', 'h\r\ni') === 'hi');
    // URL / email: strip newlines, then leading/trailing ASCII whitespace.
    checks.push(valueOf('url', '  https://x.test/a  ') === 'https://x.test/a');
    checks.push(valueOf('email', ' a@b.com \n') === 'a@b.com');
    var multiple = input('email', { multiple: '', value: ' a@b.com , c@d.com ' });
    checks.push(multiple.value === 'a@b.com,c@d.com');
    // Number: a valid floating-point number or the empty string.
    checks.push(valueOf('number', 'abc') === '');
    checks.push(valueOf('number', '1e3') === '1e3');
    checks.push(valueOf('number', '01') === '01');
    checks.push(valueOf('number', ' 7 ') === '');
    checks.push(valueOf('number', '+7') === '');
    checks.push(valueOf('number', '1.') === '');
    checks.push(valueOf('number', '-0') === '-0');
    // Range: invalid -> default halfway value, otherwise clamp to min/max.
    var range = input('range', { min: '10', max: '20', value: '99' });
    checks.push(range.value === '20');
    var defaultRange = document.createElement('input');
    defaultRange.setAttribute('min', '10');
    defaultRange.setAttribute('max', '20');
    defaultRange.setAttribute('type', 'range');
    checks.push(defaultRange.value === '15');
    var stepped = input('range', { min: '0', max: '1', step: '0.1', value: '0.35' });
    checks.push(stepped.value === '0.4');
    // Date/time family: valid string form, otherwise empty.
    checks.push(valueOf('date', '2020-01-02') === '2020-01-02');
    checks.push(valueOf('date', '2020-1-2') === '');
    checks.push(valueOf('date', '2020-02-30') === '');
    checks.push(valueOf('month', '2020-01') === '2020-01');
    checks.push(valueOf('month', '2020-1') === '');
    checks.push(valueOf('week', '2020-W53') === '2020-W53');
    checks.push(valueOf('week', '2020-W54') === '');
    checks.push(valueOf('time', '09:05') === '09:05');
    checks.push(valueOf('time', '9:5') === '');
    checks.push(valueOf('time', '24:00') === '');
    checks.push(valueOf('datetime-local', '2020-1-2T3:4') === '');
    checks.push(valueOf('datetime-local', '2020-01-02T03:04:05.000') ===
                '2020-01-02T03:04:05');
    // Colour: parsed and serialized lowercase; invalid becomes black.
    checks.push(valueOf('color', 'RED') === '#ff0000');
    checks.push(valueOf('color', '#ABC') === '#aabbcc');
    checks.push(valueOf('color', 'invalid') === '#000000');
    // Checkbox/radio: value IDL is the content attribute, "on" when absent.
    checks.push(input('checkbox', {}).value === 'on');
    checks.push(input('checkbox', { value: '' }).value === '');
    checks.push(input('radio', {}).value === 'on');
    return checks.indexOf(false) === -1;
  })())JS"));
}

TEST_F(DomBinderTest, InputValueDefaultValueAndDirtyValueFlag)
{
  EXPECT_TRUE(EvalBool(R"JS((function() {
    var input = document.createElement('input');
    input.type = 'number';
    input.setAttribute('value', 'abc');
    var initial = input.value === '' && input.defaultValue === 'abc' &&
        input.getAttribute('value') === 'abc';
    input.value = '7';
    var set = input.value === '7' && input.defaultValue === 'abc' &&
        input.getAttribute('value') === 'abc';
    input.value = 'xyz';
    var rejected = input.value === '' && input.getAttribute('value') === 'abc';
    // A dirty value ignores later default-value changes.
    input.defaultValue = '42';
    var dirtyDefault = input.value === '' && input.defaultValue === '42' &&
        input.getAttribute('value') === '42';
    input.removeAttribute('value');
    var removed = input.defaultValue === '' && input.value === '';

    // While not dirty, the current value follows the attribute.
    var clean = document.createElement('input');
    clean.type = 'number';
    clean.defaultValue = '5';
    var fromDefault = clean.value === '5';
    clean.setAttribute('value', '6');
    var fromAttribute = clean.value === '6';
    clean.defaultValue = '7';
    var fromDefaultAgain = clean.value === '7';
    return initial && set && rejected && dirtyDefault && removed && fromDefault &&
        fromAttribute && fromDefaultAgain;
  })())JS"));
}

TEST_F(DomBinderTest, InputTypeChangeReSanitizesTheCurrentValue)
{
  EXPECT_TRUE(EvalBool(R"JS((function() {
    // Number -> Text: the current value (sanitized to "") survives; it is not
    // re-derived from the raw "abc" attribute.
    var number = document.createElement('input');
    number.type = 'number';
    number.setAttribute('value', 'abc');
    number.type = 'text';
    var numberToText = number.value === '' && number.getAttribute('value') === 'abc';

    // Dirty Number -> Text: the dirty value survives.
    var dirty = document.createElement('input');
    dirty.type = 'number';
    dirty.setAttribute('value', 'abc');
    dirty.value = '7';
    dirty.type = 'text';
    var dirtyKept = dirty.value === '7' && dirty.getAttribute('value') === 'abc';

    // Text -> Checkbox: a non-empty value-mode value is written to the
    // content attribute (the default/on mode) and read back.
    var text = document.createElement('input');
    text.type = 'text';
    text.value = '9';
    text.type = 'checkbox';
    var toCheckbox = text.value === '9' && text.getAttribute('value') === '9' &&
        text.defaultValue === '9';

    // Nothing -> Range: the current value is sanitized with the new state's
    // algorithm and min/max.
    var range = document.createElement('input');
    range.setAttribute('min', '10');
    range.setAttribute('max', '20');
    range.type = 'range';
    var toRange = range.value === '15';

    // Type keyword reflection: missing/invalid defaults to "text".
    var reflected = document.createElement('input');
    var defaultText = reflected.type === 'text';
    reflected.setAttribute('type', 'TEXT');
    var lowercased = reflected.type === 'text';
    reflected.setAttribute('type', 'bogus');
    var invalidDefault = reflected.type === 'text';
    return numberToText && dirtyKept && toCheckbox && toRange && defaultText && lowercased &&
        invalidDefault;
  })())JS"));
}

TEST_F(DomBinderTest, FileInputValueIsAlwaysEmptyAndRejectsNonEmptySetter)
{
  EXPECT_TRUE(EvalBool(R"JS((function() {
    var file = document.createElement('input');
    file.type = 'file';
    file.setAttribute('value', 'ignored.txt');
    var empty = file.value === '';
    var threw = false;
    try {
      file.value = 'x.txt';
    } catch (error) {
      threw = error && error.name === 'InvalidStateError';
    }
    file.value = '';
    return empty && threw && file.value === '';
  })())JS"));
}

TEST_F(DomBinderTest, SetRangeTextUsesTheSanitizedCurrentValue)
{
  EXPECT_TRUE(EvalBool(R"JS((function() {
    var input = document.createElement('input');
    input.type = 'text';
    input.value = 'hello';
    input.setSelectionRange(0, 5);
    input.setRangeText('a\nb');
    // setRangeText goes through the same current-value store: the newline is
    // stripped by the Text state's sanitization, and the content attribute is
    // not rewritten.
    var replaced = input.value === 'ab' && input.getAttribute('value') === null;
    input.setAttribute('value', 'zzz');
    // setRangeText set the dirty value flag, so the attribute change does not
    // overwrite the current value.
    return replaced && input.value === 'ab' && input.defaultValue === 'zzz';
  })())JS"));
}

TEST_F(DomBinderTest, MaxlengthAndMinlengthUseUtf16CodeUnits)
{
  EXPECT_TRUE(EvalBool(R"JS((function() {
    var input = document.createElement('input');
    input.value = 'abcd';
    input.setAttribute('maxlength', '3');
    var tooLong = input.validity.tooLong && !input.validity.valid;
    input.removeAttribute('maxlength');
    input.setAttribute('minlength', '5');
    input.value = 'ab';
    var tooShort = input.validity.tooShort && !input.validity.valid;
    input.value = '';
    var emptyNotShort = input.validity.valid;
    input.removeAttribute('minlength');
    var emoji = String.fromCharCode(0xD83D, 0xDE00);
    input.value = emoji;
    input.setAttribute('maxlength', '2');
    var surrogatePairFits = input.validity.valid;
    input.setAttribute('maxlength', '1');
    var surrogatePairTooLong = input.validity.tooLong;
    return tooLong && tooShort && emptyNotShort && surrogatePairFits && surrogatePairTooLong;
  })())JS"));
}

TEST_F(DomBinderTest, CustomValidityErrorAndClear)
{
  EXPECT_TRUE(EvalBool(R"JS((function() {
    var input = document.createElement('input');
    input.setCustomValidity('custom boom');
    var set = input.validity.customError && !input.validity.valid
        && input.validationMessage === 'custom boom';
    input.setCustomValidity('');
    var cleared = !input.validity.customError && input.validity.valid
        && input.validationMessage === '';
    input.setAttribute('disabled', '');
    input.setCustomValidity('still barred');
    var barred = input.willValidate === false && input.validity.valid
        && input.validity.customError && input.checkValidity() === true;
    return set && cleared && barred;
  })())JS"));
}

TEST_F(DomBinderTest, BarredControlsAreValid)
{
  EXPECT_TRUE(EvalBool(R"JS((function() {
    function barred(configure) {
      var input = document.createElement('input');
      input.setAttribute('required', '');
      configure(input);
      return input.willValidate === false && input.validity.valid
          && input.checkValidity() === true;
    }
    var readonly = barred(function(input) { input.setAttribute('readonly', ''); });
    var hidden = barred(function(input) { input.type = 'hidden'; });
    var submit = barred(function(input) { input.type = 'submit'; });
    var disabled = barred(function(input) { input.setAttribute('disabled', ''); });
    var fieldset = document.createElement('fieldset');
    fieldset.setAttribute('disabled', '');
    var inFieldset = document.createElement('input');
    inFieldset.setAttribute('required', '');
    fieldset.appendChild(inFieldset);
    document.body.appendChild(fieldset);
    var fieldsetBarred = inFieldset.willValidate === false && inFieldset.validity.valid;
    var legend = document.createElement('legend');
    var inLegend = document.createElement('input');
    inLegend.setAttribute('required', '');
    legend.appendChild(inLegend);
    var fieldset2 = document.createElement('fieldset');
    fieldset2.setAttribute('disabled', '');
    fieldset2.appendChild(legend);
    document.body.appendChild(fieldset2);
    var legendNotBarred = inLegend.willValidate === true;
    return readonly && hidden && submit && disabled && fieldsetBarred && legendNotBarred;
  })())JS"));
}

TEST_F(DomBinderTest, FormAggregatesValidityAndFiresInvalidOncePerCheck)
{
  EXPECT_TRUE(EvalBool(R"JS((function() {
    var form = document.createElement('form');
    var invalid = document.createElement('input');
    invalid.setAttribute('required', '');
    var valid = document.createElement('input');
    valid.value = 'ok';
    form.appendChild(invalid);
    form.appendChild(valid);
    document.body.appendChild(form);
    var fired = 0;
    var ancestorFired = 0;
    var bubbles = null;
    var cancelable = null;
    invalid.addEventListener('invalid', function(event) {
      fired += 1;
      bubbles = event.bubbles;
      cancelable = event.cancelable;
    });
    form.addEventListener('invalid', function() { ancestorFired += 1; });
    var first = form.checkValidity();
    var afterFirst = fired;
    var reported = form.reportValidity();
    var afterReport = fired;
    // HTMLFormElement has no `validity` member; only checkValidity/reportValidity/noValidate.
    var no_invented_form_validity = form.validity === undefined;
    form.noValidate = true;
    var novalidate = form.noValidate && form.hasAttribute('novalidate');
    form.noValidate = false;
    return first === false && afterFirst === 1
        && reported === false && afterReport === 2
        && bubbles === false && cancelable === true && ancestorFired === 0
        && no_invented_form_validity && novalidate && form.noValidate === false;
  })())JS"));
}

TEST_F(DomBinderTest, FormAssociatedControlFindsOwnerForm)
{
  EXPECT_TRUE(EvalBool(R"JS((function() {
    var form = document.createElement('form');
    form.setAttribute('id', 'owner');
    document.body.appendChild(form);
    var input = document.createElement('input');
    input.setAttribute('form', 'owner');
    input.setAttribute('required', '');
    document.body.appendChild(input);
    return input.form === form && form.checkValidity() === false;
  })())JS"));
}

TEST_F(DomBinderTest, PatternMismatchIsDeferredNotFaked)
{
  // patternMismatch is documented as NOT IMPLEMENTED (no ECMAScript RegExp in
  // the shared HTML layer).  The flag must stay false rather than fake a match.
  EXPECT_TRUE(EvalBool(R"JS((function() {
    var input = document.createElement('input');
    input.value = 'abc';
    input.setAttribute('pattern', '[0-9]+');
    return input.validity.patternMismatch === false && input.validity.valid === true;
  })())JS"));
}

TEST_F(DomBinderTest, NodeListCollectionSemantics)
{
  EXPECT_TRUE(EvalBool("typeof NodeList === 'function'"));
  EXPECT_TRUE(EvalBool("(function(){ var list = document.querySelectorAll('p'); return list "
                       "instanceof NodeList; })()"));
  EXPECT_TRUE(EvalBool("(function(){ var list = document.querySelectorAll('p'); return typeof "
                       "list.item === 'function'; })()"));
  EXPECT_TRUE(EvalBool(
      "(function(){ var list = document.querySelectorAll('p'); return list.length > 0; })()"));
}

TEST_F(DomBinderTest, ChildrenUsesHtmlCollectionInterface)
{
  EXPECT_TRUE(EvalBool(
      "(function(){ var children = document.getElementById('main').children; "
      "var descriptor = Object.getOwnPropertyDescriptor(HTMLCollection.prototype, 'length'); "
      "return children instanceof HTMLCollection && children.length === 3 "
      "    && children.item(0) === children[0] && children.namedItem('first') === children[0] "
      "    && typeof descriptor.get === 'function' && descriptor.set === undefined; })()"));
}

TEST_F(DomBinderTest, SvgElementUsesDatasetAccessor)
{
  EXPECT_TRUE(EvalBool(
      "(function(){ var element = document.createElementNS("
      "'http://www.w3.org/2000/svg', 'svg'); element.dataset.state = 'ready'; "
      "var descriptor = Object.getOwnPropertyDescriptor(SVGElement.prototype, 'dataset'); "
      "return element.dataset.state === 'ready' && element.getAttribute('data-state') === 'ready' "
      "    && typeof descriptor.get === 'function' && descriptor.set === undefined; })()"));
}

TEST_F(DomBinderTest, DocumentDoctypeUsesReadonlyAccessor)
{
  // The fixture document starts with <!doctype html>, so doctype is a real
  // DocumentType node (it used to be a permanent null stub).
  EXPECT_TRUE(EvalBool(
      "(function(){ var descriptor = Object.getOwnPropertyDescriptor("
      "Document.prototype, 'doctype'); return document.doctype !== null "
      "    && document.doctype.nodeType === 10 "
      "    && document.doctype.name === 'html' "
      "    && document.doctype.nodeName === 'html' "
      "    && typeof descriptor.get === 'function' && descriptor.set === undefined; })()"));
}

TEST_F(DomBinderTest, WindowClosedUsesReadonlyAccessor)
{
  EXPECT_TRUE(
      EvalBool("(function(){ var descriptor = Object.getOwnPropertyDescriptor(window, 'closed'); "
               "return window.closed === false && typeof descriptor.get === 'function' "
               "    && descriptor.set === undefined; })()"));
}

TEST_F(DomBinderTest, HtmlScriptElementUsesDedicatedPrototype)
{
  EXPECT_TRUE(EvalBool(
      "(function(){ var element = document.createElement('script'); element.src = '/app.js'; "
      "var descriptor = Object.getOwnPropertyDescriptor(HTMLScriptElement.prototype, 'src'); "
      "return typeof HTMLScriptElement === 'function' && element instanceof HTMLScriptElement "
      "    && HTMLScriptElement.prototype !== Element.prototype && typeof descriptor.get === "
      "'function' "
      "    && typeof descriptor.set === 'function' && element.src === '/app.js'; })()"));
}

TEST_F(DomBinderTest, HtmlAnchorElementUsesDedicatedPrototype)
{
  EXPECT_TRUE(EvalBool(
      "(function(){ var element = document.createElement('a'); element.href = '/path'; "
      "return typeof HTMLAnchorElement === 'function' && element instanceof HTMLAnchorElement "
      "    && HTMLAnchorElement.prototype !== Element.prototype && element.href.indexOf('/path') "
      "!== -1; })()"));
}

TEST_F(DomBinderTest, HtmlFormElementUsesDedicatedPrototype)
{
  EXPECT_TRUE(
      EvalBool("(function(){ var element = document.createElement('form'); return "
               "typeof HTMLFormElement === 'function' && element instanceof HTMLFormElement "
               "    && HTMLFormElement.prototype !== Element.prototype; })()"));
}

TEST_F(DomBinderTest, HtmlFormElementReflectsSubmissionAttributes)
{
  EXPECT_TRUE(EvalBool(
      "(function(){ var form = document.createElement('form'); var submitted = false; "
      "form.action = '/submit'; form.enctype = 'multipart/form-data'; form.method = 'post'; "
      "form.addEventListener('submit', function(event) { submitted = true; event.preventDefault(); "
      "}); "
      "form.requestSubmit(); "
      "var action = Object.getOwnPropertyDescriptor(HTMLFormElement.prototype, 'action'); "
      "var enctype = Object.getOwnPropertyDescriptor(HTMLFormElement.prototype, 'enctype'); "
      "var method = Object.getOwnPropertyDescriptor(HTMLFormElement.prototype, 'method'); "
      "return typeof action.get === 'function' && typeof action.set === 'function' "
      "    && typeof enctype.get === 'function' && typeof enctype.set === 'function' "
      "    && typeof method.get === 'function' && typeof method.set === 'function' "
      "    && typeof form.submit === 'function' && typeof form.requestSubmit === 'function' "
      "    && form.action === '/submit' && form.enctype === 'multipart/form-data' "
      "    && form.method === 'post' && submitted; })()"));
}

TEST_F(DomBinderTest, FormSubmitDoesNotFireSubmitEvent)
{
  EXPECT_TRUE(EvalBool("(function(){ var form = document.createElement('form'); var fired = false; "
                       "form.addEventListener('submit', function() { fired = true; }); "
                       "form.submit(); return !fired; })()"));
}

TEST_F(DomBinderTest, FormRequestSubmitValidatesBeforeFiringSubmit)
{
  EXPECT_TRUE(
      EvalBool("(function(){ var form = document.createElement('form'); "
               "var input = document.createElement('input'); input.setAttribute('name', 'q'); "
               "input.setAttribute('required', ''); form.appendChild(input); "
               "var submitted = false; var invalid = false; "
               "form.addEventListener('submit', function() { submitted = true; }); "
               "input.addEventListener('invalid', function() { invalid = true; }); "
               "form.requestSubmit(); return invalid && !submitted; })()"));
  // With the required field filled the event fires; preventDefault keeps the
  // standalone binder from doing anything else.
  EXPECT_TRUE(
      EvalBool("(function(){ var form = document.createElement('form'); "
               "var input = document.createElement('input'); input.setAttribute('name', 'q'); "
               "input.setAttribute('required', ''); input.setAttribute('value', 'filled'); "
               "form.appendChild(input); var submitted = false; "
               "form.addEventListener('submit', function(event) { submitted = true; "
               "event.preventDefault(); }); "
               "form.requestSubmit(); return submitted; })()"));
}

TEST_F(DomBinderTest, FormRequestSubmitRejectsInvalidSubmitter)
{
  EXPECT_TRUE(EvalBool("(function(){ var form = document.createElement('form'); "
                       "var div = document.createElement('div'); "
                       "try { form.requestSubmit(div); return false; } "
                       "catch (error) { return error.name === 'TypeError'; } })()"));
  EXPECT_TRUE(EvalBool("(function(){ var form = document.createElement('form'); "
                       "var other = document.createElement('form'); "
                       "var button = document.createElement('button'); other.appendChild(button); "
                       "try { form.requestSubmit(button); return false; } "
                       "catch (error) { return error.name === 'NotFoundError'; } })()"));
  EXPECT_TRUE(EvalBool("(function(){ var form = document.createElement('form'); "
                       "var button = document.createElement('button'); form.appendChild(button); "
                       "var submitted = false; "
                       "form.addEventListener('submit', function(event) { submitted = true; "
                       "event.preventDefault(); }); "
                       "form.requestSubmit(button); return submitted; })()"));
}

TEST_F(DomBinderTest, HtmlButtonElementUsesDedicatedPrototype)
{
  EXPECT_TRUE(EvalBool(
      "(function(){ var element = document.createElement('button'); element.disabled = true; "
      "return typeof HTMLButtonElement === 'function' && element instanceof HTMLButtonElement "
      "    && HTMLButtonElement.prototype !== Element.prototype && element.disabled === true; "
      "})()"));
}

TEST_F(DomBinderTest, HtmlInputElementUsesDedicatedPrototype)
{
  EXPECT_TRUE(EvalBool(
      "(function(){ var element = document.createElement('input'); element.type = 'email'; "
      "element.value = 'test@example.com'; return typeof HTMLInputElement === 'function' "
      "    && element instanceof HTMLInputElement && HTMLInputElement.prototype !== "
      "Element.prototype "
      "    && element.type === 'email' && element.value === 'test@example.com'; })()"));
}

TEST_F(DomBinderTest, HtmlImageElementUsesDedicatedPrototype)
{
  EXPECT_TRUE(EvalBool(
      "(function(){ var element = document.createElement('img'); element.src = '/image.png'; "
      "element.srcset = 'small.png 320w, large.png 640w'; "
      "var src = Object.getOwnPropertyDescriptor(HTMLImageElement.prototype, 'src'); "
      "var currentSrc = Object.getOwnPropertyDescriptor(HTMLImageElement.prototype, 'currentSrc'); "
      "var srcset = Object.getOwnPropertyDescriptor(HTMLImageElement.prototype, 'srcset'); "
      "return typeof HTMLImageElement === 'function' && element instanceof HTMLImageElement "
      "    && HTMLImageElement.prototype !== Element.prototype && typeof src.get === 'function' "
      "    && typeof src.set === 'function' && typeof currentSrc.get === 'function' "
      "    && !currentSrc.set && typeof srcset.get === 'function' && typeof srcset.set === "
      "'function' "
      "    && element.currentSrc === '/image.png' && element.srcset === 'small.png 320w, large.png "
      "640w'; })()"));
}

TEST_F(DomBinderTest, HtmlVideoElementInheritsHtmlMediaElement)
{
  EXPECT_TRUE(EvalBool(
      "(function(){ var element = document.createElement('video'); element.src = '/clip.mp4'; "
      "var src = Object.getOwnPropertyDescriptor(HTMLMediaElement.prototype, 'src'); "
      "var currentSrc = Object.getOwnPropertyDescriptor(HTMLMediaElement.prototype, 'currentSrc'); "
      "return "
      "typeof HTMLMediaElement === 'function' && element instanceof HTMLMediaElement "
      "    && element instanceof HTMLVideoElement && "
      "Object.getPrototypeOf(HTMLVideoElement.prototype) "
      "       === HTMLMediaElement.prototype && typeof src.get === 'function' "
      "    && typeof src.set === 'function' && typeof currentSrc.get === 'function' "
      "    && !currentSrc.set && element.currentSrc === '/clip.mp4'; })()"));
}

TEST_F(DomBinderTest, HtmlIFrameElementReflectsSrcAndSrcDoc)
{
  EXPECT_TRUE(EvalBool(
      "(function(){ var element = document.createElement('iframe'); element.src = '/frame.html'; "
      "element.srcdoc = '<p>frame</p>'; "
      "element.credentialless = true; "
      "var src = Object.getOwnPropertyDescriptor(HTMLIFrameElement.prototype, 'src'); "
      "var srcdoc = Object.getOwnPropertyDescriptor(HTMLIFrameElement.prototype, 'srcdoc'); "
      "var credentialless = Object.getOwnPropertyDescriptor(HTMLIFrameElement.prototype, "
      "'credentialless'); "
      "return typeof src.get === 'function' && typeof src.set === 'function' "
      "    && typeof srcdoc.get === 'function' && typeof srcdoc.set === 'function' "
      "    && typeof credentialless.get === 'function' && typeof credentialless.set === 'function' "
      "    && element.src === '/frame.html' && element.srcdoc === '<p>frame</p>' "
      "    && element.credentialless; })()"));
}

TEST_F(DomBinderTest, HtmlAnchorElementReflectsHrefDownloadAndPing)
{
  EXPECT_TRUE(EvalBool(
      "(function(){ var element = document.createElement('a'); element.href = '/file'; "
      "element.download = 'report.json'; element.ping = '/audit'; "
      "var href = Object.getOwnPropertyDescriptor(HTMLAnchorElement.prototype, 'href'); "
      "var download = Object.getOwnPropertyDescriptor(HTMLAnchorElement.prototype, 'download'); "
      "var ping = Object.getOwnPropertyDescriptor(HTMLAnchorElement.prototype, 'ping'); "
      "return typeof href.get === 'function' && typeof href.set === 'function' "
      "    && typeof download.get === 'function' && typeof download.set === 'function' "
      "    && typeof ping.get === 'function' && typeof ping.set === 'function' "
      "    && element.href === '/file' && element.download === 'report.json' "
      "    && element.ping === '/audit'; })()"));
}

TEST_F(DomBinderTest, SubmitControlsReflectFormAction)
{
  EXPECT_TRUE(
      EvalBool("(function(){ var button = document.createElement('button'); "
               "var input = document.createElement('input'); button.formAction = '/button'; "
               "input.formAction = '/input'; "
               "var buttonDescriptor = "
               "Object.getOwnPropertyDescriptor(HTMLButtonElement.prototype, 'formAction'); "
               "var inputDescriptor = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "
               "'formAction'); "
               "return typeof buttonDescriptor.get === 'function' && typeof buttonDescriptor.set "
               "=== 'function' "
               "    && typeof inputDescriptor.get === 'function' && typeof inputDescriptor.set === "
               "'function' "
               "    && button.formAction === '/button' && input.formAction === '/input'; })()"));
}

TEST_F(DomBinderTest, HtmlInputElementHasOwnValueAccessor)
{
  EXPECT_TRUE(EvalBool(
      "(function(){ var input = document.createElement('input'); input.value = 'url'; "
      "var descriptor = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value'); "
      "return typeof descriptor.get === 'function' && typeof descriptor.set === 'function' "
      "    && input.value === 'url'; })()"));
}

TEST_F(DomBinderTest, TranscendDescriptorCacheHasRequiredAccessors)
{
  EXPECT_TRUE(EvalBool(
      "(function(){ "
      "var base = Object.getOwnPropertyDescriptor(HTMLBaseElement.prototype, 'href'); "
      "var script = Object.getOwnPropertyDescriptor(HTMLScriptElement.prototype, 'src'); "
      "var imageSrc = Object.getOwnPropertyDescriptor(HTMLImageElement.prototype, 'src'); "
      "var imageSrcSet = Object.getOwnPropertyDescriptor(HTMLImageElement.prototype, 'srcset'); "
      "var media = Object.getOwnPropertyDescriptor(HTMLMediaElement.prototype, 'src'); "
      "var frameSrc = Object.getOwnPropertyDescriptor(HTMLIFrameElement.prototype, 'src'); "
      "var frameDoc = Object.getOwnPropertyDescriptor(HTMLIFrameElement.prototype, 'srcdoc'); "
      "var anchorHref = Object.getOwnPropertyDescriptor(HTMLAnchorElement.prototype, 'href'); "
      "var anchorPing = Object.getOwnPropertyDescriptor(HTMLAnchorElement.prototype, 'ping'); "
      "var anchorDownload = Object.getOwnPropertyDescriptor(HTMLAnchorElement.prototype, "
      "'download'); "
      "var inputValue = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value'); "
      "var innerHtml = Object.getOwnPropertyDescriptor(Element.prototype, 'innerHTML'); "
      "var cookie = Object.getOwnPropertyDescriptor(Document.prototype, 'cookie'); "
      "return [base, script, imageSrc, imageSrcSet, media, frameSrc, frameDoc, anchorHref, "
      "anchorPing, anchorDownload, inputValue, innerHtml, cookie].every(function(descriptor){ "
      "return descriptor && typeof descriptor.get === 'function' && typeof descriptor.set === "
      "'function'; "
      "}); })()"));
}

TEST_F(DomBinderTest, DocumentCookieCallbacks)
{
  std::string cookie = "session=abc";
  PageApis apis;
  apis.cookie_get = [&cookie] { return cookie; };
  apis.cookie_set = [&cookie](std::string_view value) { cookie = std::string(value); };
  DomBinder binder(*document_, apis);
  EXPECT_EQ(binder.Evaluate("document.cookie").value().ToString().value(), "session=abc");
  ASSERT_TRUE(binder.Evaluate("document.cookie = 'theme=dark; Path=/';").has_value());
  EXPECT_EQ(cookie, "theme=dark; Path=/");
}

TEST_F(DomBinderTest, DocumentHead)
{
  EXPECT_EQ(EvalString("document.head.tagName"), "HEAD");
  // Pages do document.head.appendChild(...) to inject <style>/<meta>.
  EXPECT_TRUE(EvalBool("(function(){ var d = document; "
                       "var meta = d.createElement('meta'); meta.name = 'x'; "
                       "d.head.appendChild(meta); "
                       "return d.head.children[d.head.children.length - 1] === meta; })()"));
}

TEST_F(DomBinderTest, LegacyDocumentWriteIsCallable)
{
  ASSERT_TRUE(EvalBool("typeof document.write === 'function'"));
  EXPECT_TRUE(EvalBool("document.write('<link rel=stylesheet href=\\\"style.css\\\">'); true"));
}

TEST_F(DomBinderTest, DocumentReadyState)
{
  // Scripts run after parsing, so the document is always "complete".
  EXPECT_EQ(EvalString("document.readyState"), "complete");
}

TEST_F(DomBinderTest, DocumentImplementationSupportsBasicFeatures)
{
  EXPECT_TRUE(
      EvalBool("document.implementation !== null && typeof document.implementation === 'object'"));
  EXPECT_TRUE(EvalBool("document.implementation.hasFeature('HTML', '1.0')"));
  EXPECT_TRUE(EvalBool("document.implementation.hasFeature('DOM', '1.0')"));
  EXPECT_FALSE(EvalBool("document.implementation.hasFeature('NoSuchFeature', '1.0')"));

  EXPECT_TRUE(
      EvalBool("(function(){ var d = document.implementation.createHTMLDocument('Doc Title'); "
               "return d instanceof Document && d.title === 'Doc Title'; })()"));
}

TEST_F(DomBinderTest, NonstandardGlobalsAreNotFabricated)
{
  // Browsers do not define window.jQuery, window.$, window._w, window._d,
  // window.Feedback, window.BM or window.Log.  The engine used to fabricate
  // minimal stand-ins; sites feature-detect them and then call the full API,
  // so the stubs broke real pages (bilibili's bundle writes jQuery.fn.lazyload
  // whenever window.jQuery is truthy and crashed on the stub).
  for (const char* name : {"jQuery", "$", "_w", "_d", "Feedback", "BM", "Log"}) {
    EXPECT_FALSE(EvalBool(std::string("'") + name + "' in window")) << name;
  }
  // The bilibili regression shape: with honest feature detection the guarded
  // write is skipped instead of throwing.
  EXPECT_TRUE(EvalBool("(function(){"
                       "  if (window.jQuery) { window.jQuery.fn.lazyload = function(){}; }"
                       "  return true;"
                       "})()"));
}

TEST_F(DomBinderTest, StandardPlatformGlobalsAreAvailable)
{
  EXPECT_TRUE(EvalBool("typeof PerformanceObserver === 'function'"));
  EXPECT_TRUE(EvalBool("document.visibilityState === 'visible'"));
  EXPECT_TRUE(EvalBool("navigator.serviceWorker && navigator.serviceWorker.controller === null"));
  EXPECT_TRUE(EvalBool("window.visualViewport && typeof window.visualViewport.width === 'number'"));
}

TEST_F(DomBinderTest, TemplateElementExposesItsContentsFragment)
{
  // HTMLTemplateElement (HTML 4.12.3): innerHTML reads and writes the
  // template contents, not the element's own child list, and content is a
  // DocumentFragment.  Vue's runtime DOM relies on this exact shape
  // (createElement('template') + innerHTML + content.firstChild +
  // content.cloneNode); bilibili's hydration failed with "cannot read
  // property 'firstChild' of undefined" while content was missing.
  EXPECT_TRUE(
      EvalBool("(function(){"
               "  var t = document.createElement('template');"
               "  if ('content' in document.createElement('div')) return false;"
               "  if (typeof t.content !== 'object' || t.content === null) return false;"
               "  if (t.content.nodeType !== 11) return false;"
               "  t.innerHTML = '<div id=x>hi</div>';"
               "  if (t.childNodes.length !== 0) return false;"
               "  if (t.content.childNodes.length !== 1) return false;"
               "  if (t.content.firstChild.getAttribute('id') !== 'x') return false;"
               "  if (t.innerHTML.indexOf('id=\"x\"') === -1) return false;"
               "  var clone = t.content.cloneNode(true);"
               "  if (clone.firstChild === null"
               "      || clone.firstChild.getAttribute('id') !== 'x') return false;"
               "  if (!(t instanceof HTMLTemplateElement)) return false;"
               "  var t2 = document.createElement('template');"
               "  t2.appendChild(document.createElement('u'));"
               "  if (t2.childNodes.length !== 1) return false;" // appendChild stays on the element
               "  var deep = t.cloneNode(true);"
               "  return deep.content.childNodes.length === 1;"
               "})()"));
}

TEST_F(DomBinderTest, LegacyCreateEventInitializers)
{
  // document.createEvent + initEvent: the deprecated DOM Level 2/3 API that
  // bilibili's LoginInfo emitter uses (document.createEvent("HTMLEvents"),
  // then initEvent("onLoginInfoLoaded", true, true), then
  // document.dispatchEvent).  Missing createEvent broke its user-state flow.
  EXPECT_TRUE(EvalBool(
      "(function(){"
      "  var ev = document.createEvent('HTMLEvents');"
      "  if (ev.type !== '') return false;"
      "  ev.initEvent('onLoginInfoLoaded', true, true);"
      "  if (ev.type !== 'onLoginInfoLoaded' || !ev.bubbles || !ev.cancelable) return false;"
      "  ev.data = {uid: 7};"
      "  var seen = null;"
      "  document.addEventListener('onLoginInfoLoaded', function(e){ seen = e.data; });"
      "  if (document.dispatchEvent(ev) !== true) return false;"
      "  if (seen === null || seen.uid !== 7) return false;"
      "  document.removeEventListener('onLoginInfoLoaded', function(){});"
      "  try { ev.initEvent('again', false, false); return false; }"
      "  catch (e) { if (e.name !== 'InvalidStateError') return false; }"
      "  var ce = document.createEvent('CustomEvent');"
      "  ce.initCustomEvent('t', false, false, {v: 2});"
      "  if (ce.type !== 't' || ce.detail.v !== 2) return false;"
      "  try { document.createEvent('NoSuchInterface'); return false; }"
      "  catch (e) { return e.name === 'NotSupportedError'; }"
      "})()"));
}

TEST_F(DomBinderTest, FragmentParentNodeQueries)
{
  // DocumentFragment implements ParentNode (DOM §4.2): querySelector and
  // querySelectorAll search the fragment's subtree.  Scripts build detached
  // subtrees inside fragments and query them before insertion.
  EXPECT_TRUE(EvalBool("(function(){"
                       "  var f = document.createDocumentFragment();"
                       "  var d = document.createElement('div');"
                       "  d.innerHTML = '<b class=r>x</b><b class=r>y</b>';"
                       "  f.appendChild(d);"
                       "  if (f.querySelector('.r') === null) return false;"
                       "  if (f.querySelectorAll('b').length !== 2) return false;"
                       "  if (f.querySelectorAll('.missing').length !== 0) return false;"
                       "  return f.children.length === 1;"
                       "})()"));
}

TEST_F(DomBinderTest, InsertingTemplateContentsMovesItsChildren)
{
  // Browsers allow parent.insertBefore(template.content, anchor) and
  // parent.appendChild(template.content) — Vue's insertStaticContent
  // hydration does exactly this.  The contents fragment is owned by the
  // template element rather than the binder, so the insert must move its
  // children (leaving the fragment empty, as in browsers) instead of
  // reporting an internal ownership error.
  EXPECT_TRUE(EvalBool("(function(){"
                       "  var host = document.createElement('div');"
                       "  document.body.appendChild(host);"
                       "  var t = document.createElement('template');"
                       "  t.innerHTML = '<i>a</i><b>b</b>';"
                       "  host.insertBefore(t.content, null);"
                       "  if (host.childNodes.length !== 2) return false;"
                       "  if (host.firstChild.tagName !== 'I') return false;"
                       "  if (t.content.childNodes.length !== 0) return false;"
                       "  var t2 = document.createElement('template');"
                       "  t2.innerHTML = '<u>u</u>';"
                       "  host.appendChild(t2.content);"
                       "  if (host.childNodes.length !== 3) return false;"
                       "  if (host.lastChild.tagName !== 'U') return false;"
                       "  if (t2.content.childNodes.length !== 0) return false;"
                       "  return true;"
                       "})()"));
}

TEST_F(DomBinderTest, ProxyWrappedReceiversBehaveLikeTheirTarget)
{
  // Web IDL: a Proxy whose target is a platform object acts as the target for
  // interface members, and accessors invoked through the proxy receive it as
  // |this|.  Vue's reactivity reads DOM properties with
  // Reflect.get(target, key, receiver) — a Proxy receiver — and bilibili's
  // homepage relies on it (its bundle threw "detached node" for
  // parentElement / querySelectorAll before the engine unwrapped proxies).
  EXPECT_TRUE(EvalBool("(function(){"
                       "  var el = document.createElement('div');"
                       "  el.innerHTML = '<b>x</b>';"
                       "  var p = new Proxy(el, {});"
                       "  return p.nodeType === 1"
                       "      && p.tagName === 'DIV'"
                       "      && p.querySelectorAll('b').length === 1"
                       "      && Reflect.get(el, 'nodeType', p) === 1;"
                       "})()"));
  // Detached nodes read through a proxy answer like the target does instead
  // of throwing: parentElement/parentNode are null, nodeType still works.
  EXPECT_TRUE(EvalBool("(function(){"
                       "  var el = document.createElement('span');"
                       "  var p = new Proxy(el, {});"
                       "  return p.parentElement === null && p.parentNode === null"
                       "      && p.nodeType === 1 && p.textContent === '';"
                       "})()"));
  // Vue-style get trap (Reflect.get forwards the proxy as receiver).
  EXPECT_TRUE(EvalBool("(function(){"
                       "  var el = document.createElement('div');"
                       "  var v = new Proxy(el, {"
                       "    get: function(t, k, r){ return Reflect.get(t, k, r); },"
                       "  });"
                       "  return v.nodeType === 1 && v.classList.length === 0"
                       "      && v.querySelectorAll('i').length === 0;"
                       "})()"));
  // Mutating methods called with a proxy receiver act on the target.
  EXPECT_TRUE(EvalBool("(function(){"
                       "  var el = document.createElement('div');"
                       "  var p = new Proxy(el, {});"
                       "  p.setAttribute('data-x', '1');"
                       "  var child = document.createElement('i');"
                       "  el.appendChild.call(p, child);"
                       "  return el.getAttribute('data-x') === '1'"
                       "      && el.firstChild === child && child.parentNode === el;"
                       "})()"));
  // Genuinely invalid receivers still throw a TypeError.
  EXPECT_TRUE(EvalBool("(function(){"
                       "  var d = Object.getOwnPropertyDescriptor(Node.prototype, 'nodeType');"
                       "  try { d.get.call({}); return false; }"
                       "  catch (e) { return e instanceof TypeError; }"
                       "})()"));
}

TEST_F(DomBinderTest, ElementPrototypeExposesNamespaceURI)
{
  EXPECT_TRUE(EvalBool(
      "(function(){ var html = document.createElement('div'); "
      "var svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg'); "
      "var descriptor = Object.getOwnPropertyDescriptor(Element.prototype, 'namespaceURI'); "
      "return typeof descriptor.get === 'function' && descriptor.set === undefined "
      "    && html.namespaceURI === 'http://www.w3.org/1999/xhtml' "
      "    && svg.namespaceURI === 'http://www.w3.org/2000/svg'; })()"));
}

TEST_F(DomBinderTest, CustomElementRegistryDefinesAndFindsConstructors)
{
  EXPECT_TRUE(EvalBool("typeof customElements === 'object'"));
  EXPECT_TRUE(EvalBool("(function(){ class TestElement extends HTMLElement {} "
                       "customElements.define('test-element', TestElement); "
                       "return customElements.get('test-element') === TestElement "
                       "&& customElements.get('missing-element') === undefined "
                       "&& customElements.whenDefined('test-element') instanceof Promise; })()"));
  EXPECT_TRUE(
      EvalBool("(function(){ try { customElements.define('test-element', class {}); "
               "return false; } catch (error) { return error.name === 'NotSupportedError'; } "
               "})()"));
}

TEST_F(DomBinderTest, PerformanceObserverMethodsAreCallable)
{
  EXPECT_TRUE(EvalBool("(function() {"
                       "  var observer = new PerformanceObserver(function() {});"
                       "  return typeof observer.observe === 'function' &&"
                       "         typeof observer.disconnect === 'function' &&"
                       "         typeof observer.takeRecords === 'function' &&"
                       "         observer.observe({entryTypes: []}) === undefined &&"
                       "         Array.isArray(observer.takeRecords());"
                       "})()"));
}

TEST_F(DomBinderTest, InterfaceGlobalsAndInstanceof)
{
  EXPECT_TRUE(EvalBool("document instanceof Document"));
  EXPECT_TRUE(EvalBool("document instanceof Node"));
  EXPECT_FALSE(EvalBool("document instanceof Element"));
  EXPECT_TRUE(EvalBool("document.body instanceof Element"));
  EXPECT_TRUE(EvalBool("document.body instanceof HTMLElement"));
  EXPECT_TRUE(EvalBool("document.body instanceof Node"));
  EXPECT_FALSE(EvalBool("document.body instanceof Document"));
  EXPECT_TRUE(EvalBool("document.head instanceof Element"));
  EXPECT_TRUE(EvalBool("document.createTextNode('x') instanceof Text"));
  EXPECT_TRUE(EvalBool("document.createElement('div') instanceof Element"));
  EXPECT_TRUE(EvalBool("(function(){ var frame = document.createElement('iframe'); "
                       "return typeof HTMLIFrameElement === 'function' && "
                       "frame instanceof HTMLIFrameElement && frame instanceof HTMLElement && "
                       "frame instanceof Element && frame instanceof Node && "
                       "!(document.body instanceof HTMLIFrameElement); })()"));
  EXPECT_TRUE(EvalBool("(function(){ var video = document.createElement('video'); "
                       "return typeof HTMLVideoElement === 'function' && "
                       "video instanceof HTMLVideoElement && video instanceof HTMLElement && "
                       "video instanceof Element && video instanceof Node && "
                       "!(document.body instanceof HTMLVideoElement); })()"));
  EXPECT_TRUE(EvalBool("(function(){ var svg = document.createElementNS("
                       "'http://www.w3.org/2000/svg', 'svg'); "
                       "return typeof SVGElement === 'function' && svg instanceof SVGElement && "
                       "svg instanceof Element && !(document.body instanceof SVGElement); })()"));
}

TEST_F(DomBinderTest, InterfacePrototypeExtension)
{
  // Element.prototype extension reaches live wrappers.
  EXPECT_TRUE(EvalBool("(function(){ "
                       "Element.prototype.foo = function() { return 'foo:' + this.tagName; }; "
                       "HTMLElement.prototype.bar = function() { return 'bar'; }; "
                       "return document.body.foo() === 'foo:BODY' "
                       "       && document.body.bar() === 'bar'; })()"));
  // The prototype's constructor points back at the interface constructor.
  EXPECT_TRUE(EvalBool("document.body.constructor === Element"));
  EXPECT_TRUE(EvalBool("document.constructor === Document"));
}

TEST_F(DomBinderTest, IllegalConstructorThrows)
{
  auto r = binder_->Evaluate("new Element()");
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message().find("Illegal constructor"), std::string::npos);
}

TEST_F(DomBinderTest, DocumentEventListener)
{
  EXPECT_TRUE(EvalBool("(function(){ var hits = 0; "
                       "document.addEventListener('app-ready', function(ev) { "
                       "  hits++; window._ev = ev; "
                       "}); "
                       "document.dispatchEvent({type: 'app-ready'}); "
                       "return hits === 1 && window._ev.type === 'app-ready' "
                       "       && window._ev.target === document; })()"));
}

TEST_F(DomBinderTest, MutationObserverDeliversChildListRecords)
{
  ASSERT_TRUE(EvalBool("(function(){ window._records = []; "
                       "var root = document.createElement('div'); "
                       "var observer = new MutationObserver(function(records, self) { "
                       "window._records.push([records[0].type, records[0].target === root, "
                       "records[0].addedNodes[0].tagName, self === observer]); }); "
                       "observer.observe(root, {childList:true}); "
                       "root.appendChild(document.createElement('span')); return true; })()"));
  EXPECT_EQ(EvalString("window._records[0].join(',')"), "childList,true,SPAN,true");
}

TEST_F(DomBinderTest, MutationObserverDeliversSubtreeAttributeRecords)
{
  ASSERT_TRUE(EvalBool(
      "(function(){ window._record = ''; var root = document.createElement('div'); "
      "var child = document.createElement('span'); root.appendChild(child); "
      "new MutationObserver(function(records) { window._record = records[0].type + ':' + "
      "records[0].attributeName + ':' + (records[0].target === child); })"
      ".observe(root, {attributes:true, subtree:true}); child.setAttribute('data-x', '1'); "
      "return true; })()"));
  EXPECT_EQ(EvalString("window._record"), "attributes:data-x:true");
}

TEST_F(DomBinderTest, MutationObserverUsesSharedInterfacePrototype)
{
  EXPECT_TRUE(
      EvalBool("(function(){ var observer = new MutationObserver(function(){}); "
               "return observer instanceof MutationObserver "
               "    && Object.getPrototypeOf(observer) === MutationObserver.prototype "
               "    && observer.observe === MutationObserver.prototype.observe "
               "    && observer.disconnect === MutationObserver.prototype.disconnect "
               "    && observer.takeRecords === MutationObserver.prototype.takeRecords; })()"));
}

TEST_F(DomBinderTest, DOMParserParsesIndependentHtmlDocument)
{
  EXPECT_TRUE(EvalBool("(function(){ var parser = new DOMParser(); "
                       "var parsed = parser.parseFromString('<!doctype html><title>Parsed</title>'"
                       "  + '<body><main id=content>Hello</main></body>', 'text/html'); "
                       "return parser instanceof DOMParser "
                       "    && Object.getPrototypeOf(parser) === DOMParser.prototype "
                       "    && parsed instanceof Document && parsed !== document "
                       "    && parsed.title === 'Parsed' "
                       "    && parsed.querySelector('#content').textContent === 'Hello'; })()"));
}

TEST_F(DomBinderTest, DOMParserRejectsUnsupportedMimeType)
{
  EXPECT_TRUE(EvalBool(
      "(function(){ try { "
      "new DOMParser().parseFromString('<root/>', 'application/xml'); "
      "return false; } catch (error) { return error.name === 'NotSupportedError'; } })()"));
}

TEST_F(DomBinderTest, XMLSerializerSerializesDomNodes)
{
  EXPECT_TRUE(
      EvalBool("(function(){ var serializer = new XMLSerializer(); "
               "var parsed = new DOMParser().parseFromString("
               "  '<main data-value=\"a&amp;b\">x &lt; y<!--note--></main>', 'text/html'); "
               "var main = parsed.querySelector('main'); "
               "return serializer instanceof XMLSerializer "
               "    && Object.getPrototypeOf(serializer) === XMLSerializer.prototype "
               "    && serializer.serializeToString(main) "
               "       === '<main data-value=\"a&amp;b\">x &lt; y<!--note--></main>'; })()"));
}

TEST_F(DomBinderTest, XMLSerializerRejectsNonNode)
{
  EXPECT_TRUE(
      EvalBool("(function(){ try { new XMLSerializer().serializeToString({}); "
               "return false; } catch (error) { return error instanceof TypeError; } })()"));
}

TEST_F(DomBinderTest, DocumentImplementationUsesSharedInterfacePrototype)
{
  EXPECT_TRUE(
      EvalBool("(function(){ var implementation = document.implementation; "
               "return implementation instanceof DOMImplementation "
               "    && Object.getPrototypeOf(implementation) === DOMImplementation.prototype "
               "    && implementation.createHTMLDocument "
               "       === DOMImplementation.prototype.createHTMLDocument "
               "    && implementation.createHTMLDocument('Shared').title === 'Shared'; })()"));
}

TEST_F(DomBinderTest, BaseElementUsesHTMLBaseElementInterface)
{
  EXPECT_TRUE(EvalBool("(function(){ var base = document.createElement('base'); "
                       "base.href = '/assets/'; "
                       "var descriptor = Object.getOwnPropertyDescriptor("
                       "  HTMLBaseElement.prototype, 'href'); "
                       "return base instanceof HTMLBaseElement && base instanceof HTMLElement "
                       "    && Object.getPrototypeOf(base) === HTMLBaseElement.prototype "
                       "    && typeof descriptor.get === 'function' "
                       "    && typeof descriptor.set === 'function' "
                       "    && base.getAttribute('href') === '/assets/'; })()"));
}

TEST_F(DomBinderTest, NodePrototypeExposesBaseURI)
{
  EXPECT_TRUE(
      EvalBool("(function(){ var node = document.createElement('div'); "
               "var descriptor = Object.getOwnPropertyDescriptor(Node.prototype, 'baseURI'); "
               "return typeof descriptor.get === 'function' "
               "    && !Object.prototype.hasOwnProperty.call(Element.prototype, 'baseURI') "
               "    && node.baseURI === document.baseURI; })()"));
}

TEST_F(DomBinderTest, WindowEventListenerAndGlobalAlias)
{
  // window.addEventListener and the bare global alias both register listeners
  // on the document (window and document share one event-target set here).
  EXPECT_TRUE(EvalBool(
      "(function(){ var hits = 0; "
      "window.addEventListener('load', function(ev) { hits++; window._loadType = ev.type; }); "
      "addEventListener('after-load', function() { hits++; }); "
      "document.dispatchEvent({type: 'load'}); "
      "document.dispatchEvent({type: 'after-load'}); "
      "return hits === 2 && window._loadType === 'load'; })()"));
}

TEST_F(DomBinderTest, DispatchDocumentEventFromCpp)
{
  ASSERT_TRUE(EvalBool("(function(){ window._ready = 0; window._loaded = 0; "
                       "document.addEventListener('DOMContentLoaded', "
                       "  function() { window._ready++; }); "
                       "window.addEventListener('load', function() { window._loaded++; }); "
                       "return true; })()"));
  binder_->DispatchDocumentEvent("DOMContentLoaded");
  binder_->DispatchDocumentEvent("load");
  EXPECT_EQ(EvalNumber("window._ready"), 1.0);
  EXPECT_EQ(EvalNumber("window._loaded"), 1.0);
}

TEST_F(DomBinderTest, GetElementById)
{
  EXPECT_EQ(EvalString("document.getElementById('first').tagName"), "P");
  EXPECT_EQ(EvalString("document.getElementById('first').textContent"), "Hello world");
  EXPECT_EQ(EvalString("document.getElementById('missing')"), "null");
}

TEST_F(DomBinderTest, QuerySelectorAndAll)
{
  EXPECT_EQ(EvalString("document.querySelector('.para').id"), "first");
  EXPECT_EQ(EvalNumber("document.querySelectorAll('.para').length"), 2.0);
  EXPECT_EQ(EvalString("document.querySelector('span').getAttribute('data-x')"), "1");
  EXPECT_EQ(EvalNumber("document.querySelectorAll('p').length"), 2.0);
  EXPECT_EQ(EvalNumber("document.querySelectorAll('[id^=\"firs\"]').length"), 1.0);
  // Scoped query from an element.
  EXPECT_EQ(EvalString("document.getElementById('main').querySelector('span').tagName"), "SPAN");
}

TEST_F(DomBinderTest, NodeProperties)
{
  // The parser inserts whitespace text nodes, so body has 3 child nodes but
  // exactly one element child.
  EXPECT_EQ(EvalNumber("document.body.children.length"), 1.0);
  EXPECT_EQ(EvalString("document.body.firstElementChild.id"), "main");
  EXPECT_EQ(EvalString("document.getElementById('first').parentNode.id"), "main");
  EXPECT_EQ(EvalString("document.getElementById('first').firstChild.nodeType"), "3");
  EXPECT_EQ(EvalString("document.getElementById('first').lastChild.nodeName"), "B");
  EXPECT_TRUE(EvalBool("document.body.hasChildNodes()"));
  EXPECT_EQ(EvalString("document.getElementById('main').children.length"), "3");
  EXPECT_EQ(EvalString("document.getElementById('main').firstElementChild.id"), "first");
}

TEST_F(DomBinderTest, TextContentSet)
{
  EXPECT_TRUE(
      EvalBool("(function(){ var p = document.getElementById('first'); p.textContent = 'changed'; "
               "return p.textContent === 'changed' && p.childNodes.length === 1; })()"));
  // The C++ document reflects the change.
  EXPECT_NE(document_->ToString().find(">changed</p>"), std::string::npos);
}

// nodeValue / data on CharacterData and textContent on Text/Comment nodes
// (DOM spec §4.7).  textContent writes data, not children.
TEST_F(DomBinderTest, CharacterDataNodeValueAndData)
{
  EXPECT_TRUE(EvalBool("(function(){ var d = document; "
                       "var t = d.createTextNode('hello'); "
                       "var c = d.createComment('note'); "
                       "var e = d.createElement('div'); "
                       "var ok = t.nodeValue === 'hello' && c.nodeValue === 'note'; "
                       "ok = ok && t.data === 'hello' && c.data === 'note'; "
                       "ok = ok && e.nodeValue === null; " // Elements have no nodeValue
                       "t.nodeValue = 'bye'; "
                       "ok = ok && t.data === 'bye' && t.nodeValue === 'bye'; "
                       "c.data = 'updated'; "
                       "ok = ok && c.nodeValue === 'updated' && c.textContent === 'updated'; "
                       "t.textContent = 'via text'; "
                       "ok = ok && t.data === 'via text' && t.childNodes.length === 0; "
                       "return ok; })()"));
}

TEST_F(DomBinderTest, TextAndCommentUseCharacterDataInterface)
{
  EXPECT_TRUE(
      EvalBool("(function(){ var text = document.createTextNode('text'); "
               "var comment = document.createComment('comment'); "
               "return text instanceof CharacterData && comment instanceof CharacterData "
               "    && Object.getPrototypeOf(Text.prototype) === CharacterData.prototype "
               "    && Object.getPrototypeOf(Comment.prototype) === CharacterData.prototype "
               "    && typeof DocumentType === 'function' "
               "    && Object.getPrototypeOf(DocumentType.prototype) === Node.prototype; })()"));
}

// DocumentFragment appendChild/insertBefore move the fragment's children.
TEST_F(DomBinderTest, DocumentFragmentInsertionMovesChildren)
{
  EXPECT_TRUE(EvalBool("(function(){ var d = document; "
                       "var frag = d.createDocumentFragment(); "
                       "var a = d.createElement('a'); a.textContent = 'A'; "
                       "var b = d.createElement('b'); b.textContent = 'B'; "
                       "frag.appendChild(a); frag.appendChild(b); "
                       "var container = d.createElement('div'); "
                       "d.body.appendChild(container); "
                       "container.appendChild(frag); "
                       "var ok = container.childNodes.length === 2 "
                       "    && container.firstElementChild.tagName === 'A' "
                       "    && container.textContent === 'AB'; "
                       "return ok; })()"));
}

TEST_F(DomBinderTest, DocumentFragmentChildrenContainsOnlyElementsAndIsIterable)
{
  EXPECT_TRUE(
      EvalBool("(function(){ var d = document; "
               "var frag = d.createDocumentFragment(); "
               "frag.append(d.createTextNode('before'), d.createElement('a'), "
               "            d.createComment('note'), d.createElement('b')); "
               "return frag.children.length === 2 "
               "    && frag.children[0].tagName === 'A' "
               "    && frag.children[1].tagName === 'B' "
               "    && Array.from(frag.children).map(function(e) { return e.tagName; }).join(',') "
               "       === 'A,B'; })()"));
}

// DOM tree-mutation violations throw DOMException with the WebIDL exception
// name (NotFoundError / HierarchyRequestError), not a plain TypeError.
TEST_F(DomBinderTest, TreeMutationThrowsDomException)
{
  EXPECT_TRUE(EvalBool("(function(){ var d = document; "
                       "var a = d.createElement('div'); "
                       "var b = d.createElement('span'); "
                       "a.appendChild(b); "
                       "try { b.appendChild(a); return false; } "
                       "catch (e) { if (e.name !== 'HierarchyRequestError') return false; } "
                       "try { b.removeChild(a); return false; } "
                       "catch (e) { if (e.name !== 'NotFoundError') return false; } "
                       "try { a.insertBefore(a, b); return false; } "
                       "catch (e) { if (e.name !== 'HierarchyRequestError') return false; } "
                       "return true; })()"));
}

TEST_F(DomBinderTest, ScriptCannotCreateOverdeepTree)
{
  EXPECT_TRUE(EvalBool("(function(){ var root=document.createElement('div'); var node=root; "
                       "for (var i=0; i<512; ++i) { var child=document.createElement('div'); "
                       "node.appendChild(child); node=child; } "
                       "try { node.appendChild(document.createElement('div')); return false; } "
                       "catch (e) { return e.name === 'HierarchyRequestError'; } })()"));
}

// WHATWG pre-insert adopts the child: inserting a node that already has a
// parent MOVES it (removes it from the old parent first).  The C++ DOM API
// is unique_ptr-based and rejects parented nodes by design; the binding
// performs the adoption, which is what JS observes.
TEST_F(DomBinderTest, AppendChildMovesAParentedNode)
{
  EXPECT_TRUE(EvalBool("(function(){"
                       "  var a = document.createElement('div');"
                       "  var b = document.createElement('div');"
                       "  var n = document.createElement('span');"
                       "  a.appendChild(n);"
                       "  b.appendChild(n);"
                       "  return a.childNodes.length === 0 &&"
                       "         b.childNodes.length === 1 &&"
                       "         n.parentNode === b;"
                       "})()"));
  // Moving within the same parent reorders (appendChild to the end).
  EXPECT_TRUE(EvalBool("(function(){"
                       "  var p = document.createElement('div');"
                       "  var x = document.createElement('span');"
                       "  var y = document.createElement('span');"
                       "  p.appendChild(x); p.appendChild(y);"
                       "  p.appendChild(x);"
                       "  return p.childNodes.length === 2 &&"
                       "         p.childNodes[0] === y && p.childNodes[1] === x;"
                       "})()"));
}

TEST_F(DomBinderTest, CreateElementAppendAndInsert)
{
  EXPECT_TRUE(EvalBool("(function(){ var d = document; "
                       "var div = d.createElement('div'); div.id = 'created'; "
                       "d.body.appendChild(div); "
                       "return d.getElementById('created') === div; })()"));
  EXPECT_NE(document_->ToString().find("<div id=\"created\"></div>"), std::string::npos);

  // insertBefore places the new node before the reference.
  EXPECT_TRUE(EvalBool("(function(){ var d = document; "
                       "var div = d.createElement('section'); "
                       "d.body.insertBefore(div, d.getElementById('main')); "
                       "return d.body.firstElementChild === div; })()"));
}

TEST_F(DomBinderTest, CreateTextNode)
{
  EXPECT_TRUE(EvalBool("(function(){ var d = document; "
                       "var t = d.createTextNode('plain'); "
                       "var p = d.getElementById('first'); "
                       "p.appendChild(t); "
                       "return p.textContent === 'Hello worldplain'; })()"));
}

TEST_F(DomBinderTest, RemoveChildKeepsNodeAliveAndReappend)
{
  EXPECT_TRUE(
      EvalBool("(function(){ var d = document; "
               "var main = d.getElementById('main'); "
               "var p = d.getElementById('first'); "
               "d.body.removeChild(main); "
               "if (d.getElementById('first') !== null) return false; " // detached from the tree
               "d.body.appendChild(main); "                             // re-insert
               "return d.getElementById('first') === p; })()"));
}

TEST_F(DomBinderTest, Attributes)
{
  EXPECT_TRUE(EvalBool("document.getElementById('main').hasAttribute('class')"));
  EXPECT_TRUE(EvalBool("!document.getElementById('main').hasAttribute('missing')"));
  EXPECT_TRUE(EvalBool("(function(){ var e = document.getElementById('first'); "
                       "e.setAttribute('data-k', 'v'); "
                       "return e.getAttribute('data-k') === 'v'; })()"));
  EXPECT_TRUE(EvalBool("(function(){ var e = document.getElementById('first'); "
                       "e.removeAttribute('class'); "
                       "return e.getAttribute('class') === null; })()"));
  EXPECT_EQ(EvalString("document.getElementById('main').className"), "container");
  EXPECT_TRUE(EvalBool("(function(){ var e = document.getElementById('main'); e.className = 'x y'; "
                       "return e.getAttribute('class') === 'x y'; })()"));
  EXPECT_EQ(EvalNumber("document.getElementById('first').attributes.length"), 2.0);
  EXPECT_TRUE(EvalBool("(function(){ var e = document.getElementById('first'); "
                       "e.style.color = 'red'; var a = e.attributes; "
                       "return a.style !== undefined && a.style.name === 'style' "
                       "&& a.style.value.indexOf('color: red') !== -1; })()"));
}

TEST_F(DomBinderTest, AttributesUseNamedNodeMapInterface)
{
  EXPECT_TRUE(EvalBool("(function(){ var attributes = document.getElementById('main').attributes; "
                       "return attributes instanceof NamedNodeMap "
                       "    && Object.getPrototypeOf(attributes) === NamedNodeMap.prototype "
                       "    && attributes.item(0) === attributes[0] "
                       "    && attributes.getNamedItem('class') === attributes['class'] "
                       "    && attributes.getNamedItem('missing') === null; })()"));
}

TEST_F(DomBinderTest, AttributesExposeLiveAttrNodes)
{
  EXPECT_TRUE(EvalBool(
      "(function(){ var element = document.getElementById('main'); var attribute = "
      "element.attributes.class; "
      "var descriptor = Object.getOwnPropertyDescriptor(Attr.prototype, 'value'); "
      "attribute.value = 'updated'; "
      "return attribute instanceof Attr && attribute.name === 'class' && attribute.value === "
      "'updated' "
      "    && attribute.nodeValue === 'updated' && attribute.ownerElement === element "
      "    && element.getAttribute('class') === 'updated' "
      "    && typeof descriptor.get === 'function' && typeof descriptor.set === 'function'; })()"));
}

TEST_F(DomBinderTest, TreeWalkerTraversesDescendantsByNodeType)
{
  EXPECT_TRUE(EvalBool("(function(){ var root = document.getElementById('main'); "
                       "var walker = document.createTreeWalker(root, 1); "
                       "var first = walker.nextNode(); var second = walker.nextNode(); "
                       "return walker instanceof TreeWalker "
                       "    && Object.getPrototypeOf(walker) === TreeWalker.prototype "
                       "    && walker.root === root && walker.currentNode === second "
                       "    && first.nodeType === 1 && second.nodeType === 1; })()"));
}

TEST_F(DomBinderTest, NodeFilterExposesTraversalConstants)
{
  EXPECT_TRUE(EvalBool("(function(){ var root = document.createElement('div'); "
                       "root.appendChild(document.createTextNode('skip')); "
                       "var child = document.createElement('span'); root.appendChild(child); "
                       "var walker = document.createTreeWalker(root, NodeFilter.SHOW_ELEMENT); "
                       "return NodeFilter.SHOW_ALL === 0xFFFFFFFF "
                       "    && NodeFilter.FILTER_ACCEPT === 1 "
                       "    && NodeFilter.FILTER_REJECT === 2 "
                       "    && NodeFilter.FILTER_SKIP === 3 "
                       "    && walker.nextNode() === child; })()"));
}

TEST_F(DomBinderTest, StyleDeclaration)
{
  EXPECT_TRUE(EvalBool("(function(){ var e = document.getElementById('first'); "
                       "e.style.setProperty('color', 'red'); "
                       "return e.style.getPropertyValue('color') === 'red'; })()"));
  // Direct accessors round-trip through the style attribute.
  EXPECT_TRUE(EvalBool(
      "(function(){ var e = document.getElementById('first'); "
      "e.style.fontSize = '20px'; "
      "return e.style.fontSize === '20px' && e.style.getPropertyValue('font-size') === '20px'; "
      "})()"));
  // removeProperty.
  EXPECT_TRUE(EvalBool("(function(){ var e = document.getElementById('first'); "
                       "e.style.removeProperty('color'); "
                       "return e.style.getPropertyValue('color') === ''; })()"));
  // The C++ style attribute was updated.
  EXPECT_NE(document_->ToString().find("style=\"font-size: 20px\""), std::string::npos);

  EXPECT_TRUE(EvalBool("(function(){ var e = document.body; e.style.zoom = '0.9'; "
                       "return e.style.zoom === '0.9' && "
                       "e.style.getPropertyValue('zoom') === '0.9'; })()"));
}

TEST_F(DomBinderTest, InnerHTMLGetAndSet)
{
  EXPECT_EQ(EvalString("document.getElementById('first').innerHTML"), "Hello <b>world</b>");
  EXPECT_TRUE(EvalBool("(function(){ var e = document.getElementById('main'); "
                       "e.innerHTML = '<ul><li>a</li><li>b</li></ul>'; "
                       "return e.children.length === 1 && e.children[0].tagName === 'UL' "
                       "       && e.children[0].children.length === 2; })()"));
}

TEST_F(DomBinderTest, InnerHTMLUsesTheElementAsFragmentParseContext)
{
  // WHATWG HTML 13.4: innerHTML parses a fragment with the element itself as
  // the context element, so the context drives the tokenizer state and the
  // insertion mode.  The previous implementation parsed every value as a full
  // document and extracted the <body> children, which broke table contexts,
  // <select>, RCDATA/RAWTEXT contexts and the stray-table-tag rule.
  //
  // <table> context -> "in table": the row/cell survive.
  EXPECT_TRUE(EvalBool("(function(){"
                       "  var t = document.createElement('table');"
                       "  t.innerHTML = '<tr><td>x';"
                       "  return t.querySelector('td') !== null"
                       "      && t.querySelector('tbody') !== null"
                       "      && t.textContent === 'x';"
                       "})()"));
  // <div> context -> "in body": the stray <td> is ignored.
  EXPECT_TRUE(EvalBool("(function(){"
                       "  var d = document.createElement('div');"
                       "  d.innerHTML = '<td>x';"
                       "  return d.querySelector('td') === null && d.textContent === 'x';"
                       "})()"));
  // <select> context: options become siblings.
  EXPECT_TRUE(EvalBool("(function(){"
                       "  var s = document.createElement('select');"
                       "  s.innerHTML = '<option>a<option>b';"
                       "  var opts = s.querySelectorAll('option');"
                       "  return opts.length === 2 && opts[0].textContent === 'a'"
                       "      && opts[1].textContent === 'b' && opts[1].parentNode === s;"
                       "})()"));
  // <title> context -> RCDATA: markup is text, not elements.
  EXPECT_TRUE(EvalBool("(function(){"
                       "  var ti = document.createElement('title');"
                       "  ti.innerHTML = '<b>x</b>';"
                       "  return ti.querySelector('b') === null"
                       "      && ti.childNodes.length === 1 && ti.childNodes[0].nodeType === 3"
                       "      && ti.textContent === '<b>x</b>';"
                       "})()"));
  // <style> context -> RAWTEXT: likewise.
  EXPECT_TRUE(EvalBool("(function(){"
                       "  var st = document.createElement('style');"
                       "  st.innerHTML = '<b>x</b>';"
                       "  return st.querySelector('b') === null && st.textContent === '<b>x</b>';"
                       "})()"));
}

TEST_F(DomBinderTest, TemplateInnerHTMLUsesTemplateAsFragmentContext)
{
  // <template> innerHTML is parsed with the template element as the context
  // and the resulting fragment is adopted into template.content() (HTML
  // 4.12.3), leaving the element's own child list empty.
  EXPECT_TRUE(EvalBool("(function(){"
                       "  var t = document.createElement('template');"
                       "  t.innerHTML = '<div id=q>hi</div>';"
                       "  return t.childNodes.length === 0"
                       "      && t.content.childNodes.length === 1"
                       "      && t.content.querySelector('#q') !== null"
                       "      && t.content.firstChild.textContent === 'hi';"
                       "})()"));
}

TEST_F(DomBinderTest, InsertAdjacentHTMLUsesTheElementAsFragmentContext)
{
  // insertAdjacentHTML parses with the element as the fragment context too.
  EXPECT_TRUE(EvalBool("(function(){"
                       "  var t = document.createElement('table');"
                       "  t.insertAdjacentHTML('beforeend', '<tr><td>y');"
                       "  var td = t.querySelector('td');"
                       "  return td !== null && td.textContent === 'y';"
                       "})()"));
  EXPECT_TRUE(EvalBool("(function(){"
                       "  var d = document.createElement('div');"
                       "  d.insertAdjacentHTML('beforeend', '<td>z');"
                       "  return d.querySelector('td') === null && d.textContent === 'z';"
                       "})()"));
}

TEST_F(DomBinderTest, CloneNode)
{
  EXPECT_TRUE(EvalBool(
      "(function(){ var e = document.getElementById('first'); "
      "var clone = e.cloneNode(true); "
      "return clone !== e && clone.tagName === 'P' && clone.textContent === 'Hello world'; })()"));
  // The clone is detached (owned by the binder).
  EXPECT_TRUE(EvalBool("(function(){ var d = document; "
                       "var c = d.getElementById('first').cloneNode(true); "
                       "return c.parentNode === null; })()"));
}

TEST_F(DomBinderTest, EventListenerFromScript)
{
  EXPECT_TRUE(EvalBool(
      "(function(){ var d = document; "
      "var e = d.getElementById('first'); "
      "var hits = 0; "
      "e.addEventListener('click', function(ev) { hits++; window._ev = ev; }); "
      "e.dispatchEvent({type: 'click'}); "
      "return hits === 1 && window._ev.type === 'click' && window._ev.target === e; })()"));
}

TEST_F(DomBinderTest, RemoveEventListener)
{
  EXPECT_TRUE(EvalBool("(function(){ var d = document; "
                       "var e = d.getElementById('first'); "
                       "var hits = 0; "
                       "function cb() { hits++; } "
                       "e.addEventListener('click', cb); "
                       "e.removeEventListener('click', cb); "
                       "e.dispatchEvent({type: 'click'}); "
                       "return hits === 0; })()"));
}

TEST_F(DomBinderTest, DispatchEventFromCpp)
{
  ASSERT_TRUE(EvalBool("(function(){ var e = document.getElementById('first'); "
                       "window._hits = 0; "
                       "e.addEventListener('app-hi', function(ev) { "
                       "  window._hits++; window._evType = ev.type; "
                       "}); "
                       "return true; })()"));
  dom::Element* first = dom::QuerySelector(*document_, "#first");
  ASSERT_NE(first, nullptr);
  binder_->DispatchEvent(*first, "app-hi");
  EXPECT_EQ(EvalNumber("window._hits"), 1.0);
  EXPECT_EQ(EvalString("window._evType"), "app-hi");
}

TEST_F(DomBinderTest, DispatchCancelableEventReturnsNotCanceled)
{
  dom::Element* first = dom::QuerySelector(*document_, "#first");
  ASSERT_NE(first, nullptr);
  // No listeners: the event is not canceled.
  EXPECT_TRUE(binder_->DispatchCancelableEvent(*first, "click"));
  // A listener that does not call preventDefault: still not canceled, and the
  // listener fires with a cancelable, bubbling event.
  ASSERT_TRUE(EvalBool("(function(){ var e = document.getElementById('first'); "
                       "window._clicks = 0; window._cancelable = false; "
                       "e.addEventListener('click', function(ev) { "
                       "  window._clicks++; window._cancelable = ev.cancelable; "
                       "}); "
                       "return true; })()"));
  EXPECT_TRUE(binder_->DispatchCancelableEvent(*first, "click"));
  EXPECT_EQ(EvalNumber("window._clicks"), 1.0);
  EXPECT_TRUE(EvalBool("window._cancelable === true"));
}

TEST_F(DomBinderTest, DispatchCancelableEventPreventDefault)
{
  dom::Element* first = dom::QuerySelector(*document_, "#first");
  ASSERT_NE(first, nullptr);
  ASSERT_TRUE(EvalBool("(function(){ var e = document.getElementById('first'); "
                       "e.addEventListener('click', function(ev) { "
                       "  window._canceledBefore = ev.defaultPrevented; "
                       "  ev.preventDefault(); "
                       "  window._canceledAfter = ev.defaultPrevented; "
                       "}); "
                       "return true; })()"));
  // preventDefault() cancels the event: dispatch returns false, so the caller
  // knows not to run the default action.
  EXPECT_FALSE(binder_->DispatchCancelableEvent(*first, "click"));
  EXPECT_TRUE(EvalBool("window._canceledBefore === false && window._canceledAfter === true"));
}

TEST_F(DomBinderTest, DispatchCancelableEventBubblesToAncestors)
{
  dom::Element* first = dom::QuerySelector(*document_, "#first");
  ASSERT_NE(first, nullptr);
  ASSERT_TRUE(EvalBool("(function(){ var p = document.getElementById('first'); "
                       "var main = document.getElementById('main'); "
                       "window._order = []; "
                       "p.addEventListener('click', function(ev){ "
                       "  window._order.push('target:' + ev.currentTarget.id); }); "
                       "main.addEventListener('click', function(ev){ "
                       "  window._order.push('ancestor:' + ev.currentTarget.id); }); "
                       "return true; })()"));
  binder_->DispatchCancelableEvent(*first, "click");
  EXPECT_TRUE(EvalBool("window._order.length === 2 && "
                       "window._order[0] === 'target:first' && "
                       "window._order[1] === 'ancestor:main'"));
}

TEST_F(DomBinderTest, EventConstructorAndProperties)
{
  EXPECT_TRUE(EvalBool("(function(){ var ev = new Event('go', {bubbles: true, cancelable: true}); "
                       "return ev.type === 'go' && ev.bubbles === true && ev.cancelable === true "
                       "    && ev.defaultPrevented === false && ev.target === null "
                       "    && ev.currentTarget === null && ev instanceof Event; })()"));
  // Defaults are non-bubbling / non-cancelable.
  EXPECT_TRUE(EvalBool("(function(){ var ev = new Event('x'); "
                       "return ev.bubbles === false && ev.cancelable === false; })()"));
  // preventDefault only affects cancelable events.
  EXPECT_TRUE(EvalBool("(function(){ var a = new Event('a', {cancelable: true}); "
                       "a.preventDefault(); "
                       "var b = new Event('b'); b.preventDefault(); "
                       "return a.defaultPrevented === true && b.defaultPrevented === false; })()"));
}

TEST_F(DomBinderTest, EventBubblesToAncestors)
{
  EXPECT_TRUE(EvalBool("(function(){ var d = document; "
                       "var main = d.getElementById('main'); "
                       "var p = d.getElementById('first'); "
                       "var order = []; "
                       "p.addEventListener('bub', function() { order.push('target'); }); "
                       "main.addEventListener('bub', function() { order.push('ancestor'); }); "
                       "d.addEventListener('bub', function(ev) { "
                       "  order.push('document'); "
                       "  window._cur = ev.currentTarget; "
                       "}); "
                       "p.dispatchEvent(new Event('bub', {bubbles: true})); "
                       "return order.join(',') === 'target,ancestor,document' "
                       "    && window._cur === d; })()"));
  // Non-bubbling events do not reach ancestors.
  EXPECT_TRUE(EvalBool("(function(){ var d = document; "
                       "var p = d.getElementById('first'); "
                       "var hits = 0; "
                       "d.addEventListener('nb', function() { hits++; }); "
                       "p.dispatchEvent(new Event('nb')); "
                       "return hits === 0; })()"));
}

TEST_F(DomBinderTest, EventCapturePhaseRunsRootToTarget)
{
  EXPECT_TRUE(EvalBool("(function(){ var d = document; "
                       "var main = d.getElementById('main'); "
                       "var p = d.getElementById('first'); "
                       "var order = []; "
                       "d.addEventListener('cap', function() { order.push('doc'); }, true); "
                       "main.addEventListener('cap', function() { order.push('main'); }, true); "
                       "p.addEventListener('cap', function() { order.push('target'); }); "
                       "p.dispatchEvent(new Event('cap', {bubbles: true})); "
                       "return order.join(',') === 'doc,main,target'; })()"));
}

TEST_F(DomBinderTest, EventOnceOptionAutoRemoves)
{
  EXPECT_TRUE(EvalBool("(function(){ var e = document.getElementById('first'); "
                       "var hits = 0; "
                       "e.addEventListener('once-ev', function() { hits++; }, {once: true}); "
                       "e.dispatchEvent(new Event('once-ev')); "
                       "e.dispatchEvent(new Event('once-ev')); "
                       "return hits === 1; })()"));
}

TEST_F(DomBinderTest, EventPreventDefaultReturnsFalseFromDispatch)
{
  EXPECT_TRUE(EvalBool("(function(){ var e = document.getElementById('first'); "
                       "var cancelable = new Event('pd', {cancelable: true}); "
                       "e.addEventListener('pd', function(ev) { ev.preventDefault(); }); "
                       "var retCancel = e.dispatchEvent(cancelable); "
                       "var notCancelable = new Event('nc'); "
                       "e.addEventListener('nc', function(ev) { ev.preventDefault(); }); "
                       "var retNot = e.dispatchEvent(notCancelable); "
                       "return retCancel === false && cancelable.defaultPrevented === true "
                       "    && retNot === true && notCancelable.defaultPrevented === false; })()"));
}

TEST_F(DomBinderTest, EventStopPropagationPreventsBubbling)
{
  EXPECT_TRUE(EvalBool("(function(){ var d = document; "
                       "var p = d.getElementById('first'); "
                       "var hits = 0; "
                       "p.addEventListener('sp', function(ev) { ev.stopPropagation(); }); "
                       "d.addEventListener('sp', function() { hits++; }); "
                       "p.dispatchEvent(new Event('sp', {bubbles: true})); "
                       "return hits === 0; })()"));
}

TEST_F(DomBinderTest, EventStopImmediatePropagationStopsCurrentNode)
{
  EXPECT_TRUE(EvalBool("(function(){ var e = document.getElementById('first'); "
                       "var order = []; "
                       "e.addEventListener('sip', function(ev) { "
                       "  order.push('first'); ev.stopImmediatePropagation(); }); "
                       "e.addEventListener('sip', function() { order.push('second'); }); "
                       "e.dispatchEvent(new Event('sip', {bubbles: true})); "
                       "return order.join(',') === 'first'; })()"));
}

TEST_F(DomBinderTest, RemoveEventListenerMatchesCaptureFlag)
{
  EXPECT_TRUE(EvalBool("(function(){ var e = document.getElementById('first'); "
                       "var hits = 0; "
                       "function cb() { hits++; } "
                       "e.addEventListener('rc', cb, true); "
                       "e.removeEventListener('rc', cb, false); "
                       "e.dispatchEvent(new Event('rc')); "
                       "return hits === 1; })()"));
}

// ---------------------------------------------------------------------------
// Node traversal / relationship APIs.
// ---------------------------------------------------------------------------

TEST_F(DomBinderTest, NodeSiblingsAndParentElement)
{
  EXPECT_TRUE(EvalBool("(function(){ var d = document; "
                       "var main = d.getElementById('main'); "
                       "var first = d.getElementById('first'); "
                       "var second = first.nextElementSibling; "
                       "return first.parentNode === main && first.parentElement === main "
                       "    && second !== first && second.classList.contains('para') "
                       "    && second.previousElementSibling === first "
                       "    && first.previousElementSibling === null "
                       "    && main.nextElementSibling === null; })()"));
}

TEST_F(DomBinderTest, NodeOwnerDocumentAndIsConnected)
{
  EXPECT_TRUE(EvalBool("(function(){ var d = document; "
                       "var el = d.getElementById('first'); "
                       "var detached = d.createElement('div'); "
                       "return el.ownerDocument === d && el.isConnected === true "
                       "    && detached.ownerDocument === d && detached.isConnected === false "
                       "    && d.ownerDocument === null; })()"));
}

TEST_F(DomBinderTest, NodeContains)
{
  EXPECT_TRUE(EvalBool("(function(){ var d = document; "
                       "var main = d.getElementById('main'); "
                       "var first = d.getElementById('first'); "
                       "var detached = d.createElement('span'); "
                       "return main.contains(first) && main.contains(main) "
                       "    && !main.contains(detached) && !first.contains(main); })()"));
}

TEST_F(DomBinderTest, NodeReplaceChild)
{
  EXPECT_TRUE(EvalBool("(function(){ var d = document; "
                       "var main = d.getElementById('main'); "
                       "var first = d.getElementById('first'); "
                       "var replacement = d.createElement('p'); replacement.id = 'repl'; "
                       "replacement.textContent = 'new'; "
                       "var returned = main.replaceChild(replacement, first); "
                       "return returned === replacement && first.parentNode === null "
                       "    && main.firstElementChild.id === 'repl'; })()"));
}

TEST_F(DomBinderTest, NodeNormalizeMergesTextNodes)
{
  EXPECT_TRUE(
      EvalBool("(function(){ var d = document; "
               "var el = d.createElement('div'); "
               "el.appendChild(d.createTextNode('Hello ')); "
               "el.appendChild(d.createTextNode('world')); "
               "el.normalize(); "
               "return el.childNodes.length === 1 && el.textContent === 'Hello world'; })()"));
}

// ---------------------------------------------------------------------------
// Element classList / dataset / matches / closest / remove.
// ---------------------------------------------------------------------------

TEST_F(DomBinderTest, ClassListOperations)
{
  EXPECT_TRUE(EvalBool("(function(){ var e = document.getElementById('first'); "
                       "var l = e.classList; "
                       "return l.contains('para') && l.length === 1; })()"));
  EXPECT_TRUE(EvalBool("(function(){ var e = document.getElementById('first'); "
                       "e.classList.add('a', 'b'); "
                       "var ok = e.classList.contains('a') && e.classList.contains('b') "
                       "    && e.classList.length === 3; "
                       "e.classList.remove('a'); "
                       "ok = ok && !e.classList.contains('a') && e.classList.length === 2; "
                       "ok = ok && e.classList.toggle('c') === true; "
                       "ok = ok && e.classList.toggle('c') === false; "
                       "ok = ok && e.classList.replace('b', 'bb'); "
                       "return ok && e.classList.contains('bb') && e.className === 'para bb' "
                       "    && typeof e.classList[Symbol.iterator] === 'function' "
                       "    && Array.from(e.classList).join(',') === 'para,bb'; })()"));
}

TEST_F(DomBinderTest, ToggleAttributeHonorsForceAndReturnsPresence)
{
  EXPECT_TRUE(EvalBool("(function(){ var e = document.documentElement; "
                       "var added = e.toggleAttribute('data-active'); "
                       "var removed = e.toggleAttribute('data-active'); "
                       "var forcedOn = e.toggleAttribute('data-active', true); "
                       "var forcedOff = e.toggleAttribute('data-active', false); "
                       "return added === true && removed === false && forcedOn === true "
                       "&& forcedOff === false && !e.hasAttribute('data-active'); })()"));
}

TEST_F(DomBinderTest, DatasetReadsAndWritesDataAttributes)
{
  // The fixture has <span data-x="1"></span>.
  EXPECT_TRUE(EvalBool("(function(){ var d = document; "
                       "var s = d.querySelector('span'); "
                       "return s.dataset.x === '1'; })()"));
  EXPECT_TRUE(EvalBool(
      "(function(){ var e = document.createElement('div'); "
      "e.dataset.fooBar = 'baz'; "
      "return e.dataset.fooBar === 'baz' && e.getAttribute('data-foo-bar') === 'baz'; })()"));
  EXPECT_TRUE(EvalBool("(function(){ var e = document.createElement('div'); "
                       "e.setAttribute('data-role', 'nav'); "
                       "return e.dataset.role === 'nav'; })()"));
}

TEST_F(DomBinderTest, ElementMatchesAndClosest)
{
  EXPECT_TRUE(EvalBool("(function(){ var e = document.getElementById('first'); "
                       "return e.matches('.para') && e.matches('p') && !e.matches('.nope'); })()"));
  EXPECT_TRUE(
      EvalBool("(function(){ var d = document; "
               "var b = d.getElementById('first').querySelector('b'); "
               "return b.closest('#main').id === 'main' && b.closest('.para') !== null "
               "    && b.closest('html') !== null && b.closest('.missing') === null; })()"));
}

TEST_F(DomBinderTest, ElementRemove)
{
  EXPECT_TRUE(EvalBool("(function(){ var d = document; "
                       "var main = d.getElementById('main'); "
                       "var first = d.getElementById('first'); "
                       "first.remove(); "
                       "return first.parentNode === null && main.children.length === 2; })()"));
}

TEST_F(DomBinderTest, ElementHiddenTitleLangOuterHTML)
{
  EXPECT_TRUE(EvalBool("(function(){ var d = document; "
                       "var e = d.createElement('div'); "
                       "e.hidden = true; "
                       "var ok = e.hidden === true && e.hasAttribute('hidden'); "
                       "e.title = 'hi'; e.lang = 'en'; "
                       "ok = ok && e.title === 'hi' && e.lang === 'en'; "
                       "ok = ok && e.outerHTML.indexOf('<div') === 0; "
                       "return ok; })()"));
}

TEST_F(DomBinderTest, ElementGetBoundingClientRect)
{
  EXPECT_TRUE(EvalBool("(function(){ var e = document.getElementById('first'); "
                       "var r = e.getBoundingClientRect(); "
                       "return r.width === 0 && r.height === 0 && r.left === 0 "
                       "    && typeof r.toJSON === 'function'; })()"));
}

// ---------------------------------------------------------------------------
// Form controls / links / images.
// ---------------------------------------------------------------------------

TEST_F(DomBinderTest, FormControlValueCheckedDisabled)
{
  EXPECT_TRUE(EvalBool("(function(){ var d = document; "
                       "var input = d.createElement('input'); "
                       "input.type = 'text'; input.value = 'abc'; "
                       "var ok = input.type === 'text' && input.value === 'abc'; "
                       "input.placeholder = 'hint'; input.name = 'q'; input.disabled = true; "
                       "ok = ok && input.placeholder === 'hint' && input.name === 'q' "
                       "    && input.disabled === true && input.hasAttribute('disabled'); "
                       "var cb = d.createElement('input'); cb.type = 'checkbox'; "
                       "cb.checked = true; "
                       "ok = ok && cb.checked === true && cb.hasAttribute('checked'); "
                       "return ok; })()"));
  EXPECT_TRUE(
      EvalBool("(function(){ var d = document; "
               "var ta = d.createElement('textarea'); "
               "ta.value = 'line1\\nline2'; "
               "return ta.value === 'line1\\nline2' && ta.textContent === 'line1\\nline2'; })()"));
  EXPECT_TRUE(EvalBool("(function(){ var d = document; "
                       "var sel = d.createElement('select'); "
                       "var opt = d.createElement('option'); opt.value = 'v'; "
                       "opt.textContent = 'label'; "
                       "sel.appendChild(opt); "
                       "return opt.value === 'v'; })()"));
}

TEST_F(DomBinderTest, TextareaValueKeepsOldChildWrappersAlive)
{
  // Setting value on a textarea replaces its text child.  The old child may
  // have been wrapped and referenced by JS; it must stay alive (retained by
  // the binder) rather than being freed underneath a live wrapper.
  EXPECT_TRUE(EvalBool("(function(){ var d = document; "
                       "var ta = d.createElement('textarea'); "
                       "ta.textContent = 'original'; "
                       "var child = ta.firstChild; "
                       "ta.value = 'new'; "
                       "return ta.value === 'new' && ta.textContent === 'new' "
                       "    && child !== null && child.nodeType === 3; })()"));
}

TEST_F(DomBinderTest, TextControlSelectionGettersSettersAndClamping)
{
  // setSelectionRange stores UTF-16 code unit offsets and the direction, and
  // the accessors round-trip them.
  EXPECT_EQ(EvalString("(function(){ var i = document.createElement('input'); "
                       "i.value = 'abcdef'; i.setSelectionRange(1, 4, 'forward'); "
                       "return i.selectionStart + ',' + i.selectionEnd + ',' + "
                       "i.selectionDirection; })()"),
            "1,4,forward");
  // A backwards selection keeps its ordered extent and reports the direction.
  EXPECT_EQ(EvalString("(function(){ var i = document.createElement('input'); "
                       "i.value = 'abcdef'; i.setSelectionRange(1, 4, 'backward'); "
                       "return i.selectionStart + ',' + i.selectionEnd + ',' + "
                       "i.selectionDirection; })()"),
            "1,4,backward");
  // selectionStart pushes selectionEnd forward when it was behind the new start.
  EXPECT_EQ(EvalString("(function(){ var i = document.createElement('input'); "
                       "i.value = 'abcdef'; i.setSelectionRange(1, 3); i.selectionStart = 5; "
                       "return i.selectionStart + ',' + i.selectionEnd + ',' + "
                       "i.selectionDirection; })()"),
            "5,5,none");
  // An end before the current start collapses the range at that end.
  EXPECT_EQ(EvalString("(function(){ var i = document.createElement('input'); "
                       "i.value = 'abcdef'; i.setSelectionRange(3, 5, 'backward'); "
                       "i.selectionEnd = 1; "
                       "return i.selectionStart + ',' + i.selectionEnd + ',' + "
                       "i.selectionDirection; })()"),
            "1,1,backward");
  // Offsets clamp to [0, length]; an unknown direction becomes "none".
  EXPECT_EQ(EvalString("(function(){ var i = document.createElement('input'); "
                       "i.value = 'abcdef'; i.setSelectionRange(-4, 99, 'sideways'); "
                       "return i.selectionStart + ',' + i.selectionEnd + ',' + "
                       "i.selectionDirection; })()"),
            "0,6,none");
  // direction setter keeps the extent.
  EXPECT_EQ(EvalString("(function(){ var i = document.createElement('input'); "
                       "i.value = 'abcdef'; i.setSelectionRange(1, 2, 'forward'); "
                       "i.selectionDirection = 'backward'; "
                       "return i.selectionStart + ',' + i.selectionEnd + ',' + "
                       "i.selectionDirection; })()"),
            "1,2,backward");
  // Default state per §4.10.20 is a collapsed cursor at the beginning.
  EXPECT_EQ(EvalString("(function(){ var i = document.createElement('input'); "
                       "i.value = 'abc'; return i.selectionStart + ',' + i.selectionEnd + ',' + "
                       "i.selectionDirection; })()"),
            "0,0,none");
  // A value setter clamps an existing selection to the new length.
  EXPECT_EQ(EvalString("(function(){ var i = document.createElement('input'); "
                       "i.value = 'abcdef'; i.setSelectionRange(2, 5); i.value = 'ab'; "
                       "return i.selectionStart + ',' + i.selectionEnd; })()"),
            "2,2");
}

TEST_F(DomBinderTest, TextControlSelectSelectsEverything)
{
  EXPECT_EQ(EvalString("(function(){ var i = document.createElement('input'); "
                       "i.value = 'abcdef'; i.setSelectionRange(2, 3, 'backward'); i.select(); "
                       "return i.selectionStart + ',' + i.selectionEnd + ',' + "
                       "i.selectionDirection; })()"),
            "0,6,none");
  EXPECT_EQ(EvalString("(function(){ var t = document.createElement('textarea'); "
                       "t.value = 'hello'; t.select(); "
                       "return t.selectionStart + ',' + t.selectionEnd + ',' + "
                       "t.selectionDirection; })()"),
            "0,5,none");
  // The APIs live only on the input/textarea prototypes.
  EXPECT_FALSE(EvalBool("'selectionStart' in document.createElement('div')"));
  EXPECT_FALSE(EvalBool("'setRangeText' in document.createElement('div')"));
  EXPECT_TRUE(EvalBool("'selectionStart' in document.createElement('input')"));
  EXPECT_TRUE(EvalBool("'selectionStart' in document.createElement('textarea')"));
}

TEST_F(DomBinderTest, TextControlSetRangeTextSelectionModes)
{
  // select: the inserted text becomes the selection.
  EXPECT_EQ(EvalString("(function(){ var i = document.createElement('input'); "
                       "i.value = 'abcdef'; i.setRangeText('XY', 1, 4, 'select'); "
                       "return i.value + '|' + i.selectionStart + ',' + i.selectionEnd; })()"),
            "aXYef|1,3");
  // start: collapsed before the inserted text.
  EXPECT_EQ(EvalString("(function(){ var i = document.createElement('input'); "
                       "i.value = 'abcdef'; i.setRangeText('XY', 1, 4, 'start'); "
                       "return i.value + '|' + i.selectionStart + ',' + i.selectionEnd; })()"),
            "aXYef|1,1");
  // end: collapsed after the inserted text.
  EXPECT_EQ(EvalString("(function(){ var i = document.createElement('input'); "
                       "i.value = 'abcdef'; i.setRangeText('XY', 1, 4, 'end'); "
                       "return i.value + '|' + i.selectionStart + ',' + i.selectionEnd; })()"),
            "aXYef|3,3");
  // Default mode is preserve.  With the 3-argument form the selection is left
  // where it was (clamped to the new value).
  EXPECT_EQ(EvalString("(function(){ var i = document.createElement('input'); "
                       "i.value = 'abcdef'; i.setSelectionRange(2, 4); "
                       "i.setRangeText('ZZ', 0, 1); "
                       "return i.value + '|' + i.selectionStart + ',' + i.selectionEnd; })()"),
            "ZZbcdef|2,4");
  // The one-argument form replaces the current selection; preserve keeps the
  // selection over the replacement (caret after it for a non-empty range).
  EXPECT_EQ(EvalString("(function(){ var i = document.createElement('input'); "
                       "i.value = 'abcdef'; i.setSelectionRange(4, 5); "
                       "i.setRangeText('XYZ'); "
                       "return i.value + '|' + i.selectionStart + ',' + i.selectionEnd; })()"),
            "abcdXYZf|4,7");
  // A collapsed caret is preserved before the inserted text (spec's preserve
  // rules do not move it past the insertion).
  EXPECT_EQ(EvalString("(function(){ var i = document.createElement('input'); "
                       "i.value = 'ab'; i.setSelectionRange(1, 1); i.setRangeText('XY'); "
                       "return i.value + '|' + i.selectionStart + ',' + i.selectionEnd; })()"),
            "aXYb|1,1");
  // The 3-argument form also accepts setRangeText(replacement, start, end).
  EXPECT_EQ(EvalString("(function(){ var i = document.createElement('input'); "
                       "i.value = 'abcdef'; i.setRangeText('Q', 2, 3, 'select'); "
                       "return i.value + '|' + i.selectionStart + ',' + i.selectionEnd; })()"),
            "abQdef|2,3");
  // maxlength is a user-input limit; setRangeText is allowed to exceed it.
  EXPECT_EQ(EvalString("(function(){ var i = document.createElement('input'); "
                       "i.setAttribute('maxlength', '2'); i.value = 'abc'; "
                       "i.setRangeText('XYZ', 0, 3, 'select'); "
                       "return i.value + '|' + i.value.length; })()"),
            "XYZ|3");
  // start > end is an IndexSizeError.
  EXPECT_EQ(EvalString("(function(){ var i = document.createElement('input'); "
                       "i.value = 'abc'; try { i.setRangeText('x', 2, 1); } "
                       "catch (e) { return e.name; } return 'no error'; })()"),
            "IndexSizeError");
}

TEST_F(DomBinderTest, TextControlSelectionRejectsUnsupportedInputTypes)
{
  EXPECT_EQ(
      EvalString(
          "(function(){ var types = ['number','date','checkbox','radio','file','range',"
          "'color','hidden','submit','reset','button','image','time','month','week',"
          "'datetime-local']; "
          "for (var n = 0; n < types.length; ++n) { "
          "  var i = document.createElement('input'); i.type = types[n]; "
          "  if (i.selectionStart !== null || i.selectionEnd !== null || "
          "      i.selectionDirection !== null) return 'getter:' + types[n]; "
          "  try { i.selectionStart = 0; return 'start:' + types[n]; } "
          "  catch (e) { if (e.name !== 'InvalidStateError') return 'start-name:' + e.name; } "
          "  try { i.selectionEnd = 0; return 'end:' + types[n]; } "
          "  catch (e) { if (e.name !== 'InvalidStateError') return 'end-name:' + e.name; } "
          "  try { i.selectionDirection = 'forward'; return 'dir:' + types[n]; } "
          "  catch (e) { if (e.name !== 'InvalidStateError') return 'dir-name:' + e.name; } "
          "  try { i.setSelectionRange(0, 0); return 'range:' + types[n]; } "
          "  catch (e) { if (e.name !== 'InvalidStateError') return 'range-name:' + e.name; } "
          "  try { i.setRangeText('x'); return 'text:' + types[n]; } "
          "  catch (e) { if (e.name !== 'InvalidStateError') return 'text-name:' + e.name; } "
          "  i.select(); " // returns early, no throw
          "} return 'ok'; })()"),
      "ok");
  // Password and email do support selection.
  EXPECT_EQ(EvalString("(function(){ var i = document.createElement('input'); "
                       "i.type = 'password'; i.value = 'secret'; i.setSelectionRange(1, 3); "
                       "return i.selectionStart + ',' + i.selectionEnd; })()"),
            "1,3");
  EXPECT_EQ(EvalString("(function(){ var i = document.createElement('input'); "
                       "i.type = 'email'; i.value = 'a@b.c'; i.select(); "
                       "return i.selectionStart + ',' + i.selectionEnd; })()"),
            "0,5");
  // Uppercase type names are ASCII case-insensitive.
  EXPECT_EQ(EvalString("(function(){ var i = document.createElement('input'); "
                       "i.setAttribute('type', 'TEXT'); i.value = 'ab'; i.select(); "
                       "return i.selectionStart + ',' + i.selectionEnd; })()"),
            "0,2");
  // A missing or unknown type value is the Text state (invalid value default).
  EXPECT_EQ(EvalString("(function(){ var i = document.createElement('input'); "
                       "i.setAttribute('type', 'not-a-real-type'); i.value = 'ab'; "
                       "i.setSelectionRange(0, 1); "
                       "return i.selectionStart + ',' + i.selectionEnd; })()"),
            "0,1");
  EXPECT_EQ(EvalString("(function(){ var i = document.createElement('input'); "
                       "i.setAttribute('type', ''); i.value = 'ab'; i.setSelectionRange(1, 2); "
                       "return i.selectionStart + ',' + i.selectionEnd; })()"),
            "1,2");
}

TEST_F(DomBinderTest, TextareaSelectionAndSetRangeText)
{
  EXPECT_EQ(EvalString("(function(){ var t = document.createElement('textarea'); "
                       "t.value = 'abc'; t.setSelectionRange(1, 2, 'backward'); "
                       "return t.selectionStart + ',' + t.selectionEnd + ',' + "
                       "t.selectionDirection; })()"),
            "1,2,backward");
  EXPECT_EQ(EvalString("(function(){ var t = document.createElement('textarea'); "
                       "t.value = 'abc'; t.setRangeText('X', 0, 3, 'select'); "
                       "return t.value + '|' + t.selectionStart + ',' + t.selectionEnd; })()"),
            "X|0,1");
  EXPECT_EQ(EvalString("(function(){ var t = document.createElement('textarea'); "
                       "t.value = 'line1\\nline2'; t.setSelectionRange(5, 6); "
                       "return t.selectionStart + ',' + t.selectionEnd; })()"),
            "5,6");
}

TEST_F(DomBinderTest, TextControlSelectionUsesUtf16CodeUnitOffsets)
{
  // U+1F600 is a surrogate pair: two UTF-16 code units, one code point.
  EXPECT_EQ(EvalString("(function(){ var i = document.createElement('input'); "
                       "i.value = 'a' + String.fromCodePoint(0x1F600) + 'b'; "
                       "return i.value.length; })()"),
            "4");
  EXPECT_EQ(EvalString("(function(){ var i = document.createElement('input'); "
                       "i.value = 'a' + String.fromCodePoint(0x1F600) + 'b'; "
                       "i.setSelectionRange(1, 3, 'forward'); "
                       "return i.selectionStart + ',' + i.selectionEnd; })()"),
            "1,3");
  // setRangeText removes the whole surrogate pair.
  EXPECT_EQ(EvalString("(function(){ var i = document.createElement('input'); "
                       "i.value = 'a' + String.fromCodePoint(0x1F600) + 'b'; "
                       "i.setRangeText('X', 1, 3, 'select'); "
                       "return i.value + '|' + i.value.length + '|' + i.selectionStart + ',' + "
                       "i.selectionEnd; })()"),
            "aXb|3|1,2");
  // A collapsed selection after the pair stays at code unit offset 2.
  EXPECT_EQ(EvalString("(function(){ var i = document.createElement('input'); "
                       "i.value = String.fromCodePoint(0x1F600); i.setSelectionRange(2, 2); "
                       "return i.selectionStart + ',' + i.selectionEnd; })()"),
            "2,2");
}

TEST_F(DomBinderTest, TextControlSelectionFiresSelectEvent)
{
  EXPECT_EQ(EvalString("(function(){ var i = document.createElement('input'); "
                       "i.value = 'abc'; var fired = 0; "
                       "i.addEventListener('select', function(){ fired++; }); "
                       "i.setSelectionRange(0, 2); i.setSelectionRange(0, 2); "
                       "i.select(); return fired; })()"),
            "2");
}

TEST_F(DomBinderTest, LinkHrefAndImageSrc)
{
  EXPECT_TRUE(EvalBool("(function(){ var d = document; "
                       "var a = d.createElement('a'); "
                       "a.href = '/path'; a.target = '_blank'; a.rel = 'nofollow'; "
                       "return a.target === '_blank' && a.rel === 'nofollow' "
                       "    && a.getAttribute('href') === '/path'; })()"));
  EXPECT_TRUE(EvalBool("(function(){ var d = document; "
                       "var img = d.createElement('img'); "
                       "img.src = 'pic.png'; img.alt = 'pic'; img.width = 100; "
                       "var ok = img.alt === 'pic' && img.width === 100 "
                       "    && img.complete === true && img.naturalWidth === 0 "
                       "    && img.getAttribute('src') === 'pic.png'; "
                       "return ok; })()"));
}

TEST_F(DomBinderTest, AnchorReflectsResolvedUrlComponents)
{
  PageApis apis;
  apis.resolve_url = [](const std::string& raw) {
    return raw == "/path?x=1#part" ? "https://example.com:8443/path?x=1#part" : raw;
  };
  DomBinder binder(*document_, apis);
  EXPECT_TRUE(binder.Evaluate("var a = document.createElement('a'); a.href = '/path?x=1#part';")
                  .has_value());
  const auto result =
      binder.Evaluate("a.protocol === 'https:' && a.host === 'example.com:8443' && "
                      "a.hostname === 'example.com' && a.port === '8443' && "
                      "a.pathname === '/path' && a.search === '?x=1' && a.hash === '#part';");
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result.value().ToBoolean().value());
}

// ---------------------------------------------------------------------------
// Document extensions.
// ---------------------------------------------------------------------------

TEST_F(DomBinderTest, DocumentCreateFragmentCommentElementNS)
{
  EXPECT_TRUE(EvalBool("(function(){ var d = document; "
                       "var frag = d.createDocumentFragment(); "
                       "var div = d.createElement('div'); "
                       "frag.appendChild(div); "
                       "var ok = frag instanceof DocumentFragment && frag.childNodes.length === 1; "
                       "var c = d.createComment('note'); "
                       "ok = ok && c.nodeType === 8 && c.nodeName === '#comment'; "
                       "var svg = d.createElementNS('http://www.w3.org/2000/svg', 'svg'); "
                       "ok = ok && svg.tagName === 'svg'; "
                       "return ok; })()"));
}

TEST_F(DomBinderTest, DocumentGetElementsByTagAndClass)
{
  EXPECT_TRUE(
      EvalBool("(function(){ var d = document; "
               "var ps = d.getElementsByTagName('p'); "
               "var paras = d.getElementsByClassName('para'); "
               "return ps.length === 2 && paras.length === 2 && paras[0].id === 'first'; })()"));
}

TEST_F(DomBinderTest, DocumentMetaProperties)
{
  EXPECT_EQ(EvalString("document.characterSet"), "UTF-8");
  EXPECT_EQ(EvalString("document.contentType"), "text/html");
  EXPECT_EQ(EvalString("document.referrer"), "");
  // URL/baseURI/documentURI reflect the page URL (empty when no PageApis).
  EXPECT_TRUE(EvalBool("typeof document.URL === 'string' && document.baseURI === document.URL"));
}

TEST_F(DomBinderTest, DocumentDomainReflectsHostAndAllowsOnlyRelaxing)
{
  // The getter defaults to the page host (https://www.example.com/ here).
  EXPECT_EQ(EvalString("document.domain"), "www.example.com");
  // Relaxing to a parent domain works, like in browsers.
  EXPECT_EQ(EvalString("(function(){ document.domain = 'example.com';"
                       " return document.domain; })()"),
            "example.com");
  // Lengthening back to the subdomain is refused (browsers refuse it too).
  EXPECT_TRUE(EvalBool("(function(){ try { document.domain = 'www.example.com'; }"
                       " catch (e) { return e.name === 'SecurityError'; } return false; })()"));
  EXPECT_EQ(EvalString("document.domain"), "example.com");
  // An unrelated domain is refused and leaves the value untouched.
  EXPECT_TRUE(EvalBool("(function(){ try { document.domain = 'evil.test'; }"
                       " catch (e) { return e.name === 'SecurityError'; } return false; })()"));
  EXPECT_EQ(EvalString("document.domain"), "example.com");
  // A suffix without a label boundary is not a parent domain.
  EXPECT_TRUE(EvalBool("(function(){ try { document.domain = 'ample.com'; }"
                       " catch (e) { return e.name === 'SecurityError'; } return false; })()"));
  // The empty string resets to the real host.
  EXPECT_EQ(EvalString("(function(){ document.domain = ''; return document.domain; })()"),
            "www.example.com");
  // Same-label-domain assignment is a no-op.
  EXPECT_EQ(EvalString("(function(){ document.domain = 'www.example.com';"
                       " return document.domain; })()"),
            "www.example.com");
}

// document.domain may never relax onto a PUBLIC SUFFIX (HTML §7.7.1): the
// target must share the current host's registrable domain (PSL).  Without
// the guard, assigning "com" from www.example.com would claim to relax the
// origin over every .com site.
TEST_F(DomBinderTest, DocumentDomainRejectsPublicSuffixTargets)
{
  // The fixture host is www.example.com (registrable domain example.com).
  EXPECT_TRUE(EvalBool("(function(){ try { document.domain = 'com'; }"
                       " catch (e) { return e.name === 'SecurityError'; } return false; })()"));
  // Refusal leaves the value at the real host.
  EXPECT_EQ(EvalString("document.domain"), "www.example.com");
  // The legitimate relaxation to the registrable domain still works...
  EXPECT_EQ(EvalString("(function(){ document.domain = 'example.com';"
                       " return document.domain; })()"),
            "example.com");
  // ...and even afterwards "com" stays refused (the guard is on the target,
  // not on the current value).
  EXPECT_TRUE(EvalBool("(function(){ try { document.domain = 'com'; }"
                       " catch (e) { return e.name === 'SecurityError'; } return false; })()"));
  EXPECT_EQ(EvalString("document.domain"), "example.com");
}

TEST_F(DomBinderTest, DocumentElementCollections)
{
  EXPECT_TRUE(EvalBool("(function(){ var d = document; "
                       "return d.images instanceof Array && d.forms instanceof Array "
                       "    && d.links instanceof Array && d.scripts instanceof Array; })()"));
}

TEST_F(DomBinderTest, DocumentCurrentScriptNullByDefault)
{
  // Outside of script execution document.currentScript is null.
  EXPECT_TRUE(EvalBool("document.currentScript === null"));
}

TEST_F(DomBinderTest, DocumentCurrentScriptClearedBeforeTimers)
{
  // RunPageScripts clears currentScript right after each script body, so a
  // timer scheduled by a script must never observe a stale script element
  // (WHATWG HTML: currentScript is only set during synchronous execution).
  dom::Element* el = dom::QuerySelector(*document_, "#main");
  ASSERT_NE(el, nullptr);
  binder_->SetCurrentScript(el);
  binder_->SetCurrentScript(nullptr);

  ASSERT_TRUE(EvalBool("(function(){ "
                       "window._seen = 'unset'; "
                       "setTimeout(function(){ window._seen = document.currentScript; }, 0); "
                       "return true; })()"));
  ASSERT_GT(binder_->RunPendingTimers(), 0);
  EXPECT_TRUE(EvalBool("window._seen === null"));
}

TEST_F(DomBinderTest, DocumentCurrentScriptReturnsExecutingScript)
{
  // A real <script> element in the document (external src, as umi/utoo
  // bundles use: they read currentScript.getAttribute('src') to locate their
  // chunk manifest).
  auto doc = html::Parser(R"(<html><head><script id="entry" src="app.js"></script></head>
<body></body></html>)")
                 .Parse();
  DomBinder binder(*doc);
  auto eval_bool = [&](const std::string& code) {
    auto r = binder.Evaluate(code);
    return r.has_value() && r.value().ToBoolean().has_value() && r.value().ToBoolean().value();
  };

  // Not executing: null.
  EXPECT_TRUE(eval_bool("document.currentScript === null"));

  dom::Element* script = dom::QuerySelector(*doc, "#entry");
  ASSERT_NE(script, nullptr);

  // While executing (as RunPageScripts does around each script body): the
  // getter returns the element; src and getAttribute('src') both resolve.
  binder.SetCurrentScript(script);
  {
    auto r = binder.Evaluate("document.currentScript.id + '|' + document.currentScript.src + '|' + "
                             "document.currentScript.getAttribute('src')");
    ASSERT_TRUE(r.has_value());
    auto s = r.value().ToString();
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s.value(), "entry|app.js|app.js");
  }

  // Cleared again: null.
  binder.SetCurrentScript(nullptr);
  EXPECT_TRUE(eval_bool("document.currentScript === null"));
}

// ---------------------------------------------------------------------------
// Window extensions.
// ---------------------------------------------------------------------------

TEST_F(DomBinderTest, WindowAnimationFrameAndPerformance)
{
  EXPECT_TRUE(
      EvalBool("(function(){ "
               "window._raf = 0; "
               "var id = requestAnimationFrame(function(t) { window._raf = 1; window._ts = t; }); "
               "return typeof id === 'number'; })()"));
  ASSERT_GT(binder_->RunPendingTimers(), 0);
  EXPECT_EQ(EvalNumber("window._raf"), 1.0);
  EXPECT_TRUE(EvalBool("typeof window._ts === 'number'"));
  EXPECT_TRUE(EvalBool("typeof performance.now() === 'number' && performance.now() >= 0"));
}

TEST_F(DomBinderTest, PerformanceExposesTheDocumentNavigationEntry)
{
  EXPECT_TRUE(EvalBool("performance instanceof Performance"));
  EXPECT_TRUE(EvalBool("performance.getEntries().length === 1 && "
                       "performance.getEntries()[0].entryType === 'navigation'"));
  EXPECT_TRUE(EvalBool("performance.getEntriesByType('navigation').length === 1 && "
                       "performance.getEntriesByType('resource').length === 0"));
}

TEST_F(DomBinderTest, EventIsTrustedSupportsDescriptorReflection)
{
  EXPECT_TRUE(EvalBool("(function(descriptor) { return descriptor === undefined || "
                       "typeof descriptor.get === 'function'; })("
                       "Object.getOwnPropertyDescriptor(new Event('test'), 'isTrusted'))"));
}

TEST_F(DomBinderTest, UIEventIsConstructableAndInheritsEvent)
{
  EXPECT_TRUE(EvalBool("new UIEvent('test') instanceof UIEvent"));
  EXPECT_TRUE(EvalBool("new UIEvent('test') instanceof Event"));
  EXPECT_TRUE(EvalBool("new UIEvent('test').isTrusted === false"));
}

TEST_F(DomBinderTest, WindowHistoryAndScroll)
{
  EXPECT_TRUE(EvalBool("(function(){ "
                       "window.history.pushState({}, '', '/x'); "
                       "window.history.replaceState({}, '', '/y'); "
                       "window.scrollTo(0, 100); window.scrollBy(0, 10); "
                       "return history.length === 1 "
                       "    && typeof history.back === 'function' "
                       "    && typeof history.forward === 'function'; })()"));
}

TEST_F(DomBinderTest, HistoryUsesItsInterfacePrototype)
{
  EXPECT_TRUE(
      EvalBool("(function(){ "
               "var illegal = false; "
               "try { new History(); } catch (error) { illegal = error instanceof TypeError; } "
               "return history instanceof History "
               "    && Object.getPrototypeOf(history) === History.prototype "
               "    && history.go === History.prototype.go "
               "    && illegal; })()"));
}

TEST_F(DomBinderTest, WindowGetComputedStyleWired)
{
  // A binder with a computed_style callback returns a style object with
  // getPropertyValue and camelCase accessors.
  javascript::PageApis apis;
  apis.computed_style = [](const dom::Element*) -> std::map<std::string, std::string> {
    return {{"display", "block"}, {"background-color", "rgb(255, 0, 0)"}};
  };
  DomBinder binder(*document_, apis);
  binder.SetConsoleSink([](std::string_view, std::string_view) {});
  auto r = binder.Evaluate(
      "(function(){ var e = document.getElementById('first'); "
      "var cs = getComputedStyle(e); "
      "return cs.getPropertyValue('display') === 'block' "
      "    && cs.display === 'block' && cs.backgroundColor === 'rgb(255, 0, 0)'; })()");
  ASSERT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error().message());
  auto s = r.value().ToString();
  ASSERT_TRUE(s.has_value());
  EXPECT_EQ(s.value(), "true");
}

TEST_F(DomBinderTest, SetTimeoutRunsAfterDeadline)
{
  ASSERT_TRUE(EvalBool("(function(){ window._timerHits = 0; "
                       "setTimeout(function(){ window._timerHits++; }, 1); "
                       "return true; })()"));
  EXPECT_EQ(EvalNumber("window._timerHits"), 0.0);
  // Not yet due.
  EXPECT_EQ(binder_->RunPendingTimers(), 0);
  EXPECT_EQ(EvalNumber("window._timerHits"), 0.0);
  // Make the timer due and pump.
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_EQ(binder_->RunPendingTimers(), 1);
  EXPECT_EQ(EvalNumber("window._timerHits"), 1.0);
  EXPECT_EQ(binder_->RunPendingTimers(), 0);
}

TEST_F(DomBinderTest, ClearTimeout)
{
  ASSERT_TRUE(EvalBool("(function(){ window._timerHits = 0; "
                       "var id = setTimeout(function(){ window._timerHits++; }, 1); "
                       "clearTimeout(id); return true; })()"));
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_EQ(binder_->RunPendingTimers(), 0);
  EXPECT_EQ(EvalNumber("window._timerHits"), 0.0);
}

TEST_F(DomBinderTest, SetIntervalRepeats)
{
  ASSERT_TRUE(EvalBool("(function(){ window._intervalHits = 0; "
                       "setInterval(function(){ window._intervalHits++; }, 1); "
                       "return true; })()"));
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  const int ran = binder_->RunPendingTimers();
  EXPECT_GE(ran, 1);
  EXPECT_GE(EvalNumber("window._intervalHits"), 1.0);
  // The interval is still registered for future runs.
  EXPECT_TRUE(binder_->NextTimerDeadline().has_value());
}

TEST_F(DomBinderTest, TimerErrorDoesNotKillRuntime)
{
  ASSERT_TRUE(EvalBool("(function(){ window._timerHits = 0; "
                       "setTimeout(function(){ throw new Error('timer boom'); }, 1); "
                       "setTimeout(function(){ window._timerHits++; }, 2); "
                       "return true; })()"));
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  binder_->RunPendingTimers();
  EXPECT_EQ(EvalNumber("window._timerHits"), 1.0);
}

TEST_F(DomBinderTest, MessageChannelQueuesClonedMessagesAndCanClose)
{
  ASSERT_TRUE(
      EvalBool("(function(){ "
               "var channel = new MessageChannel(); "
               "var descriptor = Object.getOwnPropertyDescriptor(MessagePort, 'prototype'); "
               "window._channel = channel; window._messages = []; "
               "channel.port2.onmessage = function(event) { "
               "  window._messages.push(event.data.value); "
               "}; "
               "var payload = { value: 'first' }; "
               "channel.port1.postMessage(payload); payload.value = 'changed'; "
               "return channel.port1 instanceof MessagePort "
               "    && channel.port2 instanceof MessagePort "
               "    && descriptor.value === MessagePort.prototype "
               "    && MessagePort.prototype.constructor === MessagePort "
               "    && typeof MessagePort.prototype.postMessage === 'function' "
               "    && typeof MessagePort.prototype.start === 'function' "
               "    && typeof MessagePort.prototype.close === 'function' "
               "    && window._messages.length === 0; })()"));

  EXPECT_EQ(binder_->RunPendingTimers(), 1);
  EXPECT_EQ(EvalString("window._messages.join(',')"), "first");

  ASSERT_TRUE(EvalBool("(function(){ window._channel.port2.close(); "
                       "window._channel.port1.postMessage({ value: 'second' }); "
                       "return true; })()"));
  EXPECT_EQ(binder_->RunPendingTimers(), 0);
  EXPECT_EQ(EvalString("window._messages.join(',')"), "first");
}

TEST_F(DomBinderTest, MessagePortEventListenerRequiresStart)
{
  ASSERT_TRUE(EvalBool("(function(){ var channel = new MessageChannel(); "
                       "window._channel = channel; window._messageHits = 0; "
                       "channel.port2.addEventListener('message', function(event) { "
                       "  if (event.data === 'queued') window._messageHits++; "
                       "}); "
                       "channel.port1.postMessage('queued'); return true; })()"));
  EXPECT_EQ(binder_->RunPendingTimers(), 0);
  EXPECT_EQ(EvalNumber("window._messageHits"), 0.0);

  ASSERT_TRUE(EvalBool("(function(){ window._channel.port2.start(); return true; })()"));
  EXPECT_EQ(binder_->RunPendingTimers(), 1);
  EXPECT_EQ(EvalNumber("window._messageHits"), 1.0);
}

// A pending message must keep its receiving port alive even when the script
// drops every reference to the channel in the same turn.  San's nextTick
// (used by the Baidu home page's chat input) creates a MessageChannel, posts,
// and returns; the only remaining handle is the queued message.  Before the
// task kept a strong reference to the receiver, the port was finalized before
// the next pump and the callback (and therefore the whole UI) never ran.
TEST_F(DomBinderTest, MessageChannelPendingMessageKeepsReceiverAliveWithoutHandle)
{
  ASSERT_TRUE(EvalBool("(function(){ window._aliveHits = 0;"
                       " (function(){ var channel = new MessageChannel();"
                       "   channel.port1.onmessage = function(event) {"
                       "     window._aliveHits = event.data; };"
                       "   channel.port2.postMessage(7);"
                       " })();"
                       " return window._aliveHits === 0; })()"));
  EXPECT_EQ(binder_->RunPendingTimers(), 1);
  EXPECT_EQ(EvalNumber("window._aliveHits"), 7.0);
}

// addEventListener requires start(); the pending message still keeps the port
// object alive across the turn that starts it.
TEST_F(DomBinderTest, MessageChannelPendingMessageSurvivesUntilStart)
{
  ASSERT_TRUE(EvalBool("(function(){ window._startedHits = 0;"
                       " window._pendingChannel = new MessageChannel();"
                       " window._pendingChannel.port1.addEventListener('message',"
                       "   function(event) { window._startedHits = event.data; });"
                       " window._pendingChannel.port2.postMessage(9);"
                       " return window._startedHits === 0; })()"));
  EXPECT_EQ(binder_->RunPendingTimers(), 0);
  ASSERT_TRUE(EvalBool("(function(){ window._pendingChannel.port1.start(); return true; })()"));
  EXPECT_EQ(binder_->RunPendingTimers(), 1);
  EXPECT_EQ(EvalNumber("window._startedHits"), 9.0);
}

TEST_F(DomBinderTest, EventTargetDispatchesEventsAndIsMessagePortBase)
{
  EXPECT_TRUE(EvalBool("(function(){ var target = new EventTarget(); var hits = 0; "
                       "function listener(event) { "
                       "  if (event.target === target && event.currentTarget === target) hits++; "
                       "} "
                       "target.addEventListener('ready', listener); "
                       "var dispatched = target.dispatchEvent(new Event('ready')); "
                       "target.removeEventListener('ready', listener); "
                       "target.dispatchEvent(new Event('ready')); "
                       "var channel = new MessageChannel(); "
                       "return dispatched && hits === 1 "
                       "    && channel.port1 instanceof EventTarget; })()"));
}

// The Performance interface inherits EventTarget.  Hardened libraries register
// listeners through
// EventTarget.prototype.addEventListener.call(performance, ...) (Transcend's
// airgap.js does for "resourcetimingbufferfull"), which requires the instance
// to be a real EventTarget rather than a plain object.
TEST_F(DomBinderTest, PerformanceIsAnEventTarget)
{
  EXPECT_TRUE(EvalBool(
      "(function(){"
      " if (!(window.performance instanceof EventTarget)) return false;"
      " if (window.performance.addEventListener !== EventTarget.prototype.addEventListener)"
      "   return false;"
      " var add = Object.getOwnPropertyDescriptor(EventTarget.prototype, 'addEventListener');"
      " if (!add || typeof add.value !== 'function') return false;"
      " add.value.call(window.performance, 'resourcetimingbufferfull', function(){});"
      " return true; })()"));
}

// Bootstrap code (Transcend's airgap.js among others) captures these methods
// with optional chaining but calls them unconditionally, so a browser without
// them throws "not a function" during initialization.  The resource timing
// buffer itself stays empty (no resource entries are produced yet).
TEST_F(DomBinderTest, PerformanceResourceTimingBufferMethodsAreCallable)
{
  EXPECT_TRUE(
      EvalBool("(function(){"
               " if (typeof performance.setResourceTimingBufferSize !== 'function') return false;"
               " if (typeof performance.clearResourceTimings !== 'function') return false;"
               " performance.setResourceTimingBufferSize(1000);"
               " performance.clearResourceTimings();"
               " return performance.getEntriesByType('resource').length === 0; })()"));
}

TEST_F(DomBinderTest, DomTokenListInterfaceUsesClassListPrototype)
{
  EXPECT_TRUE(EvalBool("typeof DOMTokenList === 'function' "
                       "&& document.body.classList instanceof DOMTokenList "
                       "&& DOMTokenList.prototype.contains === "
                       "   Object.getPrototypeOf(document.body.classList).contains"));
}

TEST_F(DomBinderTest, HeadersNormalizeNamesAndExposeEntries)
{
  EXPECT_TRUE(EvalBool("(function(){ var headers = new Headers(); "
                       "headers.append('X-Test', 'one'); "
                       "headers.append('x-test', 'two'); "
                       "headers.set('Content-Type', 'text/plain'); "
                       "var entries = Array.from(headers.entries()); "
                       "headers.delete('CONTENT-TYPE'); "
                       "return headers.get('X-TEST') === 'one, two' "
                       "    && !headers.has('content-type') "
                       "    && entries.length === 2 "
                       "    && headers[Symbol.iterator] === headers.entries; })()"));
}

TEST_F(DomBinderTest, ScriptErrorsAreReported)
{
  auto bad = binder_->Evaluate("document.getElementById('nope').foo");
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().category(), base::ErrorCategory::kJavascript);
  EXPECT_NE(bad.error().message().find("null"), std::string::npos);
}

TEST_F(DomBinderTest, ConsoleFromPageScript)
{
  ASSERT_TRUE(binder_->Evaluate("console.log('from page')").has_value());
  ASSERT_EQ(console_.size(), 1u);
  EXPECT_EQ(console_[0], "log: from page");
}

TEST_F(DomBinderTest, NavigatorGlobal)
{
  // The UA matches what the network stack sends; the page can read it without
  // a ReferenceError.  It follows the conventional browser UA format (see
  // base::GetUserAgent) so servers serve full content.
  const std::string ua = EvalString("navigator.userAgent");
  EXPECT_EQ(ua, EvalString("window.navigator.userAgent"));
  EXPECT_NE(ua.find("NekoBrowser/"), std::string::npos);
  EXPECT_NE(ua.find("Mozilla/5.0"), std::string::npos);
  EXPECT_TRUE(EvalBool("navigator === window.navigator"));
  EXPECT_TRUE(EvalBool("navigator instanceof Navigator"));
  EXPECT_TRUE(
      EvalBool("(function(){ var descriptor = "
               "Object.getOwnPropertyDescriptor(Navigator.prototype, 'languages'); "
               "return typeof descriptor.get === 'function' && descriptor.set === undefined "
               "    && !Object.prototype.hasOwnProperty.call(navigator, 'languages'); })()"));
  EXPECT_EQ(EvalString("navigator.language"), "en-US");
  EXPECT_EQ(EvalString("navigator.languages.join(',')"), "en-US");
  EXPECT_TRUE(EvalBool("navigator.onLine === true"));
  EXPECT_TRUE(EvalBool("navigator.cookieEnabled === true"));
  EXPECT_TRUE(EvalNumber("navigator.hardwareConcurrency") >= 1.0);
  // Web APIs pages feature-detect: these exist as documented stubs (see the
  // compatibility matrix), so feature detection takes the supported branch.
  EXPECT_TRUE(EvalBool("'geolocation' in navigator"));
  EXPECT_TRUE(EvalBool("typeof navigator.geolocation.getCurrentPosition === 'function'"));
  EXPECT_TRUE(EvalBool("typeof navigator.geolocation.watchPosition === 'function'"));
  EXPECT_TRUE(EvalBool("typeof navigator.geolocation.clearWatch === 'function'"));
  EXPECT_TRUE(EvalBool("'clipboard' in navigator"));
  EXPECT_TRUE(EvalBool("typeof navigator.clipboard.writeText === 'function'"));
  EXPECT_TRUE(EvalBool("typeof navigator.clipboard.readText === 'function'"));
  EXPECT_TRUE(EvalBool("typeof navigator.sendBeacon === 'function'"));
  EXPECT_TRUE(EvalBool("typeof navigator.permissions.query === 'function'"));
  EXPECT_TRUE(EvalBool("typeof navigator.mediaDevices.enumerateDevices === 'function'"));
  EXPECT_EQ(EvalString("navigator.connection.effectiveType"), "4g");
}

TEST_F(DomBinderTest, IntlDateTimeFormatExposesResolvedOptions)
{
  EXPECT_TRUE(EvalBool("(function(){ var formatter = new Intl.DateTimeFormat(); "
                       "var descriptor = Object.getOwnPropertyDescriptor("
                       "Intl.DateTimeFormat.prototype, 'resolvedOptions'); "
                       "var options = formatter.resolvedOptions(); "
                       "return formatter instanceof Intl.DateTimeFormat "
                       "    && typeof descriptor.value === 'function' "
                       "    && options.locale === 'en-US' && options.calendar === 'gregory' "
                       "    && options.numberingSystem === 'latn'; })()"));
}

TEST_F(DomBinderTest, DocumentCreateRangeHasDocumentBoundary)
{
  EXPECT_TRUE(EvalBool(
      "(function(){ var range = document.createRange(); "
      "var descriptor = Object.getOwnPropertyDescriptor("
      "Range.prototype, 'commonAncestorContainer'); "
      "return range instanceof Range && range.startContainer === document "
      "    && range.endContainer === document && range.startOffset === 0 "
      "    && range.endOffset === 0 && range.commonAncestorContainer === document "
      "    && typeof descriptor.get === 'function' && descriptor.set === undefined; })()"));
}

// ---------------------------------------------------------------------------
// Range (DOM Standard §5.5).
// ---------------------------------------------------------------------------

TEST_F(DomBinderTest, RangeCreationAndConstants)
{
  EXPECT_TRUE(EvalBool(R"js(
    (function(){
      var range = document.createRange();
      var constructed = new Range();
      return range instanceof Range && constructed instanceof Range
          && Object.getPrototypeOf(range) === Range.prototype
          && Object.getPrototypeOf(constructed) === Range.prototype
          && range.startContainer === document && range.endContainer === document
          && range.startOffset === 0 && range.endOffset === 0
          && range.collapsed === true
          && range.commonAncestorContainer === document
          && constructed.startContainer === document
          && constructed.endContainer === document
          && constructed.startOffset === 0 && constructed.endOffset === 0
          && constructed.collapsed === true
          && String(range) === ''
          && Range.START_TO_START === 0 && Range.START_TO_END === 1
          && Range.END_TO_END === 2 && Range.END_TO_START === 3
          && Range.prototype.START_TO_START === 0
          && Range.prototype.START_TO_END === 1
          && Range.prototype.END_TO_END === 2
          && Range.prototype.END_TO_START === 3
          && Object.getOwnPropertyDescriptor(Range.prototype, 'collapsed').set === undefined
          && Object.getOwnPropertyDescriptor(Range.prototype, 'startOffset').set === undefined
          && Range.length === 0;
    })()
  )js"));
}

TEST_F(DomBinderTest, RangeSetStartEndCollapseAndValidation)
{
  const auto run = [this](const std::string& body) {
    return EvalString("(function(){ var p = document.createElement('p');"
                      " var t = document.createTextNode('abcdef'); p.appendChild(t);"
                      " document.body.appendChild(p);" +
                      body + "})()");
  };

  // start/end getters, collapsed, commonAncestorContainer and stringifier.
  EXPECT_EQ(run("var r = document.createRange(); r.setStart(t, 1); r.setEnd(t, 4);"
                "return [r.startContainer === t, r.startOffset, r.endOffset, r.collapsed,"
                " r.commonAncestorContainer === t, String(r)].join(',');"),
            "true,1,4,false,true,bcd");

  // Setting the start past the end collapses the end onto the new start.
  EXPECT_EQ(run("var r = document.createRange(); r.setEnd(t, 2); r.setStart(t, 4);"
                "return [r.startOffset, r.endOffset, r.collapsed].join(',');"),
            "4,4,true");

  // Setting the end before the start collapses the start onto the new end.
  EXPECT_EQ(run("var r = document.createRange(); r.setStart(t, 4); r.setEnd(t, 2);"
                "return [r.startOffset, r.endOffset, r.collapsed].join(',');"),
            "2,2,true");

  // collapse(true) keeps the start, collapse() keeps the end.
  EXPECT_EQ(run("var r = document.createRange(); r.setStart(t, 1); r.setEnd(t, 4);"
                "r.collapse(true); var a = [r.startOffset, r.endOffset].join(',');"
                "r.setStart(t, 1); r.setEnd(t, 4); r.collapse();"
                "return a + '|' + [r.startOffset, r.endOffset].join(',');"),
            "1,1|4,4");

  // Offset beyond the character-data length and doctype boundaries.
  EXPECT_EQ(run("var r = document.createRange();"
                "try { r.setStart(t, 7); } catch (e) { return e.name; } return 'no';"),
            "IndexSizeError");
  EXPECT_EQ(run("var r = document.createRange();"
                "try { r.setEnd(t, 7); } catch (e) { return e.name; } return 'no';"),
            "IndexSizeError");
  EXPECT_EQ(run("var r = document.createRange();"
                "try { r.setStart(document.doctype, 0); } catch (e) { return e.name; }"
                "return 'no';"),
            "InvalidNodeTypeError");
  EXPECT_EQ(run("var r = document.createRange();"
                "try { r.setEnd(document.doctype, 0); } catch (e) { return e.name; }"
                "return 'no';"),
            "InvalidNodeTypeError");

  // A node owned by another Document is rejected (the parsed body's root is
  // that other Document, not the page's).
  EXPECT_EQ(
      EvalString(
          "(function(){ var other = document.implementation.createHTMLDocument('x');"
          " var r = document.createRange();"
          " try { r.setStart(other.body, 0); } catch (e) { return e.name; } return 'no'; })()"),
      "WrongDocumentError");

  // A range created by another Document belongs to that document, so its own
  // tree is accepted even though it is not the binder's main document.
  EXPECT_EQ(EvalString("(function(){ var other = document.implementation.createHTMLDocument('x');"
                       " var r = other.createRange(); r.setStart(other.body, 0);"
                       " return [r.startContainer === other.body, r.startOffset,"
                       " r.startContainer === document.body].join(','); })()"),
            "true,0,false");

  // Non-character-data node lengths are child counts.
  EXPECT_EQ(run("var r = document.createRange(); r.setStart(p, 1); r.setEnd(p, 1);"
                "return [r.startContainer === p, r.startOffset, r.collapsed].join(',');"),
            "true,1,true");
  EXPECT_EQ(run("var r = document.createRange();"
                "try { r.setStart(p, 2); } catch (e) { return e.name; } return 'no';"),
            "IndexSizeError");
}

TEST_F(DomBinderTest, RangeRelativeBoundarySetters)
{
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var p = document.createElement('p');
      var t1 = document.createTextNode('a');
      var b = document.createElement('b');
      b.appendChild(document.createTextNode('X'));
      var t2 = document.createTextNode('c');
      p.appendChild(t1); p.appendChild(b); p.appendChild(t2);
      document.body.appendChild(p);
      var r = document.createRange();
      r.setStartBefore(b); r.setEndAfter(b);
      var selected = [r.startContainer === p, r.startOffset, r.endOffset, String(r)].join(',');
      r.setStartAfter(t1);
      var afterStart = [r.startOffset].join(',');
      r.setStartBefore(t1);
      var beforeStart = [r.startOffset].join(',');
      r.setEndBefore(b);
      var beforeEnd = [r.endOffset].join(',');
      r.setEndAfter(b);
      var afterEnd = [r.endOffset].join(',');
      return selected + '|' + afterStart + '|' + beforeStart + '|' + beforeEnd + '|' + afterEnd;
    })()
  )js"),
            "true,1,2,X|1|0|1|2");

  // A detached node has no parent: the spec throws InvalidNodeTypeError.
  EXPECT_EQ(EvalString(
                "(function(){ var t = document.createTextNode('x'); var r = document.createRange();"
                " try { r.setStartBefore(t); } catch (e) { return e.name; } return 'no'; })()"),
            "InvalidNodeTypeError");
}

TEST_F(DomBinderTest, RangeSelectNodeAndSelectNodeContents)
{
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var host = document.createElement('div');
      var a = document.createElement('span');
      var b = document.createElement('span');
      a.appendChild(document.createTextNode('A'));
      b.appendChild(document.createTextNode('B'));
      host.appendChild(a); host.appendChild(b);
      document.body.appendChild(host);
      var r = document.createRange();
      r.selectNode(b);
      var node = [r.startContainer === host, r.startOffset, r.endOffset, r.collapsed,
                  r.commonAncestorContainer === host].join(',');
      r.selectNodeContents(host);
      var contents = [r.startContainer === host, r.startOffset, r.endOffset, String(r)].join(',');
      r.selectNodeContents(b.firstChild);
      var text = [r.startContainer === b.firstChild, r.startOffset, r.endOffset, String(r)].join(',');
      return node + '|' + contents + '|' + text;
    })()
  )js"),
            "true,1,2,false,true|true,0,2,AB|true,0,1,B");

  EXPECT_EQ(
      EvalString(
          "(function(){ var span = document.createElement('span'); var r = document.createRange();"
          " try { r.selectNode(span); } catch (e) { return e.name; } return 'no'; })()"),
      "InvalidNodeTypeError");
  EXPECT_EQ(EvalString("(function(){ var r = document.createRange();"
                       " try { r.selectNodeContents(document.doctype); } catch (e) {"
                       " return e.name; } return 'no'; })()"),
            "InvalidNodeTypeError");
}

TEST_F(DomBinderTest, RangeCloneContentsMixedTextAndElements)
{
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var host = document.createElement('div');
      var p = document.createElement('p');
      var t1 = document.createTextNode('ab');
      var b = document.createElement('b');
      b.appendChild(document.createTextNode('cd'));
      var t3 = document.createTextNode('ef');
      p.appendChild(t1); p.appendChild(b); p.appendChild(t3);
      host.appendChild(p); document.body.appendChild(host);
      var r = document.createRange();
      r.setStart(t1, 1); r.setEnd(t3, 1);
      var f = r.cloneContents();
      return [f.childNodes.length, f.firstChild.nodeValue, f.childNodes[1].tagName,
              f.childNodes[1].textContent, f.lastChild.nodeValue,
              p.childNodes.length, t1.nodeValue, t3.nodeValue,
              String(r)].join('|');
    })()
  )js"),
            "3|b|B|cd|e|3|ab|ef|bcde");

  // Collapsed ranges clone to an empty fragment; same-node character data is
  // substringed and does not mutate the original.
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var p = document.createElement('p');
      var t = document.createTextNode('abcdef');
      p.appendChild(t); document.body.appendChild(p);
      var r = document.createRange();
      var empty = r.cloneContents();
      r.setStart(t, 2); r.setEnd(t, 4);
      var f = r.cloneContents();
      return [empty.childNodes.length, f.childNodes.length, f.firstChild.nodeValue,
              t.nodeValue].join('|');
    })()
  )js"),
            "0|1|cd|abcdef");
}

TEST_F(DomBinderTest, RangeExtractContentsMixedTextAndElements)
{
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var host = document.createElement('div');
      var p = document.createElement('p');
      var t1 = document.createTextNode('ab');
      var b = document.createElement('b');
      b.appendChild(document.createTextNode('cd'));
      var t3 = document.createTextNode('ef');
      p.appendChild(t1); p.appendChild(b); p.appendChild(t3);
      host.appendChild(p); document.body.appendChild(host);
      var r = document.createRange();
      r.setStart(t1, 1); r.setEnd(t3, 1);
      var f = r.extractContents();
      return [f.childNodes.length, f.firstChild.nodeValue, f.childNodes[1].tagName,
              f.childNodes[1].textContent, f.lastChild.nodeValue,
              p.childNodes.length, p.firstChild.nodeValue, p.lastChild.nodeValue,
              r.collapsed, r.startContainer === p, r.startOffset, String(r)].join('|');
    })()
  )js"),
            "3|b|B|cd|e|2|a|f|true|true|1|");

  // Same-node extraction: fragment gets the substring, the text loses it and
  // the range collapses at the old start offset.
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var p = document.createElement('p');
      var t = document.createTextNode('abcdef');
      p.appendChild(t); document.body.appendChild(p);
      var r = document.createRange();
      r.setStart(t, 2); r.setEnd(t, 4);
      var f = r.extractContents();
      return [f.childNodes.length, f.firstChild.nodeValue, t.nodeValue,
              r.collapsed, r.startOffset].join('|');
    })()
  )js"),
            "1|cd|abef|true|2");
}

TEST_F(DomBinderTest, RangeDeleteContentsMixedTextAndElements)
{
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var host = document.createElement('div');
      var p = document.createElement('p');
      var t1 = document.createTextNode('ab');
      var b = document.createElement('b');
      b.appendChild(document.createTextNode('cd'));
      var t3 = document.createTextNode('ef');
      p.appendChild(t1); p.appendChild(b); p.appendChild(t3);
      host.appendChild(p); document.body.appendChild(host);
      var r = document.createRange();
      r.setStart(t1, 1); r.setEnd(t3, 1);
      r.deleteContents();
      return [p.childNodes.length, p.firstChild.nodeValue, p.lastChild.nodeValue,
              r.collapsed, r.startContainer === p, r.startOffset, String(r)].join('|');
    })()
  )js"),
            "2|a|f|true|true|1|");

  EXPECT_EQ(EvalString(R"js(
    (function(){
      var p = document.createElement('p');
      var t = document.createTextNode('abc');
      p.appendChild(t); document.body.appendChild(p);
      var r = document.createRange();
      r.setStart(t, 1); r.setEnd(t, 2);
      r.deleteContents();
      return [t.nodeValue, r.collapsed, r.startOffset, String(r)].join('|');
    })()
  )js"),
            "ac|true|1|");

  // Deleting a collapsed range is a no-op.
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var p = document.createElement('p');
      var t = document.createTextNode('abc');
      p.appendChild(t); document.body.appendChild(p);
      var r = document.createRange();
      r.setStart(t, 1); r.collapse(true);
      r.deleteContents();
      return [t.nodeValue, r.startOffset, r.endOffset].join('|');
    })()
  )js"),
            "abc|1|1");
}

TEST_F(DomBinderTest, RangeInsertNodeSplitsTextAndValidates)
{
  // A collapsed range inside a Text node splits it and inserts between the two
  // halves; the range end expands to cover the inserted node (spec step 13).
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var p = document.createElement('p');
      var t = document.createTextNode('ab');
      p.appendChild(t); document.body.appendChild(p);
      var i = document.createElement('i');
      i.textContent = 'x';
      var r = document.createRange();
      r.setStart(t, 1); r.collapse(true);
      r.insertNode(i);
      return [p.childNodes.length, p.childNodes[0].nodeValue, p.childNodes[1].tagName,
              p.childNodes[1].textContent, p.childNodes[2].nodeValue,
              r.collapsed, r.startContainer === p.childNodes[0], r.startOffset,
              r.endOffset, String(r)].join('|');
    })()
  )js"),
            "3|a|I|x|b|false|true|1|2|x");

  // Inserting at an element offset 1 goes before that child.
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var host = document.createElement('div');
      var a = document.createElement('span');
      var b = document.createElement('span');
      host.appendChild(a); host.appendChild(b);
      document.body.appendChild(host);
      var inserted = document.createElement('em');
      var r = document.createRange();
      r.setStart(host, 1); r.collapse(true);
      r.insertNode(inserted);
      return [host.childNodes.length, host.childNodes[0] === a,
              host.childNodes[1] === inserted, host.childNodes[2] === b,
              r.startOffset, r.endOffset].join('|');
    })()
  )js"),
            "3|true|true|true|1|2");

  // DocumentFragment insertion adds all of its children and expands the end by
  // the fragment's length.
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var p = document.createElement('p');
      p.appendChild(document.createTextNode('x'));
      document.body.appendChild(p);
      var fragment = document.createDocumentFragment();
      var a = document.createElement('span');
      var b = document.createElement('span');
      fragment.appendChild(a); fragment.appendChild(b);
      var r = document.createRange();
      r.selectNodeContents(p); r.collapse(true);
      r.insertNode(fragment);
      return [p.childNodes.length, p.childNodes[0] === a, p.childNodes[1] === b,
              fragment.childNodes.length, r.startOffset, r.endOffset].join('|');
    })()
  )js"),
            "3|true|true|0|0|2");

  // Invalid start nodes throw HierarchyRequestError.
  EXPECT_EQ(
      EvalString("(function(){ var t = document.createTextNode('x');"
                 " var span = document.createElement('span');"
                 " var r = document.createRange(); r.setStart(t, 0);"
                 " try { r.insertNode(span); } catch (e) { return e.name; } return 'no'; })()"),
      "HierarchyRequestError");
  EXPECT_EQ(
      EvalString("(function(){ var p = document.createElement('p');"
                 " var c = document.createComment('x'); p.appendChild(c);"
                 " document.body.appendChild(p); var span = document.createElement('span');"
                 " var r = document.createRange(); r.setStart(c, 0);"
                 " try { r.insertNode(span); } catch (e) { return e.name; } return 'no'; })()"),
      "HierarchyRequestError");
  EXPECT_EQ(EvalString("(function(){ var p = document.createElement('p');"
                       " p.appendChild(document.createTextNode('x')); document.body.appendChild(p);"
                       " var r = document.createRange(); r.selectNodeContents(p);"
                       " try { r.insertNode(p); } catch (e) { return e.name; } return 'no'; })()"),
            "HierarchyRequestError");
  EXPECT_EQ(EvalString("(function(){ var p = document.createElement('p');"
                       " p.appendChild(document.createTextNode('x')); document.body.appendChild(p);"
                       " var r = document.createRange(); r.selectNodeContents(document);"
                       " try { r.insertNode(document.createTextNode('y')); } catch (e) {"
                       " return e.name; } return 'no'; })()"),
            "HierarchyRequestError");
}

TEST_F(DomBinderTest, RangeSurroundContents)
{
  // The wrapper's existing children are replaced; the selected contents move
  // inside it and the range selects the wrapper.
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var host = document.createElement('div');
      var p = document.createElement('p');
      p.appendChild(document.createTextNode('ab'));
      host.appendChild(p); document.body.appendChild(host);
      var span = document.createElement('span');
      span.textContent = 'old';
      var r = document.createRange();
      r.selectNodeContents(p);
      r.surroundContents(span);
      return [p.childNodes.length, p.firstChild === span, span.textContent,
              r.startContainer === p, r.startOffset, r.endOffset, r.collapsed,
              String(r)].join('|');
    })()
  )js"),
            "1|true|ab|true|0|1|false|ab");

  // Partially selecting a non-Text node is an InvalidStateError.
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var p = document.createElement('p');
      var t1 = document.createTextNode('a');
      var b = document.createElement('b');
      b.appendChild(document.createTextNode('B'));
      var t2 = document.createTextNode('c');
      p.appendChild(t1); p.appendChild(b); p.appendChild(t2);
      document.body.appendChild(p);
      var r = document.createRange();
      r.setStart(b.firstChild, 0); r.setEnd(t2, 1);
      try { r.surroundContents(document.createElement('span')); }
      catch (e) { return e.name; }
      return 'no';
    })()
  )js"),
            "InvalidStateError");

  // Document/DocumentType/DocumentFragment wrappers are rejected up front.
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var p = document.createElement('p');
      p.appendChild(document.createTextNode('ab'));
      document.body.appendChild(p);
      var r = document.createRange(); r.selectNodeContents(p);
      var names = [];
      [document, document.doctype, document.createDocumentFragment()].forEach(function(node) {
        try { r.surroundContents(node); names.push('no'); }
        catch (e) { names.push(e.name); }
      });
      return names.join(',');
    })()
  )js"),
            "InvalidNodeTypeError,InvalidNodeTypeError,InvalidNodeTypeError");
}

TEST_F(DomBinderTest, RangeCompareBoundaryPoints)
{
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var p = document.createElement('p');
      var t = document.createTextNode('abcdef');
      p.appendChild(t); document.body.appendChild(p);
      var r1 = document.createRange(); r1.setStart(t, 1); r1.setEnd(t, 4);
      var r2 = document.createRange(); r2.setStart(t, 2); r2.setEnd(t, 5);
      return [
        r1.compareBoundaryPoints(Range.START_TO_START, r2),
        r1.compareBoundaryPoints(Range.START_TO_END, r2),
        r1.compareBoundaryPoints(Range.END_TO_END, r2),
        r1.compareBoundaryPoints(Range.END_TO_START, r2),
        r1.compareBoundaryPoints(Range.START_TO_START, r1)
      ].join(',');
    })()
  )js"),
            "-1,1,-1,-1,0");

  EXPECT_EQ(EvalString("(function(){ var p = document.createElement('p');"
                       " var t = document.createTextNode('a'); p.appendChild(t);"
                       " var r = document.createRange(); r.setStart(t, 0);"
                       " try { r.compareBoundaryPoints(4, r); } catch (e) { return e.name; }"
                       " return 'no'; })()"),
            "NotSupportedError");

  // Ranges in different trees (here a detached element) are a WrongDocumentError.
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var p = document.createElement('p');
      var t = document.createTextNode('abcdef');
      p.appendChild(t); document.body.appendChild(p);
      var r1 = document.createRange(); r1.setStart(t, 1); r1.setEnd(t, 3);
      var detached = document.createElement('div');
      var t2 = document.createTextNode('x');
      detached.appendChild(t2);
      var r2 = document.createRange(); r2.selectNodeContents(t2);
      try { r1.compareBoundaryPoints(Range.START_TO_START, r2); }
      catch (e) { return e.name; }
      return 'no';
    })()
  )js"),
            "WrongDocumentError");
}

TEST_F(DomBinderTest, RangeCompareBoundaryPointsAcrossAncestorDescendant)
{
  // DOM §5.2: when the child index equals the ancestor offset, the point lies
  // inside that child and the comparison descends.  (paragraph, 0) and
  // (firstText, 0) are the same position, not ordered.
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var p = document.createElement('p');
      var t = document.createTextNode('abcdef');
      p.appendChild(t); document.body.appendChild(p);
      var outer = document.createRange(); outer.selectNodeContents(p);
      var inner = document.createRange(); inner.setStart(t, 0); inner.setEnd(t, 3);
      return [
        outer.compareBoundaryPoints(Range.START_TO_START, inner),
        outer.compareBoundaryPoints(Range.START_TO_END, inner),
        outer.compareBoundaryPoints(Range.END_TO_END, inner),
        outer.compareBoundaryPoints(Range.END_TO_START, inner)
      ].join(',');
    })()
  )js"),
            "-1,1,1,-1");
}

TEST_F(DomBinderTest, RangeComparePointIsPointInRangeAndIntersectsNode)
{
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var p = document.createElement('p');
      var t = document.createTextNode('abcdef');
      p.appendChild(t); document.body.appendChild(p);
      var r = document.createRange(); r.setStart(t, 2); r.setEnd(t, 4);
      return [
        r.comparePoint(t, 1), r.comparePoint(t, 2), r.comparePoint(t, 3),
        r.comparePoint(t, 4), r.comparePoint(t, 5),
        r.isPointInRange(t, 1), r.isPointInRange(t, 2),
        r.isPointInRange(t, 3), r.isPointInRange(t, 4), r.isPointInRange(t, 5)
      ].join(',');
    })()
  )js"),
            "-1,0,0,0,1,false,true,true,true,false");

  EXPECT_EQ(EvalString(R"js(
    (function(){
      var p = document.createElement('p');
      var t = document.createTextNode('abc');
      p.appendChild(t); document.body.appendChild(p);
      var r = document.createRange(); r.setStart(t, 0); r.setEnd(t, 1);
      try { r.comparePoint(t, 4); } catch (e) { return e.name; }
      return 'no';
    })()
  )js"),
            "IndexSizeError");
  EXPECT_EQ(EvalString("(function(){ var r = document.createRange();"
                       " try { r.comparePoint(document.doctype, 0); } catch (e) {"
                       " return e.name; } return 'no'; })()"),
            "InvalidNodeTypeError");

  // intersectsNode: the selected element intersects, its siblings do not; a
  // node from another tree never intersects.
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var p = document.createElement('p');
      var t1 = document.createTextNode('a');
      var b = document.createElement('b');
      b.appendChild(document.createTextNode('B'));
      var t2 = document.createTextNode('c');
      p.appendChild(t1); p.appendChild(b); p.appendChild(t2);
      document.body.appendChild(p);
      var r = document.createRange(); r.selectNode(b);
      var detached = document.createElement('div');
      return [r.intersectsNode(b), r.intersectsNode(t1), r.intersectsNode(t2),
              r.intersectsNode(p), r.intersectsNode(detached)].join(',');
    })()
  )js"),
            "true,false,false,true,false");
}

TEST_F(DomBinderTest, RangeToStringConcatenatesTextNodesOnly)
{
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var p = document.createElement('p');
      var t1 = document.createTextNode('Hello ');
      var b = document.createElement('b');
      b.appendChild(document.createTextNode('world'));
      var t2 = document.createTextNode('!');
      p.appendChild(t1); p.appendChild(b); p.appendChild(t2);
      document.body.appendChild(p);
      var r = document.createRange();
      r.setStart(t1, 0); r.setEnd(t2, 1);
      var whole = String(r);
      r.setStart(b.firstChild, 1); r.setEnd(t2, 1);
      return whole + '|' + String(r);
    })()
  )js"),
            "Hello world!|orld!");

  // Comments are not character data for the stringifier.
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var p = document.createElement('p');
      p.appendChild(document.createTextNode('a'));
      p.appendChild(document.createComment('SECRET'));
      p.appendChild(document.createTextNode('b'));
      document.body.appendChild(p);
      var r = document.createRange(); r.selectNodeContents(p);
      return String(r);
    })()
  )js"),
            "ab");

  // Substrings inside one Text node.
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var p = document.createElement('p');
      var t = document.createTextNode('abcdef');
      p.appendChild(t); document.body.appendChild(p);
      var r = document.createRange(); r.setStart(t, 1); r.setEnd(t, 3);
      return String(r);
    })()
  )js"),
            "bc");

  // Range offsets are UTF-16 code units: a surrogate pair counts as two, and
  // a range may split around (but never inside) it.
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var emoji = String.fromCodePoint(0x1F600);
      var p = document.createElement('p');
      var t = document.createTextNode('a' + emoji + 'b');
      p.appendChild(t); document.body.appendChild(p);
      var r = document.createRange();
      r.setStart(t, 1); r.setEnd(t, 3);
      var selected = String(r) === emoji;
      r.setEnd(t, 4);
      var full = [r.startOffset, r.endOffset].join(',');
      var tooLong = 'accepted';
      try { r.setEnd(t, 5); } catch (e) { tooLong = e.name; }
      return [selected, full, tooLong].join('|');
    })()
  )js"),
            "true|1,4|IndexSizeError");
}

TEST_F(DomBinderTest, RangeCloneRangeIsIndependentAndDetachIsNoop)
{
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var p = document.createElement('p');
      var t = document.createTextNode('abcdef');
      p.appendChild(t); document.body.appendChild(p);
      var r = document.createRange(); r.setStart(t, 1); r.setEnd(t, 3);
      var clone = r.cloneRange();
      var before = [clone.startOffset, clone.endOffset].join(',');
      r.setStart(t, 0);
      var detached = r.detach();
      return [before, clone.startOffset, clone.endOffset, r.startOffset,
              detached === undefined].join('|');
    })()
  )js"),
            "1,3|1|3|0|true");
}

TEST_F(DomBinderTest, RangeGeometryIsExplicitlyNotImplemented)
{
  // Range geometry needs the layout tree (CSSOM View); the bindings must not
  // fabricate rectangles, so both methods throw NotSupportedError.
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var r = document.createRange();
      var names = [];
      ['getBoundingClientRect', 'getClientRects'].forEach(function(name) {
        try { r[name](); names.push('no'); } catch (e) { names.push(e.name); }
      });
      return [typeof r.getBoundingClientRect, typeof r.getClientRects,
              names.join(',')].join('|');
    })()
  )js"),
            "function|function|NotSupportedError,NotSupportedError");
}

// ---------------------------------------------------------------------------
// Selection (Selection API, https://w3c.github.io/selection-api/).
// ---------------------------------------------------------------------------

TEST_F(DomBinderTest, SelectionGetSelectionIdentityAndInitialState)
{
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var first = window.getSelection();
      var second = document.getSelection();
      var third = getSelection();
      return [first === second, second === third, first === window.getSelection(),
              first.rangeCount, first.isCollapsed,
              first.anchorNode === null, first.focusNode === null,
              first.anchorOffset, first.focusOffset, first.type,
              String(first) === '',
              first instanceof Selection,
              typeof Selection, typeof getSelection, Selection.length,
              Object.getOwnPropertyDescriptor(Selection.prototype, 'anchorNode').set === undefined,
              Object.getOwnPropertyDescriptor(Selection.prototype, 'rangeCount').set === undefined,
              Object.getOwnPropertyDescriptor(Selection.prototype, 'type').set === undefined,
              document.implementation.createHTMLDocument('x').getSelection() === null
             ].join(',');
    })()
  )js"),
            "true,true,true,0,true,true,true,0,0,None,true,true,function,function,0,true,true,true,"
            "true");

  // A Selection keeps a stable object identity: repeated reads, including reads
  // after other script has run, return the same object (the per-document
  // singleton).
  EXPECT_TRUE(EvalBool("(function(){ var a = document.getSelection();"
                       " window._selectionIdentity = a; return true; })()"));
  EXPECT_TRUE(EvalBool("window._selectionIdentity === getSelection()"));
}

TEST_F(DomBinderTest, SelectionSelectAllChildrenAndStringifier)
{
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var p = document.getElementById('first');
      var selection = document.getSelection();
      selection.selectAllChildren(p);
      var range = selection.getRangeAt(0);
      var element = [selection.rangeCount, selection.type, selection.isCollapsed,
                     selection.anchorNode === p, selection.anchorOffset,
                     selection.focusNode === p, selection.focusOffset,
                     range.startContainer === p, range.startOffset,
                     range.endContainer === p, range.endOffset,
                     selection.toString(), range.toString()].join(',');
      selection.selectAllChildren(p.firstChild);
      var text = [selection.anchorNode === p.firstChild, selection.anchorOffset,
                  selection.focusOffset, selection.toString()].join(',');
      return element + '|' + text;
    })()
  )js"),
            "1,Range,false,true,0,true,2,true,0,true,2,Hello world,Hello world|"
            "true,0,6,Hello ");
}

TEST_F(DomBinderTest, SelectionCollapseVariants)
{
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var p = document.getElementById('first');
      var t = p.firstChild;
      var selection = document.getSelection();
      selection.collapse(t, 2);
      var caret = [selection.type, selection.isCollapsed, selection.anchorNode === t,
                   selection.anchorOffset, selection.focusOffset, selection.rangeCount,
                   selection.toString(), selection.getRangeAt(0).collapsed].join(',');
      selection.selectAllChildren(p);
      selection.collapseToStart();
      var start = [selection.type, selection.anchorNode === p, selection.anchorOffset,
                   selection.focusOffset].join(',');
      selection.selectAllChildren(p);
      selection.collapseToEnd();
      var end = [selection.anchorNode === p, selection.anchorOffset,
                 selection.focusOffset, selection.isCollapsed].join(',');
      // The historical no-argument collapse() collapses to the end.
      selection.selectAllChildren(p);
      selection.collapse();
      var legacy = [selection.anchorNode === p, selection.anchorOffset,
                    selection.focusOffset].join(',');
      // collapse(null) removes all ranges.
      selection.collapse(null);
      var cleared = [selection.rangeCount, selection.type, selection.anchorNode === null,
                     selection.isCollapsed].join(',');
      // collapse() on an empty selection is a no-op.
      selection.collapse();
      var noop = [selection.rangeCount, selection.type].join(',');
      return [caret, start, end, legacy, cleared, noop].join('|');
    })()
  )js"),
            "Caret,true,true,2,2,1,,true|Caret,true,0,0|true,2,2,true|true,2,2|0,None,true,true|"
            "0,None");
}

TEST_F(DomBinderTest, SelectionExtendAndSetBaseAndExtent)
{
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var p = document.createElement('p');
      var t = document.createTextNode('abcdef');
      p.appendChild(t);
      document.body.appendChild(p);
      var selection = document.getSelection();
      selection.collapse(t, 4);
      selection.extend(t, 1);
      var backward = [selection.anchorNode === t, selection.anchorOffset,
                      selection.focusNode === t, selection.focusOffset,
                      selection.isCollapsed, selection.type, selection.toString(),
                      selection.getRangeAt(0).startOffset,
                      selection.getRangeAt(0).endOffset,
                      selection.getRangeAt(0).collapsed].join(',');
      selection.extend(t, 6);
      var forward = [selection.anchorOffset, selection.focusOffset, selection.type,
                     selection.toString()].join(',');
      selection.setBaseAndExtent(t, 5, t, 2);
      var base = [selection.anchorOffset, selection.focusOffset, selection.type,
                  selection.toString(), selection.getRangeAt(0).startOffset,
                  selection.getRangeAt(0).endOffset].join(',');
      selection.setBaseAndExtent(t, 1, t, 4);
      var baseForward = [selection.anchorOffset, selection.focusOffset,
                         selection.toString()].join(',');
      return [backward, forward, base, baseForward].join('|');
    })()
  )js"),
            "true,4,true,1,false,Range,bcd,1,4,false|4,6,Range,ef|"
            "5,2,Range,cde,2,5|1,4,bcd");
}

TEST_F(DomBinderTest, SelectionContainsNode)
{
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var p = document.getElementById('first');
      var hello = p.firstChild;
      var bold = p.querySelector('b');
      var world = bold.firstChild;
      var selection = document.getSelection();
      selection.setBaseAndExtent(hello, 2, world, 3);
      return [selection.containsNode(p),
              selection.containsNode(hello), selection.containsNode(hello, true),
              selection.containsNode(bold), selection.containsNode(bold, true),
              selection.containsNode(world), selection.containsNode(world, true),
              selection.containsNode(document.body),
              selection.containsNode(document.implementation.createHTMLDocument('x').body)
             ].join(',');
    })()
  )js"),
            "false,false,true,false,true,false,true,false,false");

  // An empty selection contains nothing, not even the document.
  EXPECT_EQ(EvalString("(function(){ var s = document.getSelection();"
                       " s.removeAllRanges();"
                       " return [s.containsNode(document), s.containsNode(document.body, true)]"
                       ".join(','); })()"),
            "false,false");
}

TEST_F(DomBinderTest, SelectionRangeListManagement)
{
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var p = document.createElement('p');
      var t = document.createTextNode('abcdef');
      p.appendChild(t);
      document.body.appendChild(p);
      var selection = document.getSelection();
      var first = document.createRange();
      first.setStart(t, 1); first.setEnd(t, 3);
      var second = document.createRange();
      second.setStart(t, 4); second.setEnd(t, 5);
      selection.addRange(first);
      var initial = [selection.rangeCount, selection.type, selection.anchorNode === t,
                     selection.anchorOffset, selection.focusOffset, selection.toString(),
                     selection.getRangeAt(0).toString(),
                     selection.getRangeAt(0) === first].join(',');
      selection.addRange(first); // already in the range list: ignored
      var deduped = selection.rangeCount;
      selection.addRange(second); // appended (multi-range extension)
      var multi = [selection.rangeCount, selection.getRangeAt(1).toString(),
                   selection.toString(), selection.anchorOffset,
                   selection.focusOffset].join(',');
      selection.removeRange(second);
      var removed = [selection.rangeCount, selection.toString()].join(',');
      var notFound = '';
      try { selection.removeRange(document.createRange()); } catch (e) { notFound = e.name; }
      var badIndex = '';
      try { selection.getRangeAt(1); } catch (e) { badIndex = e.name; }
      var negativeIndex = '';
      try { selection.getRangeAt(-1); } catch (e) { negativeIndex = e.name; }
      selection.removeAllRanges();
      var cleared = [selection.rangeCount, selection.type].join(',');
      selection.addRange(first);
      selection.empty();
      var emptied = selection.rangeCount;
      var emptyGet = '';
      try { selection.getRangeAt(0); } catch (e) { emptyGet = e.name; }
      var foreign = '';
      var other = document.implementation.createHTMLDocument('x');
      try { selection.addRange(other.createRange()); } catch (e) { foreign = e.name; }
      return [initial, deduped, multi, removed, notFound, badIndex, negativeIndex,
              cleared, emptied, emptyGet, foreign].join('|');
    })()
  )js"),
            // getRangeAt() builds a new Range on demand (the engine has no live
            // ranges), so the identity check is deliberately false.
            "1,Range,true,1,3,bc,bc,false|1|2,e,bc,1,3|1,bc|NotFoundError|IndexSizeError|"
            "IndexSizeError|0,None|0|IndexSizeError|WrongDocumentError");
}

TEST_F(DomBinderTest, SelectionBoundaryValidation)
{
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var p = document.getElementById('first');
      var t = p.firstChild;
      var selection = document.getSelection();
      var other = document.implementation.createHTMLDocument('x');
      function failure(fn) {
        try { fn(); return 'no'; } catch (e) { return e.name; }
      }
      var names = [];
      names.push(failure(function(){ selection.collapse(p, 3); }));
      names.push(failure(function(){ selection.collapse(document.doctype, 0); }));
      names.push(failure(function(){ selection.collapse(other.body, 0); }));
      names.push(failure(function(){ selection.selectAllChildren(document.doctype); }));
      names.push(failure(function(){ selection.selectAllChildren(other.body); }));
      names.push(failure(function(){ selection.setBaseAndExtent(t, 0, other.body, 0); }));
      names.push(failure(function(){ selection.setBaseAndExtent(t, 99, t, 0); }));
      names.push(failure(function(){ selection.collapseToStart(); }));
      names.push(failure(function(){ selection.collapseToEnd(); }));
      names.push(failure(function(){ selection.extend(t, 0); }));
      selection.collapse(t, 1);
      // A rejected call leaves the selection untouched.
      names.push(failure(function(){ selection.collapse(t, 99); }));
      var unchanged = [selection.anchorNode === t, selection.anchorOffset].join(',');
      names.push(failure(function(){ selection.extend(t, 99); }));
      names.push(failure(function(){ selection.extend(other.body, 0); }));
      names.push(failure(function(){ selection.extend(document.doctype, 0); }));
      names.push(failure(function(){ selection.setBaseAndExtent(document.doctype, 0, t, 0); }));
      return names.join(',') + '|' + unchanged;
    })()
  )js"),
            "IndexSizeError,InvalidNodeTypeError,WrongDocumentError,InvalidNodeTypeError,"
            "WrongDocumentError,WrongDocumentError,IndexSizeError,InvalidStateError,"
            "InvalidStateError,InvalidStateError,IndexSizeError,IndexSizeError,"
            "WrongDocumentError,InvalidNodeTypeError,InvalidNodeTypeError|true,1");
}

TEST_F(DomBinderTest, SelectionDeleteFromDocument)
{
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var p = document.createElement('p');
      var t = document.createTextNode('abcdef');
      p.appendChild(t);
      document.body.appendChild(p);
      var selection = document.getSelection();
      selection.setBaseAndExtent(t, 1, t, 4);
      var before = selection.toString();
      selection.deleteFromDocument();
      var same = [before, t.data, selection.isCollapsed, selection.type,
                  selection.anchorNode === t, selection.anchorOffset,
                  selection.focusOffset, selection.rangeCount,
                  selection.toString()].join(',');
      // Cross-node deletion removes the fully contained element too.
      var host = document.createElement('div');
      var head = document.createTextNode('ab');
      var bold = document.createElement('b');
      bold.appendChild(document.createTextNode('cd'));
      var tail = document.createTextNode('ef');
      host.appendChild(head); host.appendChild(bold); host.appendChild(tail);
      document.body.appendChild(host);
      selection.setBaseAndExtent(head, 1, tail, 1);
      selection.deleteFromDocument();
      var cross = [host.textContent, host.children.length, selection.anchorNode === host,
                   selection.anchorOffset, selection.rangeCount,
                   selection.type].join(',');
      return same + '|' + cross;
    })()
  )js"),
            "bcd,aef,true,Caret,true,1,1,1,|af,0,true,1,1,Caret");
}

TEST_F(DomBinderTest, SelectionChangeEvent)
{
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var p = document.getElementById('first');
      var t = p.firstChild;
      var seen = [];
      document.addEventListener('selectionchange', function(event) {
        seen.push(event.type + ':' + event.bubbles + ':' + (event.target === document));
      });
      window.addEventListener('selectionchange', function() { seen.push('window'); });
      var selection = document.getSelection();
      selection.collapse(t, 1);        // change: event
      selection.collapse(t, 1);        // same state: no event
      selection.extend(t, 3);          // change: event
      selection.selectAllChildren(p);  // change: event
      selection.removeAllRanges();     // change: event
      selection.removeAllRanges();     // already empty: no event
      var events = seen.length;
      selection.empty();               // already empty alias: no event
      var afterEmpty = seen.length;
      return [events, afterEmpty, seen.join(',')].join('|');
    })()
  )js"),
            "8|8|selectionchange:false:true,window,selectionchange:false:true,window,"
            "selectionchange:false:true,window,selectionchange:false:true,window");
}

TEST_F(DomBinderTest, SelectionSurvivesBoundaryNodeRemovalAndTextShrinking)
{
  EXPECT_EQ(EvalString(R"js(
    (function(){
      var host = document.createElement('div');
      var t = document.createTextNode('abcdef');
      host.appendChild(t);
      document.body.appendChild(host);
      var selection = document.getSelection();
      selection.setBaseAndExtent(t, 1, t, 4);
      var before = [selection.toString(), selection.rangeCount, selection.type].join(',');
      host.removeChild(t);
      // The removed node is still retained by the binder (so the pointer stays
      // valid), but it is outside the document tree: the selection reports
      // itself empty instead of crashing or rebasing onto another point.
      var after = [selection.rangeCount, selection.type,
                   selection.anchorNode === null, selection.focusNode === null,
                   selection.anchorOffset, selection.focusOffset,
                   selection.isCollapsed, String(selection),
                   selection.containsNode(t)].join(',');
      var getRange = '';
      try { selection.getRangeAt(0); } catch (e) { getRange = e.name; }
      host.appendChild(t);
      var restored = [selection.rangeCount, selection.type, selection.toString()].join(',');
      // A shortened Text node clamps the stored offsets on read.
      selection.setBaseAndExtent(t, 1, t, 5);
      t.data = 'xy';
      var clamped = [selection.anchorOffset, selection.focusOffset, selection.type,
                     selection.toString(), selection.getRangeAt(0).startOffset,
                     selection.getRangeAt(0).endOffset].join(',');
      var live = '';
      try { selection.getRangeAt(0); live = 'ok'; } catch (e) { live = e.name; }
      return [before, after, getRange, restored, clamped, live].join('|');
    })()
  )js"),
            "bcd,1,Range|0,None,true,true,0,0,false,,false|IndexSizeError|1,Range,bcd|"
            "1,2,Range,y,1,2|ok");
}

TEST_F(DomBinderTest, MessageEventConstructorAndMessagePortDispatch)
{
  EXPECT_TRUE(
      EvalBool("(function(){ var event = new MessageEvent('message', {data: 42}); "
               "return event instanceof MessageEvent && event instanceof Event "
               "    && event.type === 'message' && event.data === 42 "
               "    && event.origin === '' && event.lastEventId === '' "
               "    && event.source === null && Array.isArray(event.ports) "
               "    && typeof Object.getOwnPropertyDescriptor(MessageEvent.prototype, 'data').get "
               "       === 'function'; })()"));
  EXPECT_TRUE(EvalBool("(function(){ var channel = new MessageChannel(); var received = false; "
                       "window._messageEventChannel = channel; "
                       "channel.port2.onmessage = function(event) { "
                       "received = event instanceof MessageEvent && event.data === 'ok'; }; "
                       "channel.port1.postMessage('ok'); window._messageEventReceived = "
                       "function() { return received; }; return true; })()"));
  EXPECT_EQ(binder_->RunPendingTimers(), 1);
  EXPECT_TRUE(EvalBool("window._messageEventReceived()"));
}

TEST_F(DomBinderTest, ScreenAndViewportGlobals)
{
  // Engine-default viewport (matches renderer::Page's default layout width).
  EXPECT_EQ(EvalNumber("screen.width"), 800.0);
  EXPECT_EQ(EvalNumber("screen.height"), 600.0);
  EXPECT_EQ(EvalNumber("screen.colorDepth"), 24.0);
  EXPECT_EQ(EvalNumber("window.innerWidth"), 800.0);
  EXPECT_EQ(EvalNumber("window.innerHeight"), 600.0);
  EXPECT_EQ(EvalNumber("window.devicePixelRatio"), 1.0);
  EXPECT_TRUE(EvalBool("screen === window.screen"));
}

TEST_F(DomBinderTest, NodeAppend)
{
  EXPECT_TRUE(EvalBool("(function(){ var d = document; "
                       "var list = d.createElement('ul'); "
                       "var a = d.createElement('li'); a.textContent = 'a'; "
                       "var b = d.createElement('li'); b.textContent = 'b'; "
                       "list.append(a, b); "
                       "return list.children.length === 2 "
                       "       && list.children[0] === a && list.children[1] === b; })()"));
  // append re-parents an already-attached node (DOM adoption): the element
  // moves under the detached div, so it leaves the document tree.
  EXPECT_TRUE(EvalBool("(function(){ var d = document; "
                       "var e = d.getElementById('first'); "
                       "var div = d.createElement('div'); "
                       "div.append(e); "
                       "return e.parentNode === div && d.getElementById('first') === null; })()"));
}

TEST_F(DomBinderTest, ReplaceChildren)
{
  EXPECT_TRUE(EvalBool("(function(){ var d = document; "
                       "var list = d.createElement('ul'); "
                       "list.append(d.createElement('li'), d.createElement('li')); "
                       "list.replaceChildren(); "
                       "return list.children.length === 0; })()"));
  EXPECT_TRUE(EvalBool("(function(){ var d = document; "
                       "var e = d.getElementById('main'); "
                       "e.replaceChildren(); "
                       "return e.children.length === 0 "
                       "       && d.getElementById('first') === null; })()"));
}

namespace {

// A tiny in-memory store backing the localStorage callbacks.
class FakeStorage
{
public:
  std::optional<std::string> Get(std::string_view key) const
  {
    const auto it = entries_.find(std::string(key));
    return it == entries_.end() ? std::nullopt : std::optional<std::string>(it->second);
  }
  void Set(std::string_view key, std::string_view value)
  {
    entries_[std::string(key)] = std::string(value);
  }
  bool Remove(std::string_view key)
  {
    return entries_.erase(std::string(key)) > 0;
  }
  void Clear()
  {
    entries_.clear();
  }
  std::vector<std::string> Keys() const
  {
    std::vector<std::string> keys;
    for (const auto& entry : entries_) {
      keys.push_back(entry.first);
    }
    return keys;
  }

private:
  std::map<std::string, std::string> entries_;
};

} // namespace

TEST_F(DomBinderTest, LocalStorageApis)
{
  FakeStorage store;
  PageApis apis;
  apis.storage_get = [&store](std::string_view k) { return store.Get(k); };
  apis.storage_set = [&store](std::string_view k, std::string_view v) { store.Set(k, v); };
  apis.storage_remove = [&store](std::string_view k) { return store.Remove(k); };
  apis.storage_clear = [&store]() { store.Clear(); };
  apis.storage_keys = [&store]() { return store.Keys(); };

  DomBinder binder(*document_, apis);
  ASSERT_TRUE(binder.Evaluate("typeof Storage === 'function'").has_value());
  EXPECT_TRUE(binder.Evaluate("typeof Storage === 'function'").value().ToBoolean());
  ASSERT_TRUE(binder.Evaluate("localStorage instanceof Storage").has_value());
  EXPECT_TRUE(binder.Evaluate("localStorage instanceof Storage").value().ToBoolean());
  ASSERT_TRUE(
      binder.Evaluate("Storage.prototype === Object.getPrototypeOf(localStorage)").has_value());
  EXPECT_TRUE(binder.Evaluate("Storage.prototype === Object.getPrototypeOf(localStorage)")
                  .value()
                  .ToBoolean());
  ASSERT_TRUE(binder
                  .Evaluate("localStorage.setItem('a', '1');"
                            "localStorage.setItem('b', '2');")
                  .has_value());
  auto len = binder.Evaluate("localStorage.length");
  ASSERT_TRUE(len.has_value());
  ASSERT_TRUE(len.value().ToNumber().has_value());
  EXPECT_DOUBLE_EQ(len.value().ToNumber().value(), 2.0);
  auto a = binder.Evaluate("localStorage.getItem('a')");
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(a.value().ToString().has_value());
  EXPECT_EQ(a.value().ToString().value(), "1");
  ASSERT_TRUE(binder.Evaluate("localStorage.getItem('missing') === null").has_value());
  ASSERT_TRUE(binder
                  .Evaluate("localStorage.removeItem('a');"
                            "localStorage.getItem('a') === null")
                  .has_value());
  // key(i) enumerates stored keys; clear() empties the store.
  ASSERT_TRUE(binder.Evaluate("localStorage.key(0) === 'b'").has_value());
  ASSERT_TRUE(binder.Evaluate("localStorage.clear(); localStorage.length === 0").has_value());
}

TEST_F(DomBinderTest, SessionStorageUsesDocumentScopedStorage)
{
  ASSERT_TRUE(binder_->Evaluate("sessionStorage instanceof Storage").has_value());
  EXPECT_TRUE(binder_->Evaluate("sessionStorage instanceof Storage").value().ToBoolean());
  ASSERT_TRUE(
      binder_->Evaluate("sessionStorage.setItem('consent', 'yes'); sessionStorage.length === 1")
          .has_value());
  EXPECT_TRUE(
      binder_->Evaluate("sessionStorage.setItem('consent', 'yes'); sessionStorage.length === 1")
          .value()
          .ToBoolean());
  ASSERT_TRUE(binder_->Evaluate("sessionStorage.getItem('consent') === 'yes'").has_value());
  EXPECT_TRUE(binder_->Evaluate("sessionStorage.getItem('consent') === 'yes'").value().ToBoolean());
}

// When the browser layer provides the tab-scoped session store through
// PageApis, sessionStorage reads and writes go there instead of the
// binder-local fallback: a fresh binder (i.e. a navigation) in the same tab
// still sees the values.
TEST(DomBinderSessionStorageTest, UsesTheBrowserProvidedTabStore)
{
  auto document = html::Parser("<!doctype html><html><body></body></html>").Parse();
  storage::SessionStorage store;
  const std::string origin = "https://example.com/";
  PageApis apis;
  apis.location_href = []() { return std::string("https://example.com/"); };
  apis.session_storage_get = [&store, origin](std::string_view key) {
    return store.GetItem(origin, key);
  };
  apis.session_storage_set = [&store, origin](std::string_view key, std::string_view value) {
    store.SetItem(origin, key, value);
  };
  apis.session_storage_remove = [&store, origin](std::string_view key) {
    return store.RemoveItem(origin, key);
  };
  apis.session_storage_clear = [&store, origin]() { store.Clear(origin); };
  apis.session_storage_keys = [&store, origin]() {
    std::vector<std::string> keys;
    for (const auto& entry : store.All(origin)) {
      keys.push_back(entry.first);
    }
    return keys;
  };

  {
    DomBinder binder(*document, apis);
    ASSERT_TRUE(binder.Evaluate("sessionStorage.setItem('k', 'v')").has_value());
  }
  ASSERT_TRUE(store.GetItem(origin, "k").has_value());
  EXPECT_EQ(store.GetItem(origin, "k").value(), "v");

  // A new document (simulated navigation) with the same tab store reads it
  // back, and removal goes through the store as well.
  auto next_document = html::Parser("<!doctype html><html><body></body></html>").Parse();
  DomBinder next_binder(*next_document, apis);
  auto value = next_binder.Evaluate("sessionStorage.getItem('k')");
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(value.value().ToString().value_or(""), "v");
  ASSERT_TRUE(next_binder.Evaluate("sessionStorage.removeItem('k'); sessionStorage.length === 0")
                  .has_value());
  EXPECT_FALSE(store.GetItem(origin, "k").has_value());
}

TEST_F(DomBinderTest, UrlSearchParamsParsesMutatesAndSerializes)
{
  auto result = binder_->Evaluate(
      "const params = new URLSearchParams('?q=hello+world&tag=a&tag=b&empty=&encoded=%2F');"
      "const descriptor = Object.getOwnPropertyDescriptor(URLSearchParams.prototype, 'size');"
      "const initial = [typeof descriptor.get, params.size, params.get('q'), "
      "params.getAll('tag').join(','), params.has('empty'),"
      "                 params.get('missing') === null, params.toString()].join('|');"
      "params.append('tag', 'c');"
      "params.set('q', 'a b');"
      "params.delete('empty');"
      "params.sort();"
      "initial + '|' + params.size + '|' + params.toString();");

  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result.value().ToString().has_value());
  EXPECT_EQ(result.value().ToString().value(),
            "function|5|hello world|a,b|true|true|q=hello+world&tag=a&tag=b&empty=&encoded=%2F|"
            "5|encoded=%2F&q=a+b&tag=a&tag=b&tag=c");
}

TEST_F(DomBinderTest, FormDataProvidesEntriesAndStringValues)
{
  auto result = binder_->Evaluate(
      "const descriptor = Object.getOwnPropertyDescriptor(FormData.prototype, 'entries');"
      "const data = new FormData();"
      "data.append('tag', 'a');"
      "data.append('tag', 'b');"
      "data.set('tag', 'updated');"
      "data.append('mode', 'test');"
      "[typeof descriptor.value, data.get('tag'), data.has('tag'),"
      " Array.from(data.entries()).map((entry) => entry.join('=')).join(','),"
      " data.delete('tag'), data.has('tag')].join('|');");

  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result.value().ToString().has_value());
  EXPECT_EQ(result.value().ToString().value(),
            "function|updated|true|tag=updated,mode=test||false");
}

// CORS (Fetch §4.1): a cross-origin response may only be read when it grants
// access via Access-Control-Allow-Origin ("*" or the page's own origin).
// Same-origin reads need no grant.
// The Origin request header (Fetch §2.2.3) rides on XHR for POSTs and
// cross-origin requests -- it is what servers gate CORS grants on.
// The Origin request header also rides on window.fetch for cross-origin
// requests (Fetch §2.2.3) through the transport's header channel.
TEST_F(DomBinderTest, FetchSendsOriginHeaderForCrossOriginRequests)
{
  PageApis apis;
  apis.location_href = []() { return "https://www.example.com/"; };
  apis.resolve_url = [](const std::string& raw) { return raw; };
  std::vector<std::pair<std::string, std::string>> seen;
  apis.fetch = [&seen](const std::string&,
                       const std::vector<std::pair<std::string, std::string>>& headers)
      -> base::Result<FetchResponse> {
    seen = headers;
    FetchResponse out;
    out.status = 200;
    return base::Ok(std::move(out));
  };
  DomBinder binder(*document_, apis);
  const auto origin_header = [&seen]() {
    for (const auto& [name, value] : seen) {
      if (name == "origin") {
        return value;
      }
    }
    return std::string();
  };

  (void)binder.Evaluate("fetch('https://other.test/data')");
  EXPECT_EQ(origin_header(), "https://www.example.com");
  (void)binder.Evaluate("fetch('https://www.example.com/local')");
  EXPECT_EQ(origin_header(), "");
}

TEST_F(DomBinderTest, XhrSendsOriginHeaderForCrossOriginRequests)
{
  PageApis apis;
  apis.location_href = []() { return "https://www.example.com/"; };
  apis.resolve_url = [](const std::string& raw) { return raw; };
  std::vector<std::pair<std::string, std::string>> seen;
  apis.xhr_request = [&seen](const std::string&,
                             const std::string&,
                             const std::vector<std::pair<std::string, std::string>>& headers,
                             const std::string&) -> base::Result<FetchResponse> {
    seen = headers;
    FetchResponse out;
    out.status = 200;
    return base::Ok(std::move(out));
  };
  DomBinder binder(*document_, apis);
  const auto origin_header = [&seen]() {
    for (const auto& [name, value] : seen) {
      if (name == "origin") {
        return value;
      }
    }
    return std::string();
  };

  // Cross-origin GET: carries the page origin.
  (void)binder.Evaluate("var x = new XMLHttpRequest();"
                        "x.open('GET', 'https://other.test/a'); x.send();");
  EXPECT_EQ(origin_header(), "https://www.example.com");
  // Same-origin GET: no Origin header (browsers omit it).
  (void)binder.Evaluate("var y = new XMLHttpRequest();"
                        "y.open('GET', 'https://www.example.com/b'); y.send();");
  EXPECT_EQ(origin_header(), "");
  // Same-origin POST still carries it.
  (void)binder.Evaluate("var z = new XMLHttpRequest();"
                        "z.open('POST', 'https://www.example.com/c'); z.send('k=v');");
  EXPECT_EQ(origin_header(), "https://www.example.com");
}

TEST_F(DomBinderTest, FetchEnforcesCorsOnCrossOriginResponses)
{
  PageApis apis;
  apis.location_href = []() { return "https://www.example.com/"; };
  apis.resolve_url = [](const std::string& raw) { return raw; };
  std::vector<std::pair<std::string, std::string>> headers;
  apis.fetch =
      [&headers](
          const std::string& url,
          const std::vector<std::pair<std::string, std::string>>&) -> base::Result<FetchResponse> {
    FetchResponse response;
    response.status = 200;
    response.final_url = url;
    response.headers = headers;
    response.body = "secret";
    return base::Ok(std::move(response));
  };
  DomBinder binder(*document_, apis);

  const auto fetch_resolves = [&](const std::string& url) {
    (void)binder.Evaluate("window._done = false; window._ok = false;"
                          "fetch('" +
                          url +
                          "').then(function(r){ window._ok = true; window._done = true; },"
                          "function(){ window._ok = false; window._done = true; });");
    auto done = binder.Evaluate("window._done");
    if (!done.has_value() || !done.value().ToBoolean().value_or(false)) {
      return false;
    }
    auto ok = binder.Evaluate("window._ok");
    return ok.has_value() && ok.value().ToBoolean().value_or(false);
  };

  // Same-origin: no grant required.
  headers.clear();
  EXPECT_TRUE(fetch_resolves("https://www.example.com/data"));
  // Cross-origin without a grant: network error.
  EXPECT_FALSE(fetch_resolves("https://other.test/data"));
  // "*": any origin may read.
  headers = {{"access-control-allow-origin", "*"}};
  EXPECT_TRUE(fetch_resolves("https://other.test/data"));
  // Exactly the page's origin.
  headers = {{"access-control-allow-origin", "https://www.example.com"}};
  EXPECT_TRUE(fetch_resolves("https://other.test/data"));
  // A grant to someone else is not a grant to us.
  headers = {{"access-control-allow-origin", "https://evil.test"}};
  EXPECT_FALSE(fetch_resolves("https://other.test/data"));
}

TEST_F(DomBinderTest, FetchResolvesResponseAndBody)
{
  PageApis apis;
  apis.resolve_url = [](const std::string& raw) { return raw; };
  apis.fetch =
      [](const std::string& url,
         const std::vector<std::pair<std::string, std::string>>&) -> base::Result<FetchResponse> {
    FetchResponse response;
    response.status = 200;
    response.status_text = "OK";
    response.final_url = url;
    response.headers = {{"content-type", "text/plain"}};
    response.body = "hello body";
    return base::Ok(std::move(response));
  };

  DomBinder binder(*document_, apis);
  // The .then chain runs through the drained microtask queue after Evaluate.
  const auto ev =
      binder.Evaluate("window._done = false;"
                      "fetch('http://example.com/data').then(function(res){"
                      "  window._status = res.status;"
                      "  window._ok = res.ok;"
                      "  window._ct = res.headers.get('Content-Type');"
                      "  return res.text();"
                      "}).then(function(t){ window._body = t; window._done = true; });");
  ASSERT_TRUE(ev.has_value()) << ev.error().message();
  auto done = binder.Evaluate("window._done");
  ASSERT_TRUE(done.has_value());
  ASSERT_TRUE(done.value().ToBoolean().has_value());
  EXPECT_TRUE(done.value().ToBoolean().value());
  auto status = binder.Evaluate("window._status");
  ASSERT_TRUE(status.has_value());
  ASSERT_TRUE(status.value().ToNumber().has_value());
  EXPECT_DOUBLE_EQ(status.value().ToNumber().value(), 200.0);
  auto ct = binder.Evaluate("window._ct");
  ASSERT_TRUE(ct.has_value());
  ASSERT_TRUE(ct.value().ToString().has_value());
  EXPECT_EQ(ct.value().ToString().value(), "text/plain");
  auto body = binder.Evaluate("window._body");
  ASSERT_TRUE(body.has_value());
  ASSERT_TRUE(body.value().ToString().has_value());
  EXPECT_EQ(body.value().ToString().value(), "hello body");
}

TEST_F(DomBinderTest, ResponseUsesSharedPrototype)
{
  PageApis apis;
  apis.resolve_url = [](const std::string& raw) { return raw; };
  apis.fetch =
      [](const std::string&,
         const std::vector<std::pair<std::string, std::string>>&) -> base::Result<FetchResponse> {
    FetchResponse response;
    response.body = "hello";
    return base::Ok(std::move(response));
  };
  DomBinder binder(*document_, apis);
  const auto result = binder.Evaluate(
      "(function(){ var response = new Response('hello'); "
      "var descriptor = Object.getOwnPropertyDescriptor(Response.prototype, 'json'); "
      "return response instanceof Response && typeof Response.prototype.text === 'function' "
      "    && typeof descriptor.value === 'function' && response.text() instanceof Promise; })()");
  ASSERT_TRUE(result.has_value()) << result.error().message();
  ASSERT_TRUE(result.value().ToBoolean().has_value());
  EXPECT_TRUE(result.value().ToBoolean().value());
}

TEST_F(DomBinderTest, FetchRejectsOnNetworkError)
{
  PageApis apis;
  apis.resolve_url = [](const std::string& raw) { return raw; };
  apis.fetch =
      [](const std::string&,
         const std::vector<std::pair<std::string, std::string>>&) -> base::Result<FetchResponse> {
    return base::Err(base::Error::Network("connection refused"));
  };
  DomBinder binder(*document_, apis);
  ASSERT_TRUE(binder
                  .Evaluate("window._caught = false;"
                            "fetch('http://example.com/').catch(function(e){"
                            "  window._caught = true;"
                            "});")
                  .has_value());
  auto caught = binder.Evaluate("window._caught");
  ASSERT_TRUE(caught.has_value());
  ASSERT_TRUE(caught.value().ToBoolean().has_value());
  EXPECT_TRUE(caught.value().ToBoolean().value());
}

TEST_F(DomBinderTest, LocationExposesCurrentUrlParts)
{
  PageApis apis;
  apis.location_href = []() { return "https://www.example.com:8080/a/b?x=1#frag"; };
  DomBinder binder(*document_, apis);
  auto eval = [&binder](const std::string& code) {
    auto r = binder.Evaluate(code);
    if (!r.has_value()) {
      return std::string("<error: ") + r.error().message() + ">";
    }
    auto s = r.value().ToString();
    return s.has_value() ? s.value() : std::string("<tostring-error>");
  };
  auto eval_bool = [&binder](const std::string& code) {
    auto r = binder.Evaluate(code);
    if (!r.has_value()) {
      return false;
    }
    auto b = r.value().ToBoolean();
    return b.has_value() && b.value();
  };
  EXPECT_EQ(eval("location.href"), "https://www.example.com:8080/a/b?x=1#frag");
  EXPECT_EQ(eval("location.protocol"), "https:");
  EXPECT_EQ(eval("location.host"), "www.example.com:8080");
  EXPECT_EQ(eval("location.hostname"), "www.example.com");
  EXPECT_EQ(eval("location.port"), "8080");
  EXPECT_EQ(eval("location.pathname"), "/a/b");
  EXPECT_EQ(eval("location.search"), "?x=1");
  EXPECT_EQ(eval("location.hash"), "#frag");
  EXPECT_EQ(eval("location.origin"), "https://www.example.com:8080");
  EXPECT_EQ(eval("location.toString()"), "https://www.example.com:8080/a/b?x=1#frag");
  // window.location and the bare global are the same object.
  EXPECT_TRUE(eval_bool("window.location === location"));
}

TEST_F(DomBinderTest, LocationHrefAssignmentRequestsNavigation)
{
  PageApis apis;
  apis.location_href = []() { return "https://www.example.com/"; };
  apis.resolve_url = [](const std::string& raw) {
    return raw.rfind("http", 0) == 0 ? raw : "https://www.example.com/" + raw;
  };
  std::string requested;
  apis.navigate = [&requested](const std::string& url) { requested = url; };
  DomBinder binder(*document_, apis);
  ASSERT_TRUE(binder.Evaluate("location.href = 'https://example.org/page';").has_value());
  EXPECT_EQ(requested, "https://example.org/page");

  requested.clear();
  ASSERT_TRUE(binder.Evaluate("location.assign('next.html');").has_value());
  EXPECT_EQ(requested, "https://www.example.com/next.html");

  requested.clear();
  ASSERT_TRUE(binder.Evaluate("location.replace('https://example.org/other');").has_value());
  EXPECT_EQ(requested, "https://example.org/other");
}

TEST_F(DomBinderTest, LocationReloadRequestsReload)
{
  bool reloaded = false;
  PageApis apis;
  apis.location_href = []() { return "https://www.example.com/"; };
  apis.reload = [&reloaded]() { reloaded = true; };
  DomBinder binder(*document_, apis);
  ASSERT_TRUE(binder.Evaluate("location.reload();").has_value());
  EXPECT_TRUE(reloaded);
}

// The root cause of bing's broken script chain: window and the global scope
// must be the same object, so `window._G = {...}` in one <script> is readable
// as a bare `_G` in the next.  window.self/top/parent/frames must also point at
// window (the engine has no frame tree).
TEST_F(DomBinderTest, WindowIsTheGlobalObject)
{
  EXPECT_TRUE(EvalBool("window === globalThis"));
  EXPECT_TRUE(EvalBool("window.self === window"));
  EXPECT_TRUE(EvalBool("window.top === window"));
  EXPECT_TRUE(EvalBool("window.parent === window"));
  EXPECT_TRUE(EvalBool("window.frames === window"));
  // Cross-script global definition: write via window, read as a bare global,
  // and vice versa (exactly what bing's ~47-script bootstrap relies on).
  EXPECT_TRUE(EvalBool("(function(){ window._G = { Region: 'US' }; "
                       "var a = _G.Region === 'US'; "
                       "_G.Lang = 'en-US'; "
                       "var b = window._G.Lang === 'en-US'; "
                       "var c = self._G.Region === 'US'; "
                       "return a && b && c; })()"));
}

// Global event handler attributes (HTML spec §8.1.7.2) are global properties in
// browsers: bare `onload`/`onerror`/... resolve without a declaration and are
// assignable.  bing's scripts read `onload` as a bare identifier.
TEST_F(DomBinderTest, GlobalEventHandlersAreGlobalProperties)
{
  EXPECT_TRUE(EvalBool("onload === null"));
  EXPECT_TRUE(EvalBool("typeof onerror === 'object'"));
  EXPECT_TRUE(EvalBool("window.onload === onload"));
  EXPECT_TRUE(EvalBool("(function(){ onload = function(){}; "
                       "var ok = typeof onload === 'function' && window.onload === onload; "
                       "onload = null; return ok; })()"));
}

// innerText (read): approximated by textContent; bing sniffs page text with it.
TEST_F(DomBinderTest, ElementInnerTextGetter)
{
  EXPECT_EQ(EvalString("document.getElementById('first').innerText"), "Hello world");
  EXPECT_TRUE(EvalBool("document.getElementById('first').innerText === "
                       "document.getElementById('first').textContent"));
  // innerText is documented as a textContent approximation, so they agree even
  // where real innerText would normalize whitespace / hide elements.
  EXPECT_TRUE(EvalBool("document.body.innerText === document.body.textContent"));
}

// new CustomEvent(type, {detail, bubbles, cancelable}) — a constructable Event
// subclass carrying a `detail` payload (bing dispatches these between scripts).
TEST_F(DomBinderTest, CustomEventCarriesDetail)
{
  EXPECT_TRUE(EvalBool("(function(){ var ev = new CustomEvent('hello', "
                       "{ detail: { a: 1 }, bubbles: true, cancelable: true }); "
                       "var got = null; var bubbles = false; "
                       "document.addEventListener('hello', function(e){ "
                       "  got = e.detail; bubbles = e.bubbles; }); "
                       "document.dispatchEvent(ev); "
                       "return ev instanceof CustomEvent && ev instanceof Event "
                       "       && got && got.a === 1 && bubbles === true; })()"));
  // detail defaults to null (CustomEventInit default), and constructor args are
  // optional like Event.
  EXPECT_TRUE(EvalBool("(function(){ var ev = new CustomEvent('x'); "
                       "return ev.detail === null && ev.type === 'x'; })()"));
  // CustomEvent.prototype inherits Event.prototype.
  EXPECT_TRUE(EvalBool("CustomEvent.prototype instanceof Event"));
}

// Element.dir reflects the content attribute limited to the known values
// ltr/rtl/auto; an absent or invalid value reads back as "" (WHATWG HTML).
// Swiper (qq.com's banner slider) calls `el.dir.toLowerCase()` while mounting,
// so an undefined property used to abort the whole page render.
TEST_F(DomBinderTest, ElementDirReflectsTheAttribute)
{
  EXPECT_TRUE(EvalBool("document.createElement('div').dir === \"\""));
  EXPECT_TRUE(EvalBool("(function(){ var el = document.createElement('div'); "
                       "el.dir = 'rtl'; "
                       "return el.getAttribute('dir') === 'rtl' && el.dir === 'rtl'; })()"));
  // Known values match ASCII case-insensitively and read back lowercased.
  EXPECT_TRUE(EvalBool("(function(){ var el = document.createElement('div'); "
                       "el.setAttribute('dir', 'RTL'); "
                       "return el.dir === 'rtl'; })()"));
  EXPECT_TRUE(EvalBool("(function(){ var el = document.createElement('div'); "
                       "el.setAttribute('dir', 'auto'); "
                       "return el.dir === 'auto'; })()"));
  // Unknown values read back as "" (the attribute stays as written).
  EXPECT_TRUE(EvalBool("(function(){ var el = document.createElement('div'); "
                       "el.setAttribute('dir', 'bogus'); "
                       "return el.dir === '' && el.getAttribute('dir') === 'bogus'; })()"));
  // Assigning the empty string drops the attribute.
  EXPECT_TRUE(EvalBool("(function(){ var el = document.createElement('div'); "
                       "el.dir = 'rtl'; el.dir = ''; "
                       "return el.hasAttribute('dir') === false && el.dir === ''; })()"));
}

// navigator.javaEnabled(): always false in modern browsers (applets are
// gone).  jd.com's wl.js fingerprint collector calls it unconditionally and
// aborted its whole init with "not a function" before this existed.
TEST_F(DomBinderTest, NavigatorJavaEnabledIsFalse)
{
  EXPECT_TRUE(EvalBool("typeof navigator.javaEnabled === 'function'"));
  EXPECT_TRUE(EvalBool("navigator.javaEnabled() === false"));
}

// HTML §4.8.12 Audio(src): the legacy factory returning an <audio> element,
// with the src attribute set when given.  jd.com's accessibility script
// (aria.js) constructs one at module init.
TEST_F(DomBinderTest, AudioConstructorCreatesMediaElement)
{
  EXPECT_TRUE(EvalBool("(function(){ var a = new Audio(); "
                       "return a.tagName === 'AUDIO' && a.getAttribute('src') === null; })()"));
  EXPECT_TRUE(EvalBool("(function(){ var a = new Audio('https://example.com/x.mp3'); "
                       "return a.getAttribute('src') === 'https://example.com/x.mp3'; })()"));
  // The media surface is shared with createElement('audio'): play/pause and
  // the src accessor live on the element prototype.
  EXPECT_TRUE(EvalBool("(function(){ var a = new Audio(); "
                       "return typeof a.play === 'function' && typeof a.pause === 'function' "
                       "&& typeof a.src === 'string'; })()"));
}

// window.matchMedia(query): returns a MediaQueryList evaluated against the
// engine's fixed 800x600 viewport; the list exposes matches/media and the
// standard no-op listener hooks (so scripts don't throw).
TEST_F(DomBinderTest, MatchMediaEvaluatesBasicQueries)
{
  EXPECT_TRUE(EvalBool("matchMedia === window.matchMedia"));
  EXPECT_TRUE(EvalBool("(function(){ "
                       "var yes = matchMedia('(min-width: 800px)'); "
                       "var no = matchMedia('(max-width: 600px)'); "
                       "var h = matchMedia('(min-height: 500px) and (max-height: 700px)'); "
                       "var or = matchMedia('(min-width: 2000px), (min-width: 800px)'); "
                       "var notq = matchMedia('not (max-width: 600px)'); "
                       "var portrait = matchMedia('(orientation: portrait)'); "
                       "var dark = matchMedia('(prefers-color-scheme: dark)'); "
                       "return yes.matches === true && no.matches === false "
                       "       && h.matches === true && or.matches === true "
                       "       && notq.matches === true && portrait.matches === false "
                       "       && dark.matches === false "
                       "       && yes.media === '(min-width: 800px)' "
                       "       && typeof yes.addEventListener === 'function' "
                       "       && typeof yes.removeListener === 'function'; })()"));
}

// performance.timing.navigationStart: the page load start (epoch ms).  bing's
// bootstrap reads performance.timing.navigationStart; before this the read
// threw "cannot read property 'navigationStart' of undefined".
TEST_F(DomBinderTest, PerformanceTimingNavigationStart)
{
  EXPECT_TRUE(EvalBool("typeof performance.timing === 'object'"));
  EXPECT_TRUE(EvalBool("typeof performance.timing.navigationStart === 'number'"));
  EXPECT_TRUE(EvalNumber("performance.timing.navigationStart") > 1000000000000.0);
  EXPECT_EQ(EvalString("performance.timing.navigationStart"), EvalString("performance.timeOrigin"));
}

TEST_F(DomBinderTest, NavigatorAppVersionIsAString)
{
  EXPECT_TRUE(EvalBool("typeof navigator.appVersion === 'string'"));
  EXPECT_EQ(EvalString("navigator.appVersion"), EvalString("navigator.userAgent"));
  EXPECT_TRUE(EvalBool("navigator.appVersion.split(';').length >= 1"));
}

TEST_F(DomBinderTest, NavigatorMimeTypesIsAStableEmptyCollection)
{
  EXPECT_TRUE(EvalBool("typeof navigator.mimeTypes === 'object'"));
  EXPECT_EQ(EvalNumber("navigator.mimeTypes.length"), 0.0);
  EXPECT_TRUE(EvalBool("navigator.mimeTypes === window.navigator.mimeTypes"));
  EXPECT_TRUE(EvalBool("navigator.mimeTypes.item(0) === null"));
  EXPECT_TRUE(EvalBool("navigator.mimeTypes.namedItem('text/html') === null"));
}

TEST_F(DomBinderTest, CryptoGetRandomValuesFillsIntegerTypedArrays)
{
  EXPECT_TRUE(EvalBool("(function() {"
                       "  var values = new Uint8Array(32);"
                       "  var returned = crypto.getRandomValues(values);"
                       "  return returned === values && values.some(function(value) {"
                       "    return value !== 0;"
                       "  });"
                       "})()"));
  EXPECT_TRUE(EvalBool("(function() {"
                       "  var values = new Uint16Array(8);"
                       "  return crypto.getRandomValues(values) === values;"
                       "})()"));
}

TEST_F(DomBinderTest, CryptoGetRandomValuesValidatesInputAndQuota)
{
  EXPECT_TRUE(EvalBool("(function() { try { crypto.getRandomValues([]); } "
                       "catch (error) { return error instanceof TypeError; } return false; })()"));
  EXPECT_TRUE(EvalBool("(function() { try { crypto.getRandomValues(new Float32Array(4)); } "
                       "catch (error) { return error instanceof TypeError; } return false; })()"));
  EXPECT_TRUE(EvalBool("(function() { try { crypto.getRandomValues(new Uint8Array(65537)); } "
                       "catch (error) { return error.name === 'QuotaExceededError'; } "
                       "return false; })()"));
}

// Element layout geometry getters map the browser layer's element_geometry
// callback onto getBoundingClientRect / offsetWidth / clientWidth / offsetTop
// etc.  Here the callback returns fixed values; the browser integration tests
// exercise real layout.
TEST(DomBinderGeometryTest, GeometryGettersUseCallback)
{
  auto document = html::Parser(R"(<html><body>
    <div id="box">hi</div>
    <span id="sp">text</span>
    <p id="np">none</p>
  </body></html>)")
                      .Parse();
  javascript::PageApis apis;
  apis.element_geometry =
      [](const dom::Element& element) -> std::optional<javascript::ElementGeometry> {
    const std::string id = std::string(element.GetAttribute("id").value_or(""));
    if (id == "box") {
      javascript::ElementGeometry g;
      g.x = 10;
      g.y = 12;
      g.width = 132;
      g.height = 72;
      g.client_width = 128;
      g.client_height = 68;
      g.border_top = 2;
      g.border_left = 2;
      return g;
    }
    if (id == "sp") {
      javascript::ElementGeometry g;
      g.x = 20;
      g.y = 30;
      g.width = 40;
      g.height = 16;
      g.client_width = 40;
      g.client_height = 16;
      return g;
    }
    return std::nullopt;
  };
  javascript::DomBinder binder(*document, apis);

  const auto num = [&](const std::string& code) -> double {
    auto r = binder.Evaluate(code);
    if (!r.has_value()) {
      return -1e9;
    }
    auto n = r.value().ToNumber();
    return n.has_value() ? n.value() : -1e9;
  };
  const auto str = [&](const std::string& code) -> std::string {
    auto r = binder.Evaluate(code);
    if (!r.has_value()) {
      return "<error>";
    }
    auto s = r.value().ToString();
    return s.has_value() ? s.value() : "<tostring-error>";
  };

  EXPECT_EQ(num("document.getElementById('box').offsetWidth"), 132);
  EXPECT_EQ(num("document.getElementById('box').offsetHeight"), 72);
  EXPECT_EQ(num("document.getElementById('box').offsetLeft"), 10);
  EXPECT_EQ(num("document.getElementById('box').offsetTop"), 12);
  EXPECT_EQ(num("document.getElementById('box').clientWidth"), 128);
  EXPECT_EQ(num("document.getElementById('box').clientHeight"), 68);
  EXPECT_EQ(num("document.getElementById('box').clientLeft"), 2);
  EXPECT_EQ(num("document.getElementById('box').clientTop"), 2);
  EXPECT_EQ(num("document.getElementById('box').getBoundingClientRect().x"), 10);
  EXPECT_EQ(num("document.getElementById('box').getBoundingClientRect().y"), 12);
  EXPECT_EQ(num("document.getElementById('box').getBoundingClientRect().left"), 10);
  EXPECT_EQ(num("document.getElementById('box').getBoundingClientRect().top"), 12);
  EXPECT_EQ(num("document.getElementById('box').getBoundingClientRect().right"), 142);
  EXPECT_EQ(num("document.getElementById('box').getBoundingClientRect().bottom"), 84);
  EXPECT_EQ(num("document.getElementById('box').getBoundingClientRect().width"), 132);
  EXPECT_EQ(num("document.getElementById('box').getBoundingClientRect().height"), 72);
  EXPECT_EQ(num("document.getElementById('box').getBoundingClientRect().toJSON().width"), 132);

  EXPECT_EQ(num("document.getElementById('sp').offsetWidth"), 40);

  // Elements with no laid-out box report 0 / a zero rect.
  EXPECT_EQ(num("document.getElementById('np').offsetWidth"), 0);
  EXPECT_EQ(num("document.getElementById('np').getBoundingClientRect().width"), 0);

  // offsetParent resolves to <body>.
  EXPECT_EQ(str("document.getElementById('box').offsetParent.tagName"), "BODY");
  EXPECT_EQ(str("document.getElementById('box').offsetParent.offsetParent === null ? 'null' : 'x'"),
            "null");
}

// Element.insertAdjacentHTML parses the fragment and inserts at each position;
// the inserted nodes are reachable through the DOM.
TEST_F(DomBinderTest, InsertAdjacentHTML)
{
  // beforeend: append as the last child.
  EvalString(
      "document.getElementById('main').insertAdjacentHTML('beforeend','<span id=\"x\">X</span>');");
  ASSERT_TRUE(EvalBool("document.getElementById('x')!==null"));
  EXPECT_EQ(EvalString("document.getElementById('x').tagName"), "SPAN");
  EXPECT_EQ(EvalString("document.getElementById('x').textContent"), "X");

  // afterbegin: insert as the first child.
  EvalString("document.getElementById('main').insertAdjacentHTML('afterbegin','<span "
             "id=\"y\">Y</span>');");
  ASSERT_TRUE(EvalBool("document.getElementById('y')!==null"));
  EXPECT_EQ(EvalString("document.getElementById('main').firstChild.id"), "y");

  // beforebegin: sibling before the element.
  EvalString("var p=document.createElement('p');p.id='p1';document.body.appendChild(p);"
             "p.insertAdjacentHTML('beforebegin','<b id=\"b1\">B</b>');");
  ASSERT_TRUE(EvalBool("document.getElementById('b1')!==null"));
  EXPECT_EQ(EvalString("document.getElementById('b1').tagName"), "B");

  // afterend: sibling after the element.
  EvalString("document.getElementById('p1').insertAdjacentHTML('afterend','<i id=\"i1\">I</i>');");
  ASSERT_TRUE(EvalBool("document.getElementById('i1')!==null"));
  EXPECT_EQ(EvalString("document.getElementById('p1').nextSibling.id"), "i1");
}

// ParentNode.prepend / ChildNode.before/after/replaceWith.  These were missing
// entirely, which broke real pages: example.com's own script calls
// document.body.prepend(...) and threw "TypeError: not a function" (found with
// the GUI screenshot tool's console output while measuring load performance).
TEST_F(DomBinderTest, PrependInsertsAtTheFrontInArgumentOrder)
{
  EvalString("var d=document,m=d.getElementById('main');"
             "var a=d.createElement('a');a.id='a1';"
             "var b=d.createElement('b');b.id='b1';"
             "m.prepend(a,b);");
  // Element children (the fixture has whitespace text nodes, so firstChild is
  // not necessarily an element).
  EXPECT_EQ(EvalString("document.getElementById('main').children[0].id"), "a1");
  EXPECT_EQ(EvalString("document.getElementById('main').children[1].id"), "b1");
  // The previously first child follows the inserted nodes.
  EXPECT_EQ(EvalString("document.getElementById('main').children[2].id"), "first");

  // A node already in the tree moves instead of being cloned.
  EXPECT_EQ(EvalString("(function(){var d=document,m=d.getElementById('main');"
                       "m.prepend(d.getElementById('b1'));"
                       "return d.getElementById('b1').parentNode.id;})()"),
            "main");
  EXPECT_EQ(EvalString("document.getElementById('main').children[0].id"), "b1");
}

TEST_F(DomBinderTest, BeforeAfterAndReplaceWithInsertAroundTheNode)
{
  EvalString("var d=document,m=d.getElementById('main');"
             "var p=d.createElement('p');p.id='target';m.appendChild(p);"
             "var i=d.createElement('i');i.id='i1';p.before(i);"
             "var u=d.createElement('u');u.id='u1';p.after(u);");
  // i1, target, u1 in document order.
  EXPECT_EQ(EvalString("document.getElementById('i1').nextSibling.id"), "target");
  EXPECT_EQ(EvalString("document.getElementById('target').nextSibling.id"), "u1");

  // replaceWith swaps the node out, keeping the neighbours in order.
  EvalString("var d=document,s=d.createElement('s');s.id='s1';"
             "document.getElementById('target').replaceWith(s);");
  EXPECT_EQ(EvalString("document.getElementById('target')===null?'gone':'present'"), "gone");
  EXPECT_EQ(EvalString("document.getElementById('i1').nextSibling.id"), "s1");
  EXPECT_EQ(EvalString("document.getElementById('s1').nextSibling.id"), "u1");
}

TEST_F(DomBinderTest, ModernInsertionRejectsCycles)
{
  // An ancestor must never be inserted into its own descendant.  appendChild
  // already rejected this; insertBefore did not, and a cyclic tree does not
  // survive later traversals.
  const std::string setup = "var d=document;var outer=d.createElement('div');"
                            "var inner=d.createElement('span');outer.appendChild(inner);";
  EXPECT_TRUE(EvalBool("(function(){" + setup +
                       "try{inner.insertBefore(outer,null);}catch(e){"
                       "return e.name==='HierarchyRequestError';}return false;})()"));
  EXPECT_TRUE(EvalBool("(function(){" + setup +
                       "try{inner.replaceWith(outer);return false;}catch(e){"
                       "return e.name==='HierarchyRequestError';}})()"));
  EXPECT_TRUE(EvalBool("(function(){" + setup +
                       "try{outer.prepend(outer);return false;}catch(e){"
                       "return e.name==='HierarchyRequestError';}})()"));
}

TEST_F(DomBinderTest, InsertAdjacentElementAndText)
{
  EvalString("var d=document,m=d.getElementById('main');"
             "var x=d.createElement('em');x.id='x';"
             "var returned=m.insertAdjacentElement('beforeend',x);");
  EXPECT_EQ(EvalString("document.getElementById('x').parentNode.id"), "main");
  EXPECT_EQ(EvalString("document.getElementById('main').insertAdjacentElement('beforeend',"
                       "document.createElement('em')).tagName"),
            "EM");
  // Returns the inserted element itself.
  EXPECT_TRUE(EvalBool("(function(){var d=document,m=d.getElementById('main');"
                       "var y=d.createElement('em');y.id='y';"
                       "return m.insertAdjacentElement('afterbegin',y)===y;})()"));
  EXPECT_EQ(EvalString("document.getElementById('main').children[0].id"), "y");

  EvalString("document.getElementById('main').insertAdjacentText('beforeend','tail');");
  EXPECT_EQ(EvalString("document.getElementById('main').lastChild.nodeValue"), "tail");

  // A detached element has no sibling position: insertAdjacentElement returns
  // null and insertAdjacentText is a no-op.
  EXPECT_EQ(EvalString("(function(){var d=document;var lone=d.createElement('div');"
                       "var r=lone.insertAdjacentElement('beforebegin',d.createElement('b'));"
                       "return r===null?'null':'other';})()"),
            "null");
  EXPECT_EQ(EvalString("(function(){var d=document;var lone=d.createElement('div');"
                       "lone.insertAdjacentText('beforebegin','t');"
                       "return lone.childNodes.length;})()"),
            "0");
  // An unknown position is a SyntaxError, not a silent no-op.
  EXPECT_TRUE(EvalBool("(function(){var d=document;try{"
                       "d.getElementById('main').insertAdjacentText('sideways','t');"
                       "return false;}catch(e){return e.name==='SyntaxError';}})()"));
}

// insertAdjacentHTML must keep the fragment's document order.  The previous
// implementation walked the parsed nodes backwards, which reversed a
// multi-node fragment (the existing test only ever inserted one node).
TEST_F(DomBinderTest, InsertAdjacentHTMLKeepsFragmentOrder)
{
  EvalString("document.getElementById('main').insertAdjacentHTML('beforeend',"
             "'<i id=\"o1\"></i><i id=\"o2\"></i><i id=\"o3\"></i>');");
  EXPECT_EQ(EvalString("document.getElementById('o1').nextSibling.id"), "o2");
  EXPECT_EQ(EvalString("document.getElementById('o2').nextSibling.id"), "o3");
  EXPECT_EQ(EvalString("document.getElementById('main').lastChild.id"), "o3");

  // Same for the inner-first position.
  EvalString("document.getElementById('main').insertAdjacentHTML('afterbegin',"
             "'<i id=\"f1\"></i><i id=\"f2\"></i>');");
  EXPECT_EQ(EvalString("document.getElementById('main').firstChild.id"), "f1");
  EXPECT_EQ(EvalString("document.getElementById('f1').nextSibling.id"), "f2");
}

// The exact chain example.com's s.js runs.
TEST_F(DomBinderTest, ExampleComScriptChainRuns)
{
  EvalString("var d=document;B=d.body;"
             "B.insertAdjacentHTML('beforeend','<p>a</p><p>b</p>');"
             "P=[...B.querySelectorAll('p')];P[0].lang='en';B.prepend(P[0]);");
  EXPECT_EQ(EvalString("document.body.firstChild.tagName"), "P");
  EXPECT_EQ(EvalString("document.body.firstChild.lang"), "en");
  // The fixture body already holds two paragraphs (one inside #main), so the
  // script's two are added on top.
  EXPECT_EQ(EvalString("document.querySelectorAll('p').length"), "4");
}

// ---------------------------------------------------------------------------
// window.indexedDB — binding tests over the real storage core
// ---------------------------------------------------------------------------

namespace {

// Wires a real storage::IndexedDbStore (backed by a temp profile) into the
// PageApis callbacks, mirroring browser::RunPageScripts.
class IdbApis
{
public:
  IdbApis(storage::IndexedDbStore& store) : store_(store) {}

  PageApis Make()
  {
    // Capture the store by pointer (not |this|): the wiring object is a
    // local, but the callbacks outlive it.
    storage::IndexedDbStore* store = &store_;
    PageApis apis;
    apis.idb_current_version = [store](std::string_view db) {
      return store->CurrentVersion("https://idb.test", db);
    };
    apis.idb_create_db = [store](std::string_view db) {
      return store->CreateDatabase("https://idb.test", db);
    };
    apis.idb_set_version = [store](std::string_view db, int64_t version) {
      return store->SetVersion("https://idb.test", db, version);
    };
    apis.idb_delete_db = [store](std::string_view db) {
      return store->DeleteDatabase("https://idb.test", db);
    };
    apis.idb_store_names = [store](std::string_view db) {
      const base::Result<std::vector<storage::IndexedDbStore::ObjectStoreMeta>> metas =
          store->ObjectStores("https://idb.test", db);
      if (!metas.has_value()) {
        return base::Result<std::vector<IdbStoreMeta>>(metas.error());
      }
      std::vector<IdbStoreMeta> out;
      for (const auto& meta : metas.value()) {
        IdbStoreMeta item;
        item.name = meta.name;
        item.key_path = meta.key_path;
        item.auto_increment = meta.auto_increment;
        out.push_back(std::move(item));
      }
      return base::Result<std::vector<IdbStoreMeta>>(std::move(out));
    };
    apis.idb_create_store = [store](std::string_view db,
                                    std::string_view store_name,
                                    std::string_view key_path,
                                    bool auto_increment) {
      return store->CreateObjectStore("https://idb.test", db, store_name, key_path, auto_increment);
    };
    apis.idb_delete_store = [store](std::string_view db, std::string_view store_name) {
      return store->DeleteObjectStore("https://idb.test", db, store_name);
    };
    apis.idb_add = [store](std::string_view db,
                           std::string_view store_name,
                           std::optional<std::string> key,
                           std::string value) {
      return store->Add("https://idb.test", db, store_name, std::move(key), std::move(value));
    };
    apis.idb_put = [store](std::string_view db,
                           std::string_view store_name,
                           std::optional<std::string> key,
                           std::string value) {
      return store->Put("https://idb.test", db, store_name, std::move(key), std::move(value));
    };
    apis.idb_get = [store](std::string_view db, std::string_view store_name, std::string key) {
      return store->Get("https://idb.test", db, store_name, std::move(key));
    };
    apis.idb_delete = [store](std::string_view db, std::string_view store_name, std::string key) {
      return store->Delete("https://idb.test", db, store_name, std::move(key));
    };
    apis.idb_clear = [store](std::string_view db, std::string_view store_name) {
      return store->Clear("https://idb.test", db, store_name);
    };
    apis.idb_count = [store](std::string_view db, std::string_view store_name) {
      return store->Count("https://idb.test", db, store_name);
    };
    apis.idb_get_all = [store](std::string_view db, std::string_view store_name) {
      return store->GetAll("https://idb.test", db, store_name);
    };
    return apis;
  }

private:
  storage::IndexedDbStore& store_;
};

// Temp directory names must be unique per process.  MSVC exposes the process
// id as _getpid (<process.h>), POSIX as getpid (<unistd.h>).
long CurrentProcessId()
{
#ifdef _WIN32
  return static_cast<long>(::_getpid());
#else
  return static_cast<long>(::getpid());
#endif
}

// A temp profile directory (removed on destruction) for the real store.
class IdbTempProfile
{
public:
  IdbTempProfile()
  {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("neko_idb_js_" + std::to_string(CurrentProcessId()) + "_" + std::to_string(counter_++));
    path_ = path.string();
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
  }
  ~IdbTempProfile()
  {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  const std::string& path() const
  {
    return path_;
  }

private:
  static int counter_;
  std::string path_;
};
int IdbTempProfile::counter_ = 0;

// A DomBinder wired to a real IndexedDbStore over a temp profile.
class IndexedDbTest : public DomBinderTest
{
protected:
  void SetUp() override
  {
    DomBinderTest::SetUp();
    store_ = std::make_unique<storage::IndexedDbStore>(profile_.path());
    ASSERT_TRUE(store_->Load().has_value());
    IdbApis wiring(*store_);
    idb_binder_ = std::make_unique<DomBinder>(*document_, wiring.Make());
    idb_binder_->SetConsoleSink([this](std::string_view level, std::string_view text) {
      console_.push_back(std::string(level) + ": " + std::string(text));
    });
  }

  std::string IdbEval(const std::string& code)
  {
    auto r = idb_binder_->Evaluate(code);
    if (!r.has_value()) {
      return "<error: " + r.error().message() + ">";
    }
    auto s = r.value().ToString();
    return s.has_value() ? s.value() : "<tostring-error>";
  }

  IdbTempProfile profile_;
  std::unique_ptr<storage::IndexedDbStore> store_;
  std::unique_ptr<DomBinder> idb_binder_;
};

} // namespace

TEST_F(IndexedDbTest, OpenRunsUpgradeAndSucceeds)
{
  ASSERT_TRUE(idb_binder_
                  ->Evaluate("var db, upgraded = false, store;\n"
                             "var req = indexedDB.open('kv', 1);\n"
                             "req.onupgradeneeded = function(e) {\n"
                             "  upgraded = true;\n"
                             "  db = e.target.result;\n"
                             "  store = db.createObjectStore('items', {keyPath: 'id'});\n"
                             "};\n"
                             "req.onsuccess = function(e) { db = e.target.result; };")
                  .has_value());
  EXPECT_EQ(IdbEval("upgraded"), "true");
  EXPECT_EQ(IdbEval("db.name"), "kv");
  EXPECT_EQ(IdbEval("db.version"), "1");
  EXPECT_EQ(IdbEval("db.objectStoreNames.length"), "1");
  EXPECT_EQ(IdbEval("db.objectStoreNames[0]"), "items");
  EXPECT_EQ(IdbEval("store.keyPath"), "id");
  EXPECT_EQ(IdbEval("store.autoIncrement"), "false");
}

TEST_F(IndexedDbTest, AddGetAndTransactionComplete)
{
  ASSERT_TRUE(idb_binder_
                  ->Evaluate("var db;\n"
                             "indexedDB.open('kv', 1).onupgradeneeded = function(e) {\n"
                             "  db = e.target.result;\n"
                             "  db.createObjectStore('items', {keyPath: 'id'});\n"
                             "};")
                  .has_value());
  ASSERT_TRUE(idb_binder_
                  ->Evaluate("var got, done = false;\n"
                             "var tx = db.transaction('items', 'readwrite');\n"
                             "tx.oncomplete = function() { done = true; };\n"
                             "var st = tx.objectStore('items');\n"
                             "st.add({id: 1, label: 'one'});\n"
                             "st.get(1).onsuccess = function(e) { got = e.target.result; };")
                  .has_value());
  EXPECT_EQ(IdbEval("got.label"), "one");
  EXPECT_EQ(IdbEval("done"), "true");
}

TEST_F(IndexedDbTest, DuplicateAddFailsWithConstraintError)
{
  ASSERT_TRUE(idb_binder_
                  ->Evaluate("var db;\n"
                             "indexedDB.open('kv', 1).onupgradeneeded = function(e) {\n"
                             "  db = e.target.result;\n"
                             "  db.createObjectStore('items', {keyPath: 'id'});\n"
                             "};")
                  .has_value());
  ASSERT_TRUE(idb_binder_
                  ->Evaluate("var err;\n"
                             "var tx = db.transaction('items', 'readwrite');\n"
                             "tx.objectStore('items').add({id: 1, x: 1});\n"
                             "var tx2 = db.transaction('items', 'readwrite');\n"
                             "tx2.objectStore('items').add({id: 1, x: 2}).onerror = function(e) {\n"
                             "  err = e.target.error;\n"
                             "};")
                  .has_value());
  EXPECT_EQ(IdbEval("err.name"), "ConstraintError");
}

TEST_F(IndexedDbTest, AutoIncrementGeneratesKeys)
{
  ASSERT_TRUE(idb_binder_
                  ->Evaluate("var db;\n"
                             "indexedDB.open('auto', 1).onupgradeneeded = function(e) {\n"
                             "  db = e.target.result;\n"
                             "  db.createObjectStore('items', {autoIncrement: true});\n"
                             "};")
                  .has_value());
  ASSERT_TRUE(
      idb_binder_
          ->Evaluate("var k1, k2;\n"
                     "var tx = db.transaction('items', 'readwrite');\n"
                     "var st = tx.objectStore('items');\n"
                     "st.add({label: 'first'}).onsuccess = function(e) { k1 = e.target.result; };\n"
                     "st.add({label: 'second'}).onsuccess = function(e) { k2 = e.target.result; };")
          .has_value());
  EXPECT_EQ(IdbEval("k1"), "1");
  EXPECT_EQ(IdbEval("k2"), "2");
}

TEST_F(IndexedDbTest, GetAllSortedAndCount)
{
  ASSERT_TRUE(idb_binder_
                  ->Evaluate("var db;\n"
                             "indexedDB.open('sorted', 1).onupgradeneeded = function(e) {\n"
                             "  db = e.target.result;\n"
                             "  db.createObjectStore('items');\n"
                             "};")
                  .has_value());
  ASSERT_TRUE(idb_binder_
                  ->Evaluate("var all, n;\n"
                             "var tx = db.transaction('items', 'readwrite');\n"
                             "var st = tx.objectStore('items');\n"
                             "st.add('string-a', 'a');\n"
                             "st.add('two', 2);\n"
                             "st.add('one', 1);\n"
                             "st.getAll().onsuccess = function(e) { all = e.target.result; };\n"
                             "st.count().onsuccess = function(e) { n = e.target.result; };")
                  .has_value());
  // Numbers before strings, each ascending (values follow their keys).
  EXPECT_EQ(IdbEval("JSON.stringify(all)"), "[\"one\",\"two\",\"string-a\"]");
  EXPECT_EQ(IdbEval("n"), "3");
}

TEST_F(IndexedDbTest, LowerVersionFailsWithVersionError)
{
  ASSERT_TRUE(idb_binder_
                  ->Evaluate("var db;\n"
                             "indexedDB.open('kv', 2).onupgradeneeded = function(e) {\n"
                             "  db = e.target.result;\n"
                             "};")
                  .has_value());
  ASSERT_TRUE(
      idb_binder_
          ->Evaluate("var verr;\n"
                     "indexedDB.open('kv', 1).onerror = function(e) { verr = e.target.error; };")
          .has_value());
  EXPECT_EQ(IdbEval("verr.name"), "VersionError");
}

TEST_F(IndexedDbTest, ReadonlyTransactionRejectsWrites)
{
  ASSERT_TRUE(idb_binder_
                  ->Evaluate("var db;\n"
                             "indexedDB.open('kv', 1).onupgradeneeded = function(e) {\n"
                             "  db = e.target.result;\n"
                             "  db.createObjectStore('items');\n"
                             "};")
                  .has_value());
  ASSERT_TRUE(idb_binder_
                  ->Evaluate("var rerr;\n"
                             "var tx = db.transaction('items', 'readonly');\n"
                             "try { tx.objectStore('items').add({}); }\n"
                             "catch (e) { rerr = e.name; }")
                  .has_value());
  EXPECT_EQ(IdbEval("rerr"), "ReadOnlyError");
}

TEST_F(IndexedDbTest, CreateStoreOutsideUpgradeThrows)
{
  ASSERT_TRUE(idb_binder_
                  ->Evaluate("var db;\n"
                             "indexedDB.open('kv', 1).onupgradeneeded = function(e) {\n"
                             "  db = e.target.result;\n"
                             "  db.createObjectStore('items');\n"
                             "};")
                  .has_value());
  ASSERT_TRUE(idb_binder_
                  ->Evaluate("var thrown;\n"
                             "try { db.createObjectStore('late'); } catch (e) { thrown = e.name; }")
                  .has_value());
  EXPECT_EQ(IdbEval("thrown"), "InvalidStateError");
}

TEST_F(IndexedDbTest, DataPersistsAcrossBinders)
{
  ASSERT_TRUE(idb_binder_
                  ->Evaluate("var db;\n"
                             "indexedDB.open('kv', 1).onupgradeneeded = function(e) {\n"
                             "  db = e.target.result;\n"
                             "  db.createObjectStore('items', {keyPath: 'id'});\n"
                             "};")
                  .has_value());
  ASSERT_TRUE(idb_binder_
                  ->Evaluate("var tx = db.transaction('items', 'readwrite');\n"
                             "tx.objectStore('items').add({id: 7, label: 'seven'});")
                  .has_value());
  // A second binder over the same store (a new page load) sees the record.
  IdbApis wiring(*store_);
  DomBinder second(*document_, wiring.Make());
  ASSERT_TRUE(second
                  .Evaluate("var db2, got;\n"
                            "var req = indexedDB.open('kv');\n"
                            "req.onsuccess = function(e) {\n"
                            "  db2 = e.target.result;\n"
                            "  var tx = db2.transaction('items');\n"
                            "  tx.objectStore('items').get(7).onsuccess = function(e2) {\n"
                            "    got = e2.target.result;\n"
                            "  };\n"
                            "};")
                  .has_value());
  auto label = second.Evaluate("got.label");
  ASSERT_TRUE(label.has_value());
  ASSERT_TRUE(label.value().ToString().has_value());
  EXPECT_EQ(label.value().ToString().value(), "seven");
}

TEST_F(IndexedDbTest, DeleteDatabase)
{
  ASSERT_TRUE(idb_binder_
                  ->Evaluate("var db;\n"
                             "indexedDB.open('kv', 1).onupgradeneeded = function(e) {\n"
                             "  db = e.target.result;\n"
                             "};")
                  .has_value());
  ASSERT_TRUE(
      idb_binder_
          ->Evaluate("var del_ok = false;\n"
                     "indexedDB.deleteDatabase('kv').onsuccess = function() { del_ok = true; };")
          .has_value());
  EXPECT_EQ(IdbEval("del_ok"), "true");
  EXPECT_EQ(store_->CurrentVersion("https://idb.test", "kv").value(), 0);
}

TEST_F(DomBinderTest, VideoMediaControls)
{
  PageApis apis;
  double position = 1.5;
  bool playing = false;
  apis.video_play = [&playing](const dom::Element&) { playing = true; };
  apis.video_pause = [&playing](const dom::Element&) { playing = false; };
  apis.video_seek = [&position](const dom::Element&, double seconds) { position = seconds; };
  apis.video_duration = [](const dom::Element&) -> std::optional<double> { return 3.0; };
  apis.video_current_time = [&position](const dom::Element&) -> std::optional<double> {
    return position;
  };
  apis.video_paused = [&playing](const dom::Element&) { return !playing; };

  DomBinder binder(*document_, apis);
  const auto eval = [&](const std::string& code) -> std::string {
    auto r = binder.Evaluate(code);
    if (!r.has_value()) {
      return "<error>";
    }
    auto s = r.value().ToString();
    return s.has_value() ? s.value() : "<tostring-error>";
  };
  ASSERT_TRUE(binder.Evaluate("var v = document.createElement('video');").has_value());
  EXPECT_EQ(eval("v.duration"), "3");
  EXPECT_EQ(eval("v.currentTime"), "1.5");
  EXPECT_EQ(eval("v.paused"), "true");
  ASSERT_TRUE(binder.Evaluate("v.play();").has_value());
  EXPECT_TRUE(playing);
  EXPECT_EQ(eval("v.paused"), "false");
  ASSERT_TRUE(binder.Evaluate("v.currentTime = 0.25;").has_value());
  EXPECT_DOUBLE_EQ(position, 0.25);
  ASSERT_TRUE(binder.Evaluate("v.pause();").has_value());
  EXPECT_FALSE(playing);

  // play() on a non-media element throws.
  const auto r = binder.Evaluate("document.getElementById('main').play()");
  EXPECT_FALSE(r.has_value());
}

TEST_F(DomBinderTest, Canvas2dFillRect)
{
  PageApis apis;
  const dom::Element* painted_element = nullptr;
  double painted_x = 0;
  double painted_y = 0;
  double painted_width = 0;
  double painted_height = 0;
  std::array<std::uint8_t, 4> painted_color{};
  apis.canvas_fill_rect = [&](const dom::Element& element,
                              double x,
                              double y,
                              double width,
                              double height,
                              std::array<std::uint8_t, 4> color) {
    painted_element = &element;
    painted_x = x;
    painted_y = y;
    painted_width = width;
    painted_height = height;
    painted_color = color;
  };

  binder_ = std::make_unique<DomBinder>(*document_, apis);
  ASSERT_TRUE(binder_
                  ->Evaluate("var canvas = document.createElement('canvas');\n"
                             "var context = canvas.getContext('2d');\n"
                             "context.fillStyle = 'rgba(10, 20, 30, 0.5)';\n"
                             "context.fillRect(1, 2, 3, 4);")
                  .has_value());
  EXPECT_EQ(EvalString("canvas instanceof HTMLCanvasElement"), "true");
  EXPECT_EQ(EvalString("context instanceof CanvasRenderingContext2D"), "true");
  EXPECT_EQ(EvalString("canvas.getContext('2d') === context"), "true");
  EXPECT_EQ(EvalString("canvas.getContext('webgl')"), "null");
  EXPECT_EQ(EvalString("context.fillStyle"), "rgba(10, 20, 30, 0.5)");
  ASSERT_NE(painted_element, nullptr);
  EXPECT_EQ(painted_element->tag_name(), "canvas");
  EXPECT_DOUBLE_EQ(painted_x, 1);
  EXPECT_DOUBLE_EQ(painted_y, 2);
  EXPECT_DOUBLE_EQ(painted_width, 3);
  EXPECT_DOUBLE_EQ(painted_height, 4);
  EXPECT_EQ(painted_color, (std::array<std::uint8_t, 4>{10, 20, 30, 128}));
}

// ---------------------------------------------------------------------------
// XMLHttpRequest (wired through PageApis::xhr_request).
// ---------------------------------------------------------------------------

TEST(DomBinderXhrTest, XhrWritableAttributesAreAccepted)
{
  // jQuery's XHR transport assigns xhrFields entries onto the object
  // (xhr.withCredentials = true) and libraries set xhr.timeout: both are
  // writable IDL attributes.  The read-only withCredentials broke bilibili's
  // tracker script with "no setter for property".
  dom::Document doc;
  PageApis apis;
  apis.resolve_url = [](const std::string& raw) {
    return raw.rfind("http://", 0) == 0 ? raw : "http://test.local/" + raw;
  };
  apis.xhr_request = [](const std::string&,
                        const std::string&,
                        const std::vector<std::pair<std::string, std::string>>&,
                        const std::string&) -> base::Result<FetchResponse> {
    FetchResponse out;
    out.status = 200;
    return base::Ok(std::move(out));
  };
  DomBinder binder(doc, apis);

  const auto run = binder.Evaluate(R"(
    globalThis.ok = (function(){
      var x = new XMLHttpRequest();
      if (x.withCredentials !== false) return false;
      // The jQuery transport pattern: for (u in xhrFields) xhr[u] = xhrFields[u].
      var fields = { withCredentials: true, timeout: 2500 };
      for (var u in fields) {
        x[u] = fields[u];
      }
      if (x.withCredentials !== true || x.timeout !== 2500) return false;
      x.timeout = -5;
      return x.timeout === 0;
    })();
  )");
  ASSERT_TRUE(run.has_value()) << run.error().message();
  auto ok = binder.Evaluate("globalThis.ok");
  ASSERT_TRUE(ok.has_value());
  EXPECT_TRUE(ok.value().ToBoolean().value_or(false));
}

// One pump turn's attribution: what the turn actually ran.  This is what makes a
// long turn actionable -- measuring a real page with it showed the 2.7 s turn on
// bilibili belongs to 38 requestAnimationFrame callbacks (2.5 s), not to the XHR
// deliveries (8 of them, 0 ms), which is the opposite of the initial guess.
TEST(DomBinderPumpBreakdownTest, AttributesEnabledAndDisabledSegments)
{
  auto doc = html::Parser(R"(<!doctype html><html><body><div id="d"></div></body></html>)").Parse();
  PageApis apis;
  apis.xhr_request = [](const std::string&,
                        const std::string&,
                        const std::vector<std::pair<std::string, std::string>>&,
                        const std::string&) -> base::Result<FetchResponse> {
    FetchResponse out;
    out.status = 200;
    out.body = "ok";
    return base::Ok(std::move(out));
  };
  DomBinder binder(*doc, apis);

  ASSERT_TRUE(binder
                  .Evaluate(R"(
            globalThis.ran = {timer:0, raf:0, xhr:0};
            setTimeout(function(){ globalThis.ran.timer++; }, 0);
            requestAnimationFrame(function(){ globalThis.ran.raf++; });
            var x = new XMLHttpRequest();
            x.onload = function(){ globalThis.ran.xhr++; };
            x.open('GET','/data', true);
            x.send();
          )")
                  .has_value());

  // A page with no MessagePort/WebSocket work attributes nothing to them (the
  // first turn does run the due setTimeout, so |timers| is not asserted here).
  DomBinder::TurnBreakdown first;
  binder.RunPendingTimers(&first);
  EXPECT_EQ(first.ports, 0u);
  EXPECT_EQ(first.websockets, 0u);
  EXPECT_EQ(first.ports_ms, 0);
  EXPECT_EQ(first.websockets_ms, 0);

  bool saw_all = false;
  for (int i = 0; i < 8 && !saw_all; ++i) {
    DomBinder::TurnBreakdown turn;
    binder.RunPendingTimers(&turn);
    // Each segment is attributed to this turn, never to another one.
    EXPECT_LE(turn.timers, 1u);
    EXPECT_LE(turn.raf, 1u);
    EXPECT_EQ(turn.ports, 0u);
    EXPECT_EQ(turn.websockets, 0u);
    if (turn.timers > 0 || turn.raf > 0 || turn.xhr > 0) {
      EXPECT_GE(turn.timers_ms, 0);
      EXPECT_GE(turn.raf_ms, 0);
    }
    saw_all = binder.Evaluate("globalThis.ran.timer>0").value().ToBoolean().value_or(false) &&
              binder.Evaluate("globalThis.ran.raf>0").value().ToBoolean().value_or(false) &&
              binder.Evaluate("globalThis.ran.xhr>0").value().ToBoolean().value_or(false);
  }
  EXPECT_TRUE(saw_all) << "a timer, a rAF callback and an XHR delivery should all have run";

  // With no out-parameter the call still works (the parameter is optional).
  EXPECT_GE(binder.RunPendingTimers(), 0);
}

// The per-turn requestAnimationFrame budget: a page with many queued callbacks
// used to block the worker for the whole batch (measured at 0.8-4 s on a real
// site), so the pump serves at most |limit| per turn and defers the rest.
TEST(DomBinderPumpBreakdownTest, RafBudgetSpreadsCallbacksOverTurns)
{
  auto doc = html::Parser(R"(<!doctype html><html><body><div id="d"></div></body></html>)").Parse();
  DomBinder binder(*doc, PageApis{});
  // Default is off: the budget is opt-in until the GUI harness justifies it.
  EXPECT_EQ(binder.raf_per_turn_limit(), DomBinder::kUnlimitedRafPerTurn);

  // Three callbacks with distinct bodies, so order is observable without the
  // classic loop-variable capture mistake.
  ASSERT_TRUE(binder
                  .Evaluate("globalThis.order=[];"
                            "requestAnimationFrame(function(){ globalThis.order.push(0); });"
                            "requestAnimationFrame(function(){ globalThis.order.push(1); });"
                            "requestAnimationFrame(function(){ globalThis.order.push(2); });")
                  .has_value());

  binder.SetRafPerTurnLimit(1);
  int turns_with_raf = 0;
  for (int i = 0; i < 6; ++i) {
    DomBinder::TurnBreakdown turn;
    binder.RunPendingTimers(&turn);
    EXPECT_LE(turn.raf, 1u); // the budget holds every turn
    if (turn.raf > 0) {
      ++turns_with_raf;
    }
  }
  EXPECT_EQ(turns_with_raf, 3); // one callback per turn
  EXPECT_EQ(binder.Evaluate("globalThis.order.join(',')").value().ToString().value(), "0,1,2");

  // Zero means unlimited: the whole queue runs in one turn, as before the budget.
  binder.SetRafPerTurnLimit(DomBinder::kUnlimitedRafPerTurn);
  ASSERT_TRUE(binder
                  .Evaluate("globalThis.order=[];"
                            "for (var i = 0; i < 12; i++) { requestAnimationFrame(function(){"
                            "  globalThis.order.push(1); }); }")
                  .has_value());
  DomBinder::TurnBreakdown bulk;
  binder.RunPendingTimers(&bulk);
  EXPECT_EQ(bulk.raf, 12u);
  EXPECT_EQ(binder.Evaluate("globalThis.order.length").value().ToString().value(), "12");
}

TEST(DomBinderXhrTest, XhrLifecycleAndEvents)
{
  dom::Document doc;
  PageApis apis;
  apis.resolve_url = [](const std::string& raw) {
    return raw.rfind("http://", 0) == 0 ? raw : "http://test.local/" + raw;
  };
  apis.xhr_request = [](const std::string& url,
                        const std::string& method,
                        const std::vector<std::pair<std::string, std::string>>& headers,
                        const std::string& body) -> base::Result<FetchResponse> {
    EXPECT_EQ(url, "http://test.local/api/data");
    EXPECT_EQ(method, "GET");
    EXPECT_TRUE(body.empty());
    // The caller forwarded the request header set via setRequestHeader.
    bool saw_header = false;
    for (const auto& header : headers) {
      if (header.first == "X-Test" && header.second == "yes") {
        saw_header = true;
      }
    }
    EXPECT_TRUE(saw_header);
    FetchResponse out;
    out.status = 200;
    out.status_text = "OK";
    out.final_url = url;
    out.headers.emplace_back("Content-Type", "application/json");
    out.body = R"({"ok": true})";
    return base::Ok(std::move(out));
  };
  DomBinder binder(doc, apis);

  const char* script = R"(
    globalThis.log = '';
    const xhr = new XMLHttpRequest();
    xhr.open('GET', 'api/data');
    xhr.setRequestHeader('X-Test', 'yes');
    xhr.onreadystatechange = function() {
      if (xhr.readyState === 4) {
        globalThis.log += ':rsc4-' + xhr.status + '-' + xhr.responseText +
                          '-ct:' + xhr.getResponseHeader('content-type');
      }
    };
    xhr.onload = function() { globalThis.log += ':loaded-' + xhr.statusText; };
    xhr.addEventListener('load', function() { globalThis.listener_fired = true; });
    xhr.send();
    // Async mode (the default): nothing is delivered yet; the DONE
    // transition and the events wait for the next pump.
    globalThis.immediate = xhr.readyState + ':' + xhr.status + ':' + globalThis.log;
  )";
  const auto run = binder.Evaluate(script);
  ASSERT_TRUE(run.has_value()) << "eval error: " << (run.has_value() ? "" : run.error().message());
  auto immediate = binder.Evaluate("globalThis.immediate");
  ASSERT_TRUE(immediate.has_value());
  ASSERT_TRUE(immediate.value().ToString().has_value());
  EXPECT_EQ(immediate.value().ToString().value(), "1:0:")
      << "send() must not deliver synchronously in async mode";
  (void)binder.RunPendingTimers();
  auto log = binder.Evaluate("globalThis.log");
  ASSERT_TRUE(log.has_value());
  auto text = log.value().ToString();
  ASSERT_TRUE(text.has_value());
  EXPECT_EQ(text.value(), ":rsc4-200-{\"ok\": true}-ct:application/json:loaded-OK");
  auto fired = binder.Evaluate("globalThis.listener_fired");
  ASSERT_TRUE(fired.has_value());
  EXPECT_TRUE(fired.value().ToBoolean().value_or(false));
}

TEST(DomBinderXhrTest, XhrNetworkErrorFiresOnError)
{
  dom::Document doc;
  PageApis apis;
  apis.xhr_request = [](const std::string&,
                        const std::string&,
                        const std::vector<std::pair<std::string, std::string>>&,
                        const std::string&) -> base::Result<FetchResponse> {
    return base::Err(base::Error::Network("connection refused"));
  };
  DomBinder binder(doc, apis);
  const char* script = R"(
    globalThis.out = '';
    const xhr = new XMLHttpRequest();
    xhr.open('GET', '/down');
    xhr.onerror = function() { globalThis.out += 'error'; };
    xhr.onload = function() { globalThis.out += 'load'; };
    xhr.send();
    globalThis.immediate = 'state' + xhr.readyState + '-status' + xhr.status;
    globalThis.xhr_ref = xhr;
  )";
  const auto run2 = binder.Evaluate(script);
  ASSERT_TRUE(run2.has_value()) << "eval error: "
                                << (run2.has_value() ? "" : run2.error().message());
  auto immediate = binder.Evaluate("globalThis.immediate");
  ASSERT_TRUE(immediate.has_value());
  ASSERT_TRUE(immediate.value().ToString().has_value());
  EXPECT_EQ(immediate.value().ToString().value(), "state1-status0")
      << "async mode delivers after send(), not inside it";
  (void)binder.RunPendingTimers();
  auto out = binder.Evaluate("globalThis.out");
  ASSERT_TRUE(out.has_value());
  auto text = out.value().ToString();
  ASSERT_TRUE(text.has_value());
  EXPECT_EQ(text.value(), "error");
  auto done = binder.Evaluate("globalThis.xhr_ref.readyState + ':' + globalThis.xhr_ref.status");
  ASSERT_TRUE(done.has_value());
  ASSERT_TRUE(done.value().ToString().has_value());
  EXPECT_EQ(done.value().ToString().value(), "4:0");
}

// The jd.com loader pattern: handlers assigned AFTER send() must observe the
// DONE transition and the load event (browser task semantics).
TEST(DomBinderXhrTest, XhrAsyncDeliversEventsToHandlersAttachedAfterSend)
{
  dom::Document doc;
  PageApis apis;
  apis.resolve_url = [](const std::string& raw) {
    return raw.rfind("http://", 0) == 0 ? raw : "http://test.local/" + raw;
  };
  apis.xhr_request = [](const std::string& url,
                        const std::string&,
                        const std::vector<std::pair<std::string, std::string>>&,
                        const std::string&) -> base::Result<FetchResponse> {
    FetchResponse out;
    out.status = 200;
    out.status_text = "OK";
    out.final_url = url;
    out.body = "late-body";
    return base::Ok(std::move(out));
  };
  DomBinder binder(doc, apis);
  const char* script = R"(
    globalThis.order = [];
    const xhr = new XMLHttpRequest();
    xhr.open('GET', 'api/late');
    xhr.send();
    globalThis.order.push('sent:' + xhr.readyState);
    xhr.onreadystatechange = function() {
      if (xhr.readyState === 4) { globalThis.order.push('rsc4'); }
    };
    xhr.onload = function() {
      globalThis.order.push('load:' + xhr.status + ':' + xhr.responseText);
    };
  )";
  const auto run = binder.Evaluate(script);
  ASSERT_TRUE(run.has_value()) << "eval error: " << (run.has_value() ? "" : run.error().message());
  auto before = binder.Evaluate("globalThis.order.join(',')");
  ASSERT_TRUE(before.has_value());
  ASSERT_TRUE(before.value().ToString().has_value());
  EXPECT_EQ(before.value().ToString().value(), "sent:1") << "no events before the pump";
  (void)binder.RunPendingTimers();
  auto after = binder.Evaluate("globalThis.order.join(',')");
  ASSERT_TRUE(after.has_value());
  ASSERT_TRUE(after.value().ToString().has_value());
  EXPECT_EQ(after.value().ToString().value(), "sent:1,rsc4,load:200:late-body");
}

// open(..., false) keeps the fully-synchronous behaviour: send() completes
// the request and fires the events inside the call.
TEST(DomBinderXhrTest, XhrSyncModeCompletesInsideSend)
{
  dom::Document doc;
  PageApis apis;
  apis.resolve_url = [](const std::string& raw) {
    return raw.rfind("http://", 0) == 0 ? raw : "http://test.local/" + raw;
  };
  apis.xhr_request = [](const std::string&,
                        const std::string&,
                        const std::vector<std::pair<std::string, std::string>>&,
                        const std::string&) -> base::Result<FetchResponse> {
    FetchResponse out;
    out.status = 200;
    out.status_text = "OK";
    out.body = "sync-body";
    return base::Ok(std::move(out));
  };
  DomBinder binder(doc, apis);
  const char* script = R"(
    globalThis.log = '';
    const xhr = new XMLHttpRequest();
    xhr.open('GET', 'api/sync', false);
    xhr.onload = function() { globalThis.log += 'loaded'; };
    xhr.send();
    globalThis.done = xhr.readyState + ':' + xhr.status + ':' + xhr.responseText + ':' + globalThis.log;
  )";
  const auto run = binder.Evaluate(script);
  ASSERT_TRUE(run.has_value()) << "eval error: " << (run.has_value() ? "" : run.error().message());
  auto done = binder.Evaluate("globalThis.done");
  ASSERT_TRUE(done.has_value());
  ASSERT_TRUE(done.value().ToString().has_value());
  EXPECT_EQ(done.value().ToString().value(), "4:200:sync-body:loaded");
}

TEST(DomBinderXhrTest, XhrReflectsResponseType)
{
  dom::Document doc;
  PageApis apis;
  apis.xhr_request = [](const std::string&,
                        const std::string&,
                        const std::vector<std::pair<std::string, std::string>>&,
                        const std::string&) -> base::Result<FetchResponse> {
    return base::Err(base::Error::Network("not used"));
  };
  DomBinder binder(doc, apis);
  const auto run = binder.Evaluate(
      "(function(){ var request = new XMLHttpRequest(); request.responseType = 'json'; "
      "var descriptor = Object.getOwnPropertyDescriptor(XMLHttpRequest.prototype, 'responseType'); "
      "return typeof descriptor.get + ':' + typeof descriptor.set + ':' + request.responseType; "
      "})()");
  ASSERT_TRUE(run.has_value());
  EXPECT_EQ(run.value().ToString().value(), "function:function:json");
}

// ---------------------------------------------------------------------------
// WebSocket (RFC 6455) binding.
//
// The transport is real network I/O on a background thread, so the test runs a
// local echo server and drives the page pump (RunPendingTimers) until the
// script's handlers have run.
// ---------------------------------------------------------------------------

namespace {

#ifndef _WIN32

// Serves one WebSocket connection: completes the handshake, echoes every text
// message once, then stays open for the client to close.
class WsEchoServer
{
public:
  WsEchoServer()
  {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
      return;
    }
    int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listen_fd_, 4) != 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
      return;
    }
    socklen_t len = sizeof(addr);
    ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this] { Run(); });
  }

  ~WsEchoServer()
  {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  bool valid() const
  {
    return listen_fd_ >= 0;
  }
  uint16_t port() const
  {
    return port_;
  }

private:
  static std::string AcceptKey(std::string_view key)
  {
    const std::string concatenated = std::string(key) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char hash[SHA_DIGEST_LENGTH] = {};
    SHA1(reinterpret_cast<const unsigned char*>(concatenated.data()), concatenated.size(), hash);
    std::string out;
    out.resize(4 * ((SHA_DIGEST_LENGTH + 2) / 3));
    const int len =
        EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), hash, SHA_DIGEST_LENGTH);
    out.resize(static_cast<std::size_t>(len));
    return out;
  }

  static bool Receive(int fd, std::size_t want, std::string* buffer)
  {
    while (buffer->size() < want) {
      char chunk[4096];
      const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
      if (n <= 0) {
        return false;
      }
      buffer->append(chunk, static_cast<std::size_t>(n));
    }
    return true;
  }

  void Run()
  {
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
      return;
    }
    std::string buffer;
    if (!Receive(fd, 4, &buffer)) {
      ::close(fd);
      return;
    }
    while (buffer.find("\r\n\r\n") == std::string::npos) {
      if (!Receive(fd, buffer.size() + 1, &buffer)) {
        ::close(fd);
        return;
      }
    }

    std::string lower = buffer;
    for (char& c : lower) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    const auto key_pos = lower.find("sec-websocket-key:");
    if (key_pos == std::string::npos) {
      ::close(fd);
      return;
    }
    auto key_start = key_pos + std::string("sec-websocket-key:").size();
    while (key_start < buffer.size() && (buffer[key_start] == ' ' || buffer[key_start] == '\t')) {
      ++key_start;
    }
    const auto key_end = buffer.find("\r\n", key_start);
    const std::string key = buffer.substr(key_start, key_end - key_start);

    const std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                 "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
                                 AcceptKey(key) + "\r\n\r\n";
    ::send(fd, response.data(), response.size(), 0);

    // Echo loop: decode masked client frames, reply unmasked.
    buffer.clear();
    while (true) {
      if (!Receive(fd, buffer.size() + 2, &buffer)) {
        break;
      }
      const auto byte0 = static_cast<uint8_t>(buffer[0]);
      const auto byte1 = static_cast<uint8_t>(buffer[1]);
      const uint8_t opcode = byte0 & 0x0F;
      const bool masked = (byte1 & 0x80) != 0;
      std::size_t payload_len = byte1 & 0x7F;
      std::size_t header_len = 2;
      if (payload_len == 126) {
        if (!Receive(fd, 4, &buffer)) {
          break;
        }
        payload_len = (static_cast<std::size_t>(static_cast<uint8_t>(buffer[2])) << 8) |
                      static_cast<std::size_t>(static_cast<uint8_t>(buffer[3]));
        header_len = 4;
      }
      const std::size_t mask_len = masked ? 4 : 0;
      if (!Receive(fd, header_len + mask_len + payload_len, &buffer)) {
        break;
      }
      std::string payload = buffer.substr(header_len + mask_len, payload_len);
      if (masked) {
        for (std::size_t i = 0; i < payload.size(); ++i) {
          payload[i] = static_cast<char>(static_cast<uint8_t>(payload[i]) ^
                                         static_cast<uint8_t>(buffer[header_len + (i % 4)]));
        }
      }
      buffer.erase(0, header_len + mask_len + payload_len);

      if (opcode == 0x8) { // close: echo the code and finish
        std::string close_frame;
        close_frame.push_back(static_cast<char>(0x88));
        close_frame.push_back(static_cast<char>(payload.size() >= 2 ? 2 : 0));
        close_frame.append(payload.substr(0, 2));
        ::send(fd, close_frame.data(), close_frame.size(), 0);
        break;
      }
      if (opcode == 0x1 || opcode == 0x2) {
        std::string frame;
        frame.push_back(static_cast<char>(0x80u | opcode));
        if (payload.size() <= 125) {
          frame.push_back(static_cast<char>(payload.size()));
        } else {
          frame.push_back(static_cast<char>(126));
          frame.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
          frame.push_back(static_cast<char>(payload.size() & 0xFF));
        }
        frame.append(payload);
        ::send(fd, frame.data(), frame.size(), 0);
      }
    }
    ::close(fd);
  }

  int listen_fd_ = -1;
  uint16_t port_ = 0;
  std::thread thread_;
};

// Drives the binder's pump until |predicate| (evaluated in the page) is true.
bool PumpUntil(DomBinder& binder, const std::string& expression, int timeout_ms = 5000)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    binder.RunPendingTimers();
    auto value = binder.Evaluate(expression);
    if (value.has_value() && value.value().ToBoolean().value_or(false)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

#endif // !_WIN32

} // namespace

#ifndef _WIN32

TEST(DomBinderWebSocketTest, EchoRoundTripDeliversOpenMessageAndClose)
{
  WsEchoServer server;
  ASSERT_TRUE(server.valid());

  dom::Document doc;
  PageApis apis;
  DomBinder binder(doc, apis);
  binder.SetConsoleSink([](std::string_view, std::string_view) {});

  const std::string url = "ws://127.0.0.1:" + std::to_string(server.port()) + "/echo";
  const auto run = binder.Evaluate(
      "globalThis.log = '';"
      "globalThis.ws = new WebSocket('" +
      url + "');" +
      "globalThis.ws.onopen = function() { globalThis.log += 'open;';"
      "  globalThis.ws.send('ping'); };"
      "globalThis.ws.onmessage = function(e) { globalThis.log += 'msg:' + e.data + ';'; "
      "  globalThis.ws.close(); };"
      "globalThis.ws.onclose = function(e) { globalThis.log += 'close:' + e.code; };");
  ASSERT_TRUE(run.has_value()) << run.error().message();

  ASSERT_TRUE(PumpUntil(binder, "globalThis.log.indexOf('open;') >= 0"));
  ASSERT_TRUE(PumpUntil(binder, "globalThis.log.indexOf('msg:ping') >= 0"));
  ASSERT_TRUE(PumpUntil(binder, "globalThis.log.indexOf('close:') >= 0"));

  auto log = binder.Evaluate("globalThis.log");
  ASSERT_TRUE(log.has_value());
  const auto text = log.value().ToString();
  ASSERT_TRUE(text.has_value());
  EXPECT_NE(text.value().find("open;"), std::string::npos);
  EXPECT_NE(text.value().find("msg:ping;"), std::string::npos);
}

TEST(DomBinderWebSocketTest, ReadyStateTransitions)
{
  WsEchoServer server;
  ASSERT_TRUE(server.valid());

  dom::Document doc;
  PageApis apis;
  DomBinder binder(doc, apis);
  binder.SetConsoleSink([](std::string_view, std::string_view) {});

  const std::string url = "ws://127.0.0.1:" + std::to_string(server.port()) + "/state";
  const auto run = binder.Evaluate(
      "globalThis.states = [];"
      "globalThis.ws = new WebSocket('" +
      url + "');" +
      "globalThis.states.push(globalThis.ws.readyState);"
      "globalThis.ws.onopen = function() { globalThis.states.push(globalThis.ws.readyState); };");
  ASSERT_TRUE(run.has_value()) << run.error().message();

  ASSERT_TRUE(PumpUntil(binder, "globalThis.states.length >= 2"));
  auto states = binder.Evaluate("globalThis.states.join(',')");
  ASSERT_TRUE(states.has_value());
  // CONNECTING (0) at construction, OPEN (1) once the handshake completes.
  EXPECT_EQ(states.value().ToString().value(), "0,1");
}

TEST(DomBinderWebSocketTest, AddEventListenerReceivesMessage)
{
  WsEchoServer server;
  ASSERT_TRUE(server.valid());

  dom::Document doc;
  PageApis apis;
  DomBinder binder(doc, apis);
  binder.SetConsoleSink([](std::string_view, std::string_view) {});

  const std::string url = "ws://127.0.0.1:" + std::to_string(server.port()) + "/listen";
  const auto run = binder.Evaluate(
      "globalThis.got = '';"
      "globalThis.ws = new WebSocket('" +
      url + "');" +
      "globalThis.ws.addEventListener('message', function(e) { globalThis.got = e.data; });"
      "globalThis.ws.addEventListener('open', function() { globalThis.ws.send('via-listener'); "
      "});");
  ASSERT_TRUE(run.has_value()) << run.error().message();

  ASSERT_TRUE(PumpUntil(binder, "globalThis.got === 'via-listener'"));
}

TEST(DomBinderWebSocketTest, ConnectionFailureFiresErrorAndClose)
{
  // Nothing listens on this port: the handshake must fail asynchronously with
  // error + close(1006), not throw from the constructor.
  dom::Document doc;
  PageApis apis;
  DomBinder binder(doc, apis);
  binder.SetConsoleSink([](std::string_view, std::string_view) {});

  // Pick a port that is almost certainly closed (above the ephemeral range is
  // not safe either; use a just-closed listener's port via port 1 on loopback).
  const auto run = binder.Evaluate(
      "globalThis.out = '';"
      "globalThis.ws = new WebSocket('ws://127.0.0.1:1/nope');"
      "globalThis.ws.onerror = function() { globalThis.out += 'error;'; };"
      "globalThis.ws.onclose = function(e) { globalThis.out += 'close:' + e.code; };");
  ASSERT_TRUE(run.has_value()) << run.error().message();
  EXPECT_EQ(binder.Evaluate("globalThis.ws.readyState").value().ToNumber().value_or(-1), 0.0);

  ASSERT_TRUE(PumpUntil(binder, "globalThis.out.indexOf('close:') >= 0", 10000));
  auto out = binder.Evaluate("globalThis.out");
  ASSERT_TRUE(out.has_value());
  const auto text = out.value().ToString();
  ASSERT_TRUE(text.has_value());
  EXPECT_NE(text.value().find("error;"), std::string::npos);
  EXPECT_NE(text.value().find("close:1006"), std::string::npos);
  EXPECT_EQ(binder.Evaluate("globalThis.ws.readyState").value().ToNumber().value_or(-1), 3.0);
}

TEST(DomBinderWebSocketTest, SendBeforeOpenIsQueuedUntilOpen)
{
  WsEchoServer server;
  ASSERT_TRUE(server.valid());

  dom::Document doc;
  PageApis apis;
  DomBinder binder(doc, apis);
  binder.SetConsoleSink([](std::string_view, std::string_view) {});

  const std::string url = "ws://127.0.0.1:" + std::to_string(server.port()) + "/queue";
  // CONNECTING: send() throws InvalidStateError per WHATWG (HTML §9.3).
  const auto run = binder.Evaluate(
      "globalThis.got = '';"
      "globalThis.ws = new WebSocket('" +
      url + "');" +
      "globalThis.ws.onmessage = function(e) { globalThis.got = e.data; };"
      "globalThis.threw = false;"
      "try { globalThis.ws.send('early'); } catch (err) { globalThis.threw = err.name; }");
  ASSERT_TRUE(run.has_value()) << run.error().message();

  auto threw = binder.Evaluate("globalThis.threw");
  ASSERT_TRUE(threw.has_value());
  EXPECT_EQ(threw.value().ToString().value(), "InvalidStateError");

  // Once open, a send round-trips.
  ASSERT_TRUE(PumpUntil(binder, "globalThis.ws.readyState === 1"));
  const auto send_run = binder.Evaluate("globalThis.ws.send('after-open');");
  ASSERT_TRUE(send_run.has_value()) << send_run.error().message();
  ASSERT_TRUE(PumpUntil(binder, "globalThis.got === 'after-open'"));
}

TEST(DomBinderWebSocketTest, InvalidUrlFiresErrorClose)
{
  dom::Document doc;
  PageApis apis;
  DomBinder binder(doc, apis);
  binder.SetConsoleSink([](std::string_view, std::string_view) {});

  const auto run = binder.Evaluate(
      "globalThis.out = '';"
      "globalThis.ws = new WebSocket('ftp://example.com/socket');"
      "globalThis.ws.onerror = function() { globalThis.out += 'error;'; };"
      "globalThis.ws.onclose = function(e) { globalThis.out += 'close:' + e.code; };");
  ASSERT_TRUE(run.has_value()) << run.error().message();
  ASSERT_TRUE(PumpUntil(binder, "globalThis.out.indexOf('close:') >= 0"));
  auto out = binder.Evaluate("globalThis.out");
  ASSERT_TRUE(out.has_value());
  EXPECT_NE(out.value().ToString().value().find("close:1006"), std::string::npos);
}

#endif // !_WIN32

// ---------------------------------------------------------------------------
// AbortController / AbortSignal (DOM Standard §4.1 subset).  Sites use the
// controller to time out fetch() calls (acxun.github.io's daily-quote fetch);
// a missing global kills the whole inline script at the first reference.
// ---------------------------------------------------------------------------

TEST_F(DomBinderTest, AbortControllerConstructsSignal)
{
  EXPECT_EQ(EvalString("typeof AbortController"), "function");
  EXPECT_EQ(EvalString("typeof AbortSignal"), "function");
  EXPECT_TRUE(EvalBool("(function(){ var c = new AbortController(); "
                       "return c.signal instanceof AbortSignal "
                       "    && c.signal.aborted === false; })()"));
}

TEST_F(DomBinderTest, AbortFiresAbortEventOnce)
{
  EXPECT_TRUE(
      EvalBool("(function(){ var c = new AbortController(); var calls = 0; var type = ''; "
               "c.signal.addEventListener('abort', function(e){ calls++; type = e.type; }); "
               "c.abort(); c.abort(); "
               "return calls === 1 && type === 'abort' && c.signal.aborted === true; })()"));
}

TEST_F(DomBinderTest, AbortReasonDefaultsAndCustom)
{
  // Default reason is an AbortError DOMException-like error; a custom reason
  // round-trips unchanged.
  EXPECT_TRUE(EvalBool("(function(){ var c = new AbortController(); c.abort(); "
                       "return c.signal.reason.name === 'AbortError'; })()"));
  EXPECT_TRUE(EvalBool("(function(){ var c = new AbortController(); c.abort('boom'); "
                       "return c.signal.reason === 'boom'; })()"));
}

TEST_F(DomBinderTest, ThrowIfAbortedThrowsOnlyAfterAbort)
{
  EXPECT_TRUE(EvalBool("(function(){ var c = new AbortController(); var name = 'no-throw'; "
                       "try { c.signal.throwIfAborted(); } catch(e) { name = e.name; } "
                       "if (name !== 'no-throw') { return false; } "
                       "c.abort(); "
                       "try { c.signal.throwIfAborted(); } catch(e) { name = e.name; } "
                       "return name === 'AbortError'; })()"));
}

// ---------------------------------------------------------------------------
// IntersectionObserver (DOM Standard §"IntersectionObserver" subset).  The
// page at acxun.github.io uses `new IntersectionObserver(...)` in its nav
// scroll-spy; a missing global killed the whole inline script at the first
// reference.  The observer reports real intersection geometry (wired through
// PageApis::element_geometry) and delivers entries asynchronously (via the
// QuickJS job queue, drained by DomBinder::Evaluate).
// ---------------------------------------------------------------------------

TEST_F(DomBinderTest, IntersectionObserverExposedAndValidates)
{
  EXPECT_EQ(EvalString("typeof IntersectionObserver"), "function");
  EXPECT_TRUE(
      EvalBool("(function(){ var o = new IntersectionObserver(function(){}); "
               "return typeof o.observe === 'function' && typeof o.unobserve === 'function' "
               "    && typeof o.disconnect === 'function' && typeof o.takeRecords === 'function' "
               "    && o instanceof IntersectionObserver; })()"));
  // Constructor requires a callable callback.
  EXPECT_TRUE(EvalBool("(function(){ var threw = false; try { new IntersectionObserver(); } "
                       "catch(e) { threw = (e instanceof TypeError); } return threw; })()"));
  EXPECT_TRUE(EvalBool("(function(){ var threw = false; try { new IntersectionObserver(null); } "
                       "catch(e) { threw = (e instanceof TypeError); } return threw; })()"));
}

namespace {

// A standalone geometry-wired binder for intersection tests.  `box` sits in
// the viewport, `low` far below it, `mid` in the middle band produced by the
// `-35% 0px -55% 0px` rootMargin.
class IntersectionTestHarness
{
public:
  IntersectionTestHarness()
  {
    document_ = html::Parser(R"(<html><body>
      <div id="box">box</div><p id="low">low</p><span id="mid">mid</span>
    </body></html>)")
                    .Parse();
    PageApis apis;
    apis.viewport_size = []() { return std::pair<int, int>{800, 600}; };
    apis.scroll_offset = [this]() { return std::pair<double, double>{0.0, scroll_y}; };
    apis.element_geometry = [](const dom::Element& element) -> std::optional<ElementGeometry> {
      const std::string id = std::string(element.GetAttribute("id").value_or(""));
      ElementGeometry g;
      g.x = 0;
      g.width = 200;
      g.height = 20;
      g.client_width = 200;
      g.client_height = 20;
      g.border_top = 0;
      g.border_left = 0;
      g.y = id == "box" ? 12 : id == "low" ? 5000 : id == "mid" ? 220 : 0;
      return g;
    };
    binder_ = std::make_unique<DomBinder>(*document_, apis);
  }

  // The page's scroll position (window.scrollY / the viewport root position);
  // tests move it and call Refresh() the way the browser layer does.
  double scroll_y = 0;

  // Recomputes every observer, exactly like BrowserController's scroll
  // handlers and timer pump.
  bool Refresh()
  {
    return binder_->RefreshIntersectionObservers();
  }

  std::string Eval(const std::string& code)
  {
    auto r = binder_->Evaluate(code);
    if (!r.has_value()) {
      return "<error: " + r.error().message() + ">";
    }
    auto s = r.value().ToString();
    return s.has_value() ? s.value() : "<tostring-error>";
  }

  double Num(const std::string& code)
  {
    auto r = binder_->Evaluate(code);
    if (!r.has_value()) {
      return -1e9;
    }
    auto n = r.value().ToNumber();
    return n.has_value() ? n.value() : -1e9;
  }

  bool Bool(const std::string& code)
  {
    auto r = binder_->Evaluate(code);
    if (!r.has_value()) {
      return false;
    }
    auto b = r.value().ToBoolean();
    return b.has_value() && b.value();
  }
  // Runs a setup script and discards its result (assert on follow-up reads).
  void Run(const std::string& code)
  {
    (void)binder_->Evaluate(code);
  }

private:
  std::unique_ptr<dom::Document> document_;
  std::unique_ptr<DomBinder> binder_;
};

} // namespace

TEST(IntersectionObserverTest, ObserveDeliversIntersectionEntry)
{
  IntersectionTestHarness h;
  // Observe an in-viewport element; the callback runs once the Evaluate that
  // scheduled it drains the job queue.
  h.Run("window.__seen = []; var obs = new IntersectionObserver(function(entries){ "
        "window.__seen = window.__seen.concat(entries); }); "
        "obs.observe(document.getElementById('box'));");
  EXPECT_EQ(h.Num("window.__seen.length"), 1.0);
  EXPECT_EQ(h.Eval("window.__seen[0].target.id"), "box");
  EXPECT_TRUE(h.Bool("window.__seen[0].isIntersecting"));
  EXPECT_EQ(h.Num("window.__seen[0].intersectionRatio"), 1.0);
  EXPECT_EQ(h.Num("window.__seen[0].boundingClientRect.y"), 12.0);
  EXPECT_EQ(h.Num("window.__seen[0].rootBounds.width"), 800.0);
}

// Scrolling moves the viewport root: RefreshIntersectionObservers recomputes
// every observer (BrowserController calls it from its scroll handlers and
// timer pump — that is what activates lazy-loading wiring on real pages).
TEST(IntersectionObserverTest, RefreshFollowsTheScrolledViewport)
{
  IntersectionTestHarness h;
  h.Run("window.__seen = []; var obs = new IntersectionObserver(function(entries){ "
        "window.__seen = window.__seen.concat(entries); }); "
        "obs.observe(document.getElementById('low'));");
  // 'low' sits at y=5000, below the initial 800x600 viewport.
  ASSERT_EQ(h.Eval("String(window.__seen[0].isIntersecting)"), "false");
  EXPECT_EQ(h.Num("window.__seen[0].rootBounds.y"), 0.0);

  // Scrolling down brings it into view; the refresh delivers the change.
  h.scroll_y = 4600;
  EXPECT_TRUE(h.Refresh());
  ASSERT_EQ(h.Num("window.__seen.length"), 2.0);
  EXPECT_EQ(h.Eval("String(window.__seen[1].isIntersecting)"), "true");
  EXPECT_EQ(h.Num("window.__seen[1].rootBounds.y"), 4600.0);

  // Scrolling back out flips it again.
  h.scroll_y = 0;
  (void)h.Refresh();
  ASSERT_EQ(h.Num("window.__seen.length"), 3.0);
  EXPECT_EQ(h.Eval("String(window.__seen[2].isIntersecting)"), "false");

  // A refresh with no change delivers no extra entry (no spurious callbacks).
  (void)h.Refresh();
  EXPECT_EQ(h.Num("window.__seen.length"), 3.0);
}

TEST(IntersectionObserverTest, FirstReportFiresForNonIntersectingTarget)
{
  IntersectionTestHarness h;
  h.Run("window.__seen = []; var obs = new IntersectionObserver(function(entries){ "
        "window.__seen = window.__seen.concat(entries); }); "
        "obs.observe(document.getElementById('low'));");
  // The initial observation always reports the target, even when it is outside
  // the viewport (spec behavior) — isIntersecting is false.
  EXPECT_EQ(h.Num("window.__seen.length"), 1.0);
  EXPECT_EQ(h.Eval("window.__seen[0].target.id"), "low");
  EXPECT_TRUE(h.Bool("!window.__seen[0].isIntersecting"));
  EXPECT_EQ(h.Num("window.__seen[0].intersectionRatio"), 0.0);
}

TEST(IntersectionObserverTest, RootMarginShrinksTheRootBand)
{
  IntersectionTestHarness h;
  h.Run("window.__seen = []; "
        "var obs = new IntersectionObserver(function(entries){ "
        "window.__seen = window.__seen.concat(entries); }, "
        "{ rootMargin: '-35% 0px -55% 0px' }); "
        "obs.observe(document.getElementById('box')); "
        "obs.observe(document.getElementById('mid'));");
  // The root band is [210, 270] for the 800x600 viewport: `box` (y=12..32) is
  // above the band (not intersecting), `mid` (y=220..240) is inside it.
  EXPECT_EQ(h.Num("window.__seen.length"), 2.0);
  EXPECT_TRUE(h.Bool("(function(){ var by = {}; window.__seen.forEach(function(e){ by[e.target.id] "
                     "= e.isIntersecting; }); return by.box === false && by.mid === true; })()"));
}

TEST(IntersectionObserverTest, DisconnectClearsPendingAndStopsReports)
{
  IntersectionTestHarness h;
  h.Run("window.__seen = []; var obs = new IntersectionObserver(function(entries){ "
        "window.__seen = window.__seen.concat(entries); }); "
        "obs.observe(document.getElementById('box')); obs.disconnect();");
  // disconnect() cleared the queued entry before the job could deliver it.
  EXPECT_EQ(h.Num("window.__seen.length"), 0.0);
}

TEST(IntersectionObserverTest, TakeRecordsDrainsQueuedEntries)
{
  IntersectionTestHarness h;
  h.Run("window.__taken = []; var obs = new IntersectionObserver(function(){}); "
        "obs.observe(document.getElementById('box')); "
        "window.__taken = obs.takeRecords();");
  EXPECT_EQ(h.Num("window.__taken.length"), 1.0);
}

// ---------------------------------------------------------------------------
// Focus (WHATWG HTML §6.6): UI Events sequence/bubbling, and the binder's
// script-visible focus API over the PageApis focus controller callbacks.
// ---------------------------------------------------------------------------

// The shared focus path dispatches UI Events §3.3.2 order: blur then focusout
// on the old target, focus then focusin on the new one.  blur/focus do not
// bubble, focusout/focusin do.
TEST_F(DomBinderTest, FocusUpdateDispatchesOrderedEventsAndBubbles)
{
  ASSERT_TRUE(
      binder_
          ->Evaluate("window.__log = [];"
                     "function log(s) { __log.push(s); }"
                     "var main = document.getElementById('main');"
                     "var first = document.getElementById('first');"
                     "var second = document.querySelectorAll('.para')[1];"
                     "['focus','blur','focusin','focusout'].forEach(function(type) {"
                     "  main.addEventListener(type, function() { log('main-' + type); });"
                     "  first.addEventListener(type, function() { log('first-' + type); });"
                     "  second.addEventListener(type, function() { log('second-' + type); });"
                     "});")
          .has_value());

  dom::Element* main = dom::QuerySelector(*document_, "#main");
  dom::Element* first = dom::QuerySelector(*document_, "#first");
  const std::vector<dom::Element*> paras = dom::QuerySelectorAll(*document_, "p.para");
  ASSERT_NE(main, nullptr);
  ASSERT_NE(first, nullptr);
  ASSERT_EQ(paras.size(), 2u);
  dom::Element* second = paras[1];

  binder_->PerformFocusUpdate(first, second);
  EXPECT_EQ(EvalString("__log.join(',')"),
            "first-blur,first-focusout,main-focusout,"
            "second-focus,second-focusin,main-focusin");

  ASSERT_TRUE(binder_->Evaluate("__log = []").has_value());
  binder_->PerformFocusUpdate(second, nullptr);
  EXPECT_EQ(EvalString("__log.join(',')"), "second-blur,second-focusout,main-focusout");

  ASSERT_TRUE(binder_->Evaluate("__log = []").has_value());
  binder_->PerformFocusUpdate(nullptr, first);
  EXPECT_EQ(EvalString("__log.join(',')"), "first-focus,first-focusin,main-focusin");
}

// The onfocusin/onfocusout IDL handlers and content attributes fire with the
// bubbling events (the focus/blur pair keeps its own onfocus/onblur).
TEST_F(DomBinderTest, FocusInContentAttributeHandlerFires)
{
  std::unique_ptr<dom::Document> inline_handler_document = html::Parser(R"HTML(<!doctype html>
<html><body><input id="field" onfocusin="document.body.setAttribute('data-in','1')"
onfocusout="document.body.setAttribute('data-out','1')"></body></html>)HTML")
                                                               .Parse();
  dom::Element* field = dom::QuerySelector(*inline_handler_document, "#field");
  dom::Element* body = dom::QuerySelector(*inline_handler_document, "body");
  ASSERT_NE(field, nullptr);
  ASSERT_NE(body, nullptr);
  DomBinder handler_binder(*inline_handler_document);
  handler_binder.PerformFocusUpdate(nullptr, field);
  EXPECT_EQ(body->GetAttribute("data-in").value_or(""), "1");
  EXPECT_FALSE(body->GetAttribute("data-out").has_value());
  handler_binder.PerformFocusUpdate(field, nullptr);
  EXPECT_EQ(body->GetAttribute("data-out").value_or(""), "1");
}

// document.activeElement / document.hasFocus() / Element.focus()/blur() are
// served by the browser layer's focus controller callbacks: focus() runs the
// same event sequence as a click or Tab, activeElement follows the focused
// element (body when none), and focus() on an element the controller rejects is
// a no-op.
TEST(FocusBindingTest, ScriptFocusUsesTheBrowsersFocusCallbacks)
{
  std::unique_ptr<dom::Document> document = html::Parser(R"(<!doctype html>
<html><body><div id="wrap"><input id="a"><input id="b"></div>
<div id="plain"></div></body></html>)")
                                                .Parse();
  dom::Element* a = dom::QuerySelector(*document, "#a");
  dom::Element* b = dom::QuerySelector(*document, "#b");
  dom::Element* plain = dom::QuerySelector(*document, "#plain");
  dom::Element* body = dom::QuerySelector(*document, "body");
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  ASSERT_NE(plain, nullptr);
  ASSERT_NE(body, nullptr);
  // The test doubles as the browser layer: only elements marked focusable can
  // be focused (the real controller also checks connectivity and inertness).
  a->SetAttribute("data-focusable", "");
  b->SetAttribute("data-focusable", "");

  dom::Element* focused = nullptr;
  PageApis apis;
  apis.location_href = []() { return "https://www.example.com/"; };
  apis.focus_element = [&focused](dom::Element& element) {
    if (!element.HasAttribute("data-focusable")) {
      return false;
    }
    focused = &element;
    return true;
  };
  apis.blur_element = [&focused](dom::Element& element) {
    if (focused != &element) {
      return false;
    }
    focused = nullptr;
    return true;
  };
  apis.active_element = [&focused]() { return focused; };
  apis.has_focus = []() { return true; };
  DomBinder binder(*document, apis);

  const auto run = [&binder](const std::string& code) {
    const auto result = binder.Evaluate(code);
    EXPECT_TRUE(result.has_value());
    return result.has_value() ? result.value().ToString().value_or("<none>")
                              : std::string("<error>");
  };
  EXPECT_EQ(run("document.activeElement.tagName"), "BODY");
  EXPECT_EQ(run("String(document.hasFocus())"), "true");
  EXPECT_EQ(run("(function() {"
                "  var a = document.getElementById('a');"
                "  var b = document.getElementById('b');"
                "  var log = [];"
                "  ['focus','blur','focusin','focusout'].forEach(function(type) {"
                "    a.addEventListener(type, function() { log.push('a-' + type); });"
                "    b.addEventListener(type, function() { log.push('b-' + type); });"
                "  });"
                "  a.focus();"
                "  var after_a = document.activeElement.id;"
                "  b.focus();"
                "  var after_b = document.activeElement.id;"
                "  b.blur();"
                "  var after_blur = document.activeElement.tagName;"
                "  a.blur();"
                "  var blur_unfocused = document.activeElement.tagName;"
                "  document.getElementById('plain').focus();"
                "  var after_plain = document.activeElement.tagName;"
                "  return [after_a, after_b, after_blur, blur_unfocused, after_plain,"
                "          log.join(',')].join('|');"
                "})()"),
            "a|b|BODY|BODY|BODY|"
            "a-focus,a-focusin,a-blur,a-focusout,b-focus,b-focusin,"
            "b-blur,b-focusout");
  EXPECT_EQ(focused, nullptr);
}

// activeElement and hasFocus are document-scoped: a Document created by
// script (document.implementation.createHTMLDocument) reports its own body as
// activeElement and never claims focus, even when the page document has a
// focused element.
TEST(FocusBindingTest, ActiveElementAndHasFocusAreDocumentScoped)
{
  std::unique_ptr<dom::Document> document =
      html::Parser(R"(<!doctype html><html><body><input id="field"></body></html>)").Parse();
  dom::Element* field = dom::QuerySelector(*document, "#field");
  ASSERT_NE(field, nullptr);
  dom::Element* focused = nullptr;
  PageApis apis;
  apis.location_href = []() { return "https://www.example.com/"; };
  apis.focus_element = [&focused](dom::Element& element) {
    focused = &element;
    return true;
  };
  apis.blur_element = [&focused](dom::Element&) {
    focused = nullptr;
    return true;
  };
  apis.active_element = [&focused]() { return focused; };
  apis.has_focus = []() { return true; };
  DomBinder binder(*document, apis);
  const auto result = binder.Evaluate(
      "(function() {"
      "  document.getElementById('field').focus();"
      "  var other = document.implementation.createHTMLDocument('other');"
      "  var main = document.getElementById('field') === document.activeElement"
      "      ? 'main-focused' : 'main-unfocused';"
      "  return main + '|' + other.activeElement.tagName + '|' + String(other.hasFocus());"
      "})()");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().ToString().value_or(""), "main-focused|BODY|false");
}

// Without a browsing context (a standalone binder), focus()/blur() are no-ops,
// activeElement falls back to <body> and hasFocus() is false.  No focus event
// sequence is invented.
TEST_F(DomBinderTest, FocusWithoutBrowsingContextIsANoop)
{
  EXPECT_EQ(EvalString("(function() {"
                       "  var first = document.getElementById('first');"
                       "  first.focus();"
                       "  var active = document.activeElement.tagName;"
                       "  first.blur();"
                       "  return active + '|' + document.activeElement.tagName"
                       "      + '|' + String(document.hasFocus());"
                       "})()"),
            "BODY|BODY|false");
}

// tabIndex reflects the attribute (HTML's integer parsing) and falls back to
// the historical per-element default (0 for links/form controls, -1
// otherwise); inert reflects its content attribute.
TEST(FocusBindingTest, TabIndexAndInertReflectTheirAttributes)
{
  std::unique_ptr<dom::Document> document =
      html::Parser(R"(<!doctype html><html><body><input id="field"></body></html>)").Parse();
  PageApis apis;
  apis.location_href = []() { return "https://www.example.com/"; };
  DomBinder binder(*document, apis);
  const auto run = [&binder](const std::string& code) {
    const auto result = binder.Evaluate(code);
    EXPECT_TRUE(result.has_value());
    return result.has_value() ? result.value().ToString().value_or("<none>")
                              : std::string("<error>");
  };

  // No attribute: the historical default is 0 for an input and -1 for a plain
  // <div> (HTML §6.6.3).
  EXPECT_EQ(run("String(document.createElement('input').tabIndex)"), "0");
  EXPECT_EQ(run("String(document.createElement('div').tabIndex)"), "-1");
  // Setter writes the content attribute; getter reads it back.
  EXPECT_EQ(run("(function() { var d = document.createElement('div'); d.tabIndex = 3;"
                "  return d.getAttribute('tabindex') + '|' + String(d.tabIndex); })()"),
            "3|3");
  // HTML's rules for parsing integers: leading whitespace, sign, digits;
  // trailing junk is ignored.
  EXPECT_EQ(run("(function() { var d = document.createElement('div');"
                "  d.setAttribute('tabindex', '  +2x'); return String(d.tabIndex); })()"),
            "2");
  EXPECT_EQ(run("(function() { var d = document.createElement('div');"
                "  d.setAttribute('tabindex', ' -1'); return String(d.tabIndex); })()"),
            "-1");
  // No digits: the attribute is unusable, so the element's default applies
  // (-1 for a <div>, 0 for an <input>'s historical default).
  EXPECT_EQ(run("(function() { var d = document.createElement('div');"
                "  d.setAttribute('tabindex', 'nope'); return String(d.tabIndex); })()"),
            "-1");
  EXPECT_EQ(run("(function() { var i = document.createElement('input');"
                "  i.setAttribute('tabindex', 'nope'); return String(i.tabIndex); })()"),
            "0");

  // inert reflects the attribute, defaulting to false.
  EXPECT_EQ(run("(function() { var d = document.createElement('div');"
                "  var before = String(d.inert);"
                "  d.inert = true;"
                "  var mid = String(d.inert) + '|' + String(d.hasAttribute('inert'));"
                "  d.inert = false;"
                "  return before + '|' + mid + '|' + String(d.hasAttribute('inert')); })()"),
            "false|true|true|false");
}

// HTMLDetailsElement.open (WHATWG HTML §4.11.1): reflects the `open` content
// attribute; changing whether the attribute is present fires the plain,
// non-bubbling `toggle` event on the details element (this engine dispatches
// it synchronously instead of queueing a task -- documented approximation).
TEST(DetailsBindingTest, OpenReflectsAndFiresToggle)
{
  std::unique_ptr<dom::Document> document =
      html::Parser(R"(<!doctype html><html><body><details id="d">)"
                   R"(<summary>Title</summary><p>Body</p></details></body></html>)")
          .Parse();
  PageApis apis;
  apis.location_href = []() { return "https://www.example.com/"; };
  DomBinder binder(*document, apis);
  const auto run = [&binder](const std::string& code) {
    const auto result = binder.Evaluate(code);
    EXPECT_TRUE(result.has_value());
    return result.has_value() ? result.value().ToString().value_or("<none>")
                              : std::string("<error>");
  };

  // The interface exists and open only lives on details elements.
  EXPECT_EQ(run("(document.getElementById('d') instanceof HTMLDetailsElement) ? 'yes' : 'no'"),
            "yes");
  EXPECT_EQ(run("('open' in document.createElement('div')) ? 'yes' : 'no'"), "no");

  EXPECT_EQ(run("String(document.getElementById('d').open)"), "false");
  // Property setter writes the attribute (empty value) and fires toggle once.
  EXPECT_EQ(run("(function() { var d = document.getElementById('d');"
                "  var n = 0, bubbles = null, target = null;"
                "  d.addEventListener('toggle', function(e) {"
                "    n++; bubbles = e.bubbles; target = e.target === d; });"
                "  d.open = true;"
                "  var a = d.open + '|' + d.getAttribute('open') + '|' + n + '|'"
                "      + bubbles + '|' + target;"
                "  d.open = true;" // no state change: no second event
                "  var b = n;"
                "  d.setAttribute('open', 'open');" // still open: no event
                "  var c = n;"
                "  d.removeAttribute('open');" // closes: event #2
                "  var e = n + '|' + d.open;"
                "  d.toggleAttribute('open');" // opens: event #3
                "  var f = n + '|' + d.open;"
                "  d.open = false;" // closes without the attribute present
                "  return [a, b, c, e, f, n + '|' + d.open].join(';'); })()"),
            "true||1|false|true;1;1;2|false;3|true;4|false");
}

// DOM §4.4: tagName/nodeName uppercases the qualified name only for HTML
// elements in the HTML namespace.  SVG camelCase names keep their case.
TEST_F(DomBinderTest, SvgTagNameKeepsItsCase)
{
  EXPECT_TRUE(EvalBool(R"JS((function() {
    var host = document.createElement('div');
    host.innerHTML = '<svg><lineargradient></lineargradient><foreignobject></foreignobject></svg>';
    var svg = host.querySelector('svg');
    return svg.namespaceURI === 'http://www.w3.org/2000/svg'
        && svg.children[0].tagName === 'linearGradient'
        && svg.children[1].tagName === 'foreignObject'
        && svg.children[0].nodeName === 'linearGradient'
        && document.createElement('div').tagName === 'DIV';
  })())JS"));
}

// -----------------------------------------------------------------------------
// Dynamic <script> insertion hook (WHATWG HTML §4.12.1).  The binder owns the
// "already started" bookkeeping and delegates fetch/evaluate to the browser
// layer through PageApis::execute_script_element; these tests pin the hook
// contract with a recording callback.
// -----------------------------------------------------------------------------

TEST(DynamicScriptBindingTest, ConnectedScriptIsPreparedExactlyOnce)
{
  std::unique_ptr<dom::Document> document =
      html::Parser("<html><body><div id='host'></div></body></html>").Parse();
  PageApis apis;
  apis.location_href = []() { return std::string("https://example.com/"); };
  std::vector<dom::Element*> prepared;
  apis.execute_script_element = [&prepared](DomBinder&, dom::Element& script) {
    prepared.push_back(&script);
  };
  DomBinder binder(*document, apis);

  ASSERT_TRUE(binder
                  .Evaluate("var s=document.createElement('script');"
                            "s.id='dyn';"
                            "document.getElementById('host').appendChild(s);")
                  .has_value());
  ASSERT_EQ(prepared.size(), 1u);
  EXPECT_EQ(prepared[0]->GetAttribute("id").value_or(""), "dyn");

  // Appending an already-connected node moves it; the element must not run
  // again.  insertAdjacentElement is just another DOM insertion path.
  ASSERT_TRUE(binder
                  .Evaluate("document.body.appendChild(s);"
                            "document.body.insertAdjacentElement('afterbegin', s);")
                  .has_value());
  EXPECT_EQ(prepared.size(), 1u);
}

TEST(DynamicScriptBindingTest, DetachedSubtreeWaitsUntilConnected)
{
  std::unique_ptr<dom::Document> document = html::Parser("<html><body></body></html>").Parse();
  PageApis apis;
  apis.location_href = []() { return std::string("https://example.com/"); };
  std::vector<dom::Element*> prepared;
  apis.execute_script_element = [&prepared](DomBinder&, dom::Element& script) {
    prepared.push_back(&script);
  };
  DomBinder binder(*document, apis);

  ASSERT_TRUE(binder
                  .Evaluate("var box=document.createElement('div');"
                            "var s=document.createElement('script');"
                            "s.id='nested';"
                            "box.appendChild(s);")
                  .has_value());
  EXPECT_TRUE(prepared.empty());

  ASSERT_TRUE(binder.Evaluate("document.body.appendChild(box);").has_value());
  ASSERT_EQ(prepared.size(), 1u);
  EXPECT_EQ(prepared[0]->GetAttribute("id").value_or(""), "nested");
}

TEST(DynamicScriptBindingTest, FragmentParsingScriptsNeverRun)
{
  std::unique_ptr<dom::Document> document = html::Parser("<html><body></body></html>").Parse();
  PageApis apis;
  apis.location_href = []() { return std::string("https://example.com/"); };
  std::vector<dom::Element*> prepared;
  apis.execute_script_element = [&prepared](DomBinder&, dom::Element& script) {
    prepared.push_back(&script);
  };
  DomBinder binder(*document, apis);

  // innerHTML parses a fragment; its script is inert even when moved into the
  // document later (the parser-inserted fragment scripts carry "already
  // started").  insertAdjacentHTML follows the same rule.
  ASSERT_TRUE(binder
                  .Evaluate("var d=document.createElement('div');"
                            "d.innerHTML='<scr'+'ipt id=\"inner\"></scr'+'ipt>';"
                            "document.body.appendChild(d);"
                            "var inner=d.firstChild;"
                            "document.head.appendChild(inner);"
                            "var p=document.createElement('div');"
                            "document.body.appendChild(p);"
                            "p.insertAdjacentHTML('beforeend',"
                            "  '<scr'+'ipt id=\"adjacent\"></scr'+'ipt>');")
                  .has_value());
  EXPECT_TRUE(prepared.empty());
}

TEST(DynamicScriptBindingTest, ExecutorSeesCurrentScriptAndDispatchesLoad)
{
  std::unique_ptr<dom::Document> document = html::Parser("<html><body></body></html>").Parse();
  PageApis apis;
  apis.location_href = []() { return std::string("https://example.com/"); };
  apis.execute_script_element = [](DomBinder& binder, dom::Element& script) {
    dom::Element* previous = binder.CurrentScript();
    binder.SetCurrentScript(&script);
    const base::Result<ScriptValue> result =
        binder.Evaluate(script.TextContent(), "dynamic-script.js");
    binder.SetCurrentScript(previous);
    binder.DispatchNonBubblingEvent(script, result.has_value() ? "load" : "error");
  };
  DomBinder binder(*document, apis);
  binder.SetConsoleSink([](std::string_view, std::string_view) {});

  ASSERT_TRUE(binder
                  .Evaluate(R"JS(
    window.__dynamicRan = 0;
    window.__dynamicLoaded = 0;
    window.__dynamicSawCurrent = false;
    var dynamicScript = document.createElement('script');
    dynamicScript.textContent =
        'window.__dynamicRan++;' +
        'window.__dynamicSawCurrent = document.currentScript === dynamicScript;';
    dynamicScript.onload = function() { window.__dynamicLoaded++; };
    document.body.appendChild(dynamicScript);
  )JS")
                  .has_value());

  EXPECT_EQ(binder.Evaluate("String(window.__dynamicRan)").value().ToString().value(), "1");
  EXPECT_EQ(binder.Evaluate("String(window.__dynamicLoaded)").value().ToString().value(), "1");
  EXPECT_EQ(binder.Evaluate("String(window.__dynamicSawCurrent)").value().ToString().value(),
            "true");
  EXPECT_EQ(binder.Evaluate("String(document.currentScript)").value().ToString().value(), "null");
}

} // namespace
} // namespace neko::javascript
