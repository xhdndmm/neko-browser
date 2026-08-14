// Unit tests for neko::javascript::DomBinder (DOM bindings, page <script>
// execution support, and the minimal event loop).

#include "neko/base/status.h"
#include "neko/dom/element.h"
#include "neko/dom/query.h"
#include "neko/html/parser.h"
#include "neko/javascript/dom_binding.h"

#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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
    binder_ = std::make_unique<DomBinder>(*document_);
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
  EXPECT_EQ(EvalString("document.documentElement.tagName"), "HTML");
  EXPECT_EQ(EvalString("document.body.tagName"), "BODY");
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

TEST_F(DomBinderTest, DocumentReadyState)
{
  // Scripts run after parsing, so the document is always "complete".
  EXPECT_EQ(EvalString("document.readyState"), "complete");
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
}

TEST_F(DomBinderTest, InnerHTMLGetAndSet)
{
  EXPECT_EQ(EvalString("document.getElementById('first').innerHTML"), "Hello <b>world</b>");
  EXPECT_TRUE(EvalBool("(function(){ var e = document.getElementById('main'); "
                       "e.innerHTML = '<ul><li>a</li><li>b</li></ul>'; "
                       "return e.children.length === 1 && e.children[0].tagName === 'UL' "
                       "       && e.children[0].children.length === 2; })()"));
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
  // a ReferenceError.
  EXPECT_EQ(EvalString("navigator.userAgent"), "neko-browser/0.1.0");
  EXPECT_EQ(EvalString("window.navigator.userAgent"), "neko-browser/0.1.0");
  EXPECT_TRUE(EvalBool("navigator === window.navigator"));
  EXPECT_EQ(EvalString("navigator.language"), "en-US");
  EXPECT_EQ(EvalString("navigator.languages.join(',')"), "en-US");
  EXPECT_TRUE(EvalBool("navigator.onLine === true"));
  EXPECT_TRUE(EvalBool("navigator.cookieEnabled === true"));
  EXPECT_TRUE(EvalNumber("navigator.hardwareConcurrency") >= 1.0);
  // Missing features are absent, so "x" in navigator is honestly false.
  EXPECT_FALSE(EvalBool("'geolocation' in navigator"));
  EXPECT_FALSE(EvalBool("'clipboard' in navigator"));
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

} // namespace
} // namespace neko::javascript
