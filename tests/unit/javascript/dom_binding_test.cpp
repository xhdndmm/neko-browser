// Unit tests for neko::javascript::DomBinder (DOM bindings, page <script>
// execution support, and the minimal event loop).

#include "neko/base/status.h"
#include "neko/dom/element.h"
#include "neko/dom/query.h"
#include "neko/html/parser.h"
#include "neko/javascript/dom_binding.h"
#include "neko/storage/indexed_db.h"

#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
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
  EXPECT_TRUE(EvalBool("document.location === window.location"));
}

TEST_F(DomBinderTest, BlobAndObjectUrl)
{
  EXPECT_TRUE(EvalBool("(function(){ var blob = new Blob(['hello', ' world'], {type:'text/plain'}); "
                       "var url = URL.createObjectURL(blob); return blob.size === 11 && "
                       "blob.type === 'text/plain' && typeof url === 'string' && "
                       "url.indexOf('blob:') === 0; })()"));
}

TEST_F(DomBinderTest, NodeListCollectionSemantics)
{
  EXPECT_TRUE(EvalBool("typeof NodeList === 'function'"));
  EXPECT_TRUE(EvalBool("(function(){ var list = document.querySelectorAll('p'); return list instanceof NodeList; })()"));
  EXPECT_TRUE(EvalBool("(function(){ var list = document.querySelectorAll('p'); return typeof list.item === 'function'; })()"));
  EXPECT_TRUE(EvalBool("(function(){ var list = document.querySelectorAll('p'); return list.length > 0; })()"));
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

TEST_F(DomBinderTest, DocumentReadyState)
{
  // Scripts run after parsing, so the document is always "complete".
  EXPECT_EQ(EvalString("document.readyState"), "complete");
}

TEST_F(DomBinderTest, DocumentImplementationSupportsBasicFeatures)
{
  EXPECT_TRUE(EvalBool("document.implementation !== null && typeof document.implementation === 'object'"));
  EXPECT_TRUE(EvalBool("document.implementation.hasFeature('HTML', '1.0')"));
  EXPECT_TRUE(EvalBool("document.implementation.hasFeature('DOM', '1.0')"));
  EXPECT_FALSE(EvalBool("document.implementation.hasFeature('NoSuchFeature', '1.0')"));

  EXPECT_TRUE(EvalBool("(function(){ var d = document.implementation.createHTMLDocument('Doc Title'); "
                       "return d instanceof Document && d.title === 'Doc Title'; })()"));
}

TEST_F(DomBinderTest, JQueryCompatibilityAliasesAreDefined)
{
  EXPECT_TRUE(EvalBool("typeof $ === 'function' && typeof jQuery === 'function'"));
  EXPECT_TRUE(EvalBool("(function(){ var called = false; $.ready = function(fn){ called = typeof fn === 'function'; }; $.ready(function(){}); return called && typeof jQuery.ready === 'function'; })()"));
}

TEST_F(DomBinderTest, LegacyBootstrapCompatibilityGlobals)
{
  EXPECT_TRUE(EvalBool("_w === window && _d === document"));
  EXPECT_TRUE(EvalBool("typeof PerformanceObserver === 'function'"));
  EXPECT_TRUE(EvalBool("document.visibilityState === 'visible'"));
  EXPECT_TRUE(EvalBool("navigator.serviceWorker && navigator.serviceWorker.controller === null"));
  EXPECT_TRUE(EvalBool("window.visualViewport && typeof window.visualViewport.width === 'number'"));
  EXPECT_TRUE(EvalBool("typeof Feedback === 'object' && Feedback && typeof Feedback.Bootstrap === 'object'"));
  EXPECT_TRUE(EvalBool("BM && typeof BM.trigger === 'function'"));
  EXPECT_TRUE(EvalBool("Log && typeof Log.Log === 'function'"));
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
  ASSERT_TRUE(EvalBool("(function(){ window._record = ''; var root = document.createElement('div'); "
                       "var child = document.createElement('span'); root.appendChild(child); "
                       "new MutationObserver(function(records) { window._record = records[0].type + ':' + "
                       "records[0].attributeName + ':' + (records[0].target === child); })"
                       ".observe(root, {attributes:true, subtree:true}); child.setAttribute('data-x', '1'); "
                       "return true; })()"));
  EXPECT_EQ(EvalString("window._record"), "attributes:data-x:true");
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
  EXPECT_TRUE(
      EvalBool("(function(){ var e = document.getElementById('first'); "
               "e.classList.add('a', 'b'); "
               "var ok = e.classList.contains('a') && e.classList.contains('b') "
               "    && e.classList.length === 3; "
               "e.classList.remove('a'); "
               "ok = ok && !e.classList.contains('a') && e.classList.length === 2; "
               "ok = ok && e.classList.toggle('c') === true; "
               "ok = ok && e.classList.toggle('c') === false; "
               "ok = ok && e.classList.replace('b', 'bb'); "
               "return ok && e.classList.contains('bb') && e.className === 'para bb'; })()"));
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
  EXPECT_TRUE(binder.Evaluate(
      "var a = document.createElement('a'); a.href = '/path?x=1#part';").has_value());
  const auto result = binder.Evaluate(
      "a.protocol === 'https:' && a.host === 'example.com:8443' && "
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
                       "ok = ok && svg.tagName === 'SVG'; "
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

TEST_F(DomBinderTest, WindowGetComputedStyleWired)
{
  // A binder with a computed_style callback returns a style object with
  // getPropertyValue and camelCase accessors.
  javascript::PageApis apis;
  apis.computed_style = [](const dom::Element&) -> std::map<std::string, std::string> {
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

TEST_F(DomBinderTest, FetchResolvesResponseAndBody)
{
  PageApis apis;
  apis.resolve_url = [](const std::string& raw) { return raw; };
  apis.fetch = [](const std::string& url) -> base::Result<FetchResponse> {
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

TEST_F(DomBinderTest, FetchRejectsOnNetworkError)
{
  PageApis apis;
  apis.resolve_url = [](const std::string& raw) { return raw; };
  apis.fetch = [](const std::string&) -> base::Result<FetchResponse> {
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

// A temp profile directory (removed on destruction) for the real store.
class IdbTempProfile
{
public:
  IdbTempProfile()
  {
    path_ = std::filesystem::temp_directory_path() /
            ("neko_idb_js_" + std::to_string(::getpid()) + "_" + std::to_string(counter_++));
    std::filesystem::create_directories(path_);
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

} // namespace
} // namespace neko::javascript
