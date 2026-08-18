// Systematized event dispatch tests.
//
// Covers the full DOM §2.9 "dispatching events" lifecycle over a nested tree:
//
//   target -> (capture phase, root -> target's parent)
//          -> (at-target phase, capture then bubble listeners on the target)
//          -> (bubble phase, target's parent -> root)
//          -> (default action, unless preventDefault canceled the event)
//
// plus listener bookkeeping (registration order, once, stopPropagation,
// stopImmediatePropagation), event properties (eventPhase/currentTarget/
// bubbles/cancelable/defaultPrevented), and the cancelable dispatch result the
// browser layer uses to decide whether to run the default action.

#include "neko/base/status.h"
#include "neko/dom/element.h"
#include "neko/dom/query.h"
#include "neko/html/parser.h"
#include "neko/javascript/dom_binding.h"

#include <gtest/gtest.h>
#include <memory>
#include <string>

namespace neko::javascript {
namespace {

class EventDispatchTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    // Nested tree so capture/bubble have multiple ancestor hops:
    // document -> body -> #main(div) -> #mid(p) -> #leaf(span).
    document_ = html::Parser("<!doctype html><html><head><title>T</title></head>"
                             "<body><div id=\"main\"><p id=\"mid\">"
                             "<span id=\"leaf\">x</span></p></div></body></html>")
                    .Parse();
    binder_ = std::make_unique<DomBinder>(*document_);
  }

  std::string EvalString(const std::string& body)
  {
    auto r = binder_->Evaluate("(function(){" + body + "})()");
    if (!r.has_value()) {
      return "<error: " + r.error().message() + ">";
    }
    auto s = r.value().ToString();
    return s.has_value() ? s.value() : "<tostring-error>";
  }

  double EvalNumber(const std::string& body)
  {
    auto r = binder_->Evaluate("(function(){" + body + "})()");
    if (!r.has_value()) {
      return -1e9;
    }
    auto n = r.value().ToNumber();
    return n.has_value() ? n.value() : -1e9;
  }

  bool EvalBool(const std::string& body)
  {
    auto r = binder_->Evaluate("(function(){" + body + " return true; })()");
    if (!r.has_value()) {
      return false;
    }
    auto b = r.value().ToBoolean();
    return b.has_value() && b.value();
  }

  dom::Element* ById(std::string_view id)
  {
    return dom::QuerySelector(*document_, std::string("#") + std::string(id));
  }

  std::unique_ptr<dom::Document> document_;
  std::unique_ptr<DomBinder> binder_;
};

// Full dispatch order over the tree: capture on ancestors root->parent, then
// target (capture then bubble listeners), then bubble ancestors parent->root.
// eventPhase tracks 1 (capturing) / 2 (at-target) / 3 (bubbling).
TEST_F(EventDispatchTest, CaptureThenTargetThenBubble)
{
  ASSERT_TRUE(EvalBool(R"(
    window._order = [];
    var d = document, body = document.body,
        main = document.getElementById('main'),
        mid = document.getElementById('mid'),
        leaf = document.getElementById('leaf');
    d.addEventListener('go', function(ev){ window._order.push('doc-c:'+ev.eventPhase); }, true);
    body.addEventListener('go', function(ev){ window._order.push('body-c:'+ev.eventPhase); }, true);
    main.addEventListener('go', function(ev){ window._order.push('main-c:'+ev.eventPhase); }, true);
    mid.addEventListener('go', function(ev){ window._order.push('mid-c:'+ev.eventPhase); }, true);
    leaf.addEventListener('go', function(ev){ window._order.push('leaf-c:'+ev.eventPhase); }, true);
    leaf.addEventListener('go', function(ev){ window._order.push('leaf-t:'+ev.eventPhase); });
    mid.addEventListener('go', function(ev){ window._order.push('mid-b:'+ev.eventPhase); });
    main.addEventListener('go', function(ev){ window._order.push('main-b:'+ev.eventPhase); });
    body.addEventListener('go', function(ev){ window._order.push('body-b:'+ev.eventPhase); });
    d.addEventListener('go', function(ev){ window._order.push('doc-b:'+ev.eventPhase); });
    leaf.dispatchEvent(new Event('go', {bubbles: true}));
  )"));
  EXPECT_EQ(EvalString(R"(
    return window._order.join(',');
  )"),
            "doc-c:1,body-c:1,main-c:1,mid-c:1,"
            "leaf-c:2,leaf-t:2,"
            "mid-b:3,main-b:3,body-b:3,doc-b:3");
}

// currentTarget names the node whose listener is running in each phase.
TEST_F(EventDispatchTest, CurrentTargetTracksEachPhase)
{
  ASSERT_TRUE(EvalBool(R"(
    window._order = [];
    var d = document, body = document.body, main = document.getElementById('main'),
        leaf = document.getElementById('leaf');
    d.addEventListener('go', function(ev){ window._order.push(ev.currentTarget.nodeName); }, true);
    main.addEventListener('go', function(ev){ window._order.push(ev.currentTarget.id || ev.currentTarget.nodeName); });
    leaf.addEventListener('go', function(ev){ window._order.push(ev.currentTarget.id); });
    leaf.dispatchEvent(new Event('go', {bubbles: true}));
  )"));
  EXPECT_EQ(EvalString("return window._order.join(',');"), "#document,leaf,main");
}

// Multiple listeners on one node run in registration order.
TEST_F(EventDispatchTest, ListenersRunInRegistrationOrder)
{
  ASSERT_TRUE(EvalBool(R"(
    window._order = [];
    var leaf = document.getElementById('leaf');
    leaf.addEventListener('go', function(){ window._order.push('first'); });
    leaf.addEventListener('go', function(){ window._order.push('second'); });
    leaf.addEventListener('go', function(){ window._order.push('third'); });
    leaf.dispatchEvent(new Event('go', {bubbles: true}));
  )"));
  EXPECT_EQ(EvalString("return window._order.join(',');"), "first,second,third");
}

// stopPropagation() during the capture phase stops the event before it reaches
// the target (and thus also skips bubbling).
TEST_F(EventDispatchTest, StopPropagationInCapture)
{
  ASSERT_TRUE(EvalBool(R"(
    window._order = [];
    var d = document, main = document.getElementById('main'),
        mid = document.getElementById('mid'), leaf = document.getElementById('leaf');
    d.addEventListener('go', function(){ window._order.push('doc-c'); }, true);
    main.addEventListener('go', function(ev){ window._order.push('main-c'); ev.stopPropagation(); }, true);
    mid.addEventListener('go', function(){ window._order.push('mid-c'); }, true);
    leaf.addEventListener('go', function(){ window._order.push('leaf'); });
    main.addEventListener('go', function(){ window._order.push('main-b'); });
    leaf.dispatchEvent(new Event('go', {bubbles: true}));
  )"));
  EXPECT_EQ(EvalString("return window._order.join(',');"), "doc-c,main-c");
}

// stopPropagation() on the target stops bubbling to ancestors.
TEST_F(EventDispatchTest, StopPropagationAtTargetStopsBubble)
{
  ASSERT_TRUE(EvalBool(R"(
    window._order = [];
    var main = document.getElementById('main'), leaf = document.getElementById('leaf');
    leaf.addEventListener('go', function(ev){ window._order.push('leaf'); ev.stopPropagation(); });
    main.addEventListener('go', function(){ window._order.push('main'); });
    leaf.dispatchEvent(new Event('go', {bubbles: true}));
  )"));
  EXPECT_EQ(EvalString("return window._order.join(',');"), "leaf");
}

// stopImmediatePropagation() skips later listeners on the same node and stops
// propagation entirely.
TEST_F(EventDispatchTest, StopImmediatePropagationSkipsLaterListeners)
{
  ASSERT_TRUE(EvalBool(R"(
    window._order = [];
    var main = document.getElementById('main'), leaf = document.getElementById('leaf');
    leaf.addEventListener('go', function(ev){ window._order.push('a'); ev.stopImmediatePropagation(); });
    leaf.addEventListener('go', function(){ window._order.push('b'); });
    main.addEventListener('go', function(){ window._order.push('main'); });
    leaf.dispatchEvent(new Event('go', {bubbles: true}));
  )"));
  EXPECT_EQ(EvalString("return window._order.join(',');"), "a");
}

// A once listener fires once and is removed for subsequent dispatches.
TEST_F(EventDispatchTest, OnceListenerFiresOnce)
{
  ASSERT_TRUE(EvalBool(R"(
    window._count = 0;
    var leaf = document.getElementById('leaf');
    leaf.addEventListener('go', function(){ window._count++; }, {once: true});
    leaf.dispatchEvent(new Event('go'));
    leaf.dispatchEvent(new Event('go'));
  )"));
  EXPECT_EQ(EvalNumber("return window._count;"), 1.0);
}

// A non-bubbling event reaches the target only (capture listeners on ancestors
// still fire first).
TEST_F(EventDispatchTest, NonBubblingEventStaysOnTarget)
{
  ASSERT_TRUE(EvalBool(R"(
    window._order = [];
    var d = document, main = document.getElementById('main'), leaf = document.getElementById('leaf');
    d.addEventListener('go', function(){ window._order.push('doc'); }, true);
    main.addEventListener('go', function(){ window._order.push('main'); });
    leaf.addEventListener('go', function(){ window._order.push('leaf'); });
    leaf.dispatchEvent(new Event('go', {bubbles: false}));
  )"));
  EXPECT_EQ(EvalString("return window._order.join(',');"), "doc,leaf");
}

// preventDefault() only marks cancelable events; defaultPrevented reflects it.
TEST_F(EventDispatchTest, PreventDefaultOnlyOnCancelable)
{
  ASSERT_TRUE(EvalBool(R"(
    var leaf = document.getElementById('leaf');
    window._canc = false; window._ncanc = false;
    leaf.addEventListener('c', function(ev){ ev.preventDefault(); window._canc = ev.defaultPrevented; });
    leaf.addEventListener('n', function(ev){ ev.preventDefault(); window._ncanc = ev.defaultPrevented; });
    leaf.dispatchEvent(new Event('c', {cancelable: true}));
    leaf.dispatchEvent(new Event('n', {cancelable: false}));
  )"));
  EXPECT_TRUE(EvalBool("return window._canc === true;"));
  EXPECT_TRUE(EvalBool("return window._ncanc === false;"));
}

// dispatchEvent returns false when a cancelable listener canceled the event
// (the caller then knows not to run the default action).
TEST_F(EventDispatchTest, DispatchEventReturnReflectsCancelation)
{
  ASSERT_TRUE(EvalBool(R"(
    var leaf = document.getElementById('leaf');
    leaf.addEventListener('go', function(ev){ ev.preventDefault(); });
  )"));
  dom::Element* leaf = ById("leaf");
  ASSERT_NE(leaf, nullptr);
  EXPECT_FALSE(binder_->DispatchCancelableEvent(*leaf, "go"));
}

// Without preventDefault, the cancelable dispatch reports "not canceled" so the
// default action (link navigation / form submission) runs.
TEST_F(EventDispatchTest, DispatchCancelableEventNotCanceledByDefault)
{
  dom::Element* leaf = ById("leaf");
  ASSERT_NE(leaf, nullptr);
  EXPECT_TRUE(binder_->DispatchCancelableEvent(*leaf, "go"));
}

// Re-dispatching the same synthetic Event resets its dispatch state.
TEST_F(EventDispatchTest, RepeatedDispatchResetsState)
{
  ASSERT_TRUE(EvalBool(R"(
    window._count = 0;
    var leaf = document.getElementById('leaf');
    leaf.addEventListener('go', function(){ window._count++; });
    var ev = new Event('go', {bubbles: true});
    leaf.dispatchEvent(ev);
    leaf.dispatchEvent(ev);
  )"));
  EXPECT_EQ(EvalNumber("return window._count;"), 2.0);
}

// A cancelable keyboard dispatch carries the UI Events key/code strings.
TEST_F(EventDispatchTest, KeyboardEventCarriesKeyAndCode)
{
  ASSERT_TRUE(EvalBool(R"(
    window._recorded = '';
    var body = document.body;
    body.addEventListener('keydown', function(ev){
      window._recorded = ev.type + ':' + ev.key + ':' + ev.code +
                         ':' + ev.cancelable + ':' + ev.bubbles;
    });
  )"));
  dom::Element* body = dom::QuerySelector(*document_, "body");
  ASSERT_NE(body, nullptr);
  EXPECT_TRUE(binder_->DispatchKeyboardEvent(*body, "keydown", "Enter", "Enter"));
  EXPECT_EQ(EvalString("return window._recorded;"), "keydown:Enter:Enter:true:true");
  EXPECT_TRUE(binder_->DispatchKeyboardEvent(*body, "keydown", "a", "KeyA"));
  EXPECT_EQ(EvalString("return window._recorded;"), "keydown:a:KeyA:true:true");
}

// preventDefault on a keydown cancels it (the caller skips the default action).
TEST_F(EventDispatchTest, KeyboardEventPreventDefaultCancels)
{
  ASSERT_TRUE(EvalBool(R"(
    var body = document.body;
    body.addEventListener('keydown', function(ev){ ev.preventDefault(); });
  )"));
  dom::Element* body = dom::QuerySelector(*document_, "body");
  ASSERT_NE(body, nullptr);
  EXPECT_FALSE(binder_->DispatchKeyboardEvent(*body, "keydown", "Enter", "Enter"));
}

// --- Phase 2: user interaction events --------------------------------------

// Element-level global event handler attributes: a JS-assigned IDL handler
// (element.onclick = fn) fires when the event reaches the element.
TEST_F(EventDispatchTest, ElementOnClickIdlHandlerFires)
{
  ASSERT_TRUE(EvalBool(R"(
    var b = document.getElementById('main');
    window._n = 0;
    b.onclick = function(ev){ window._n++; };
    return true;
  )"));
  dom::Element* main = ById("main");
  ASSERT_NE(main, nullptr);
  EXPECT_TRUE(binder_->DispatchMouseEvent(*main, "click", 10, 20, 0));
  EXPECT_EQ(EvalNumber("return window._n;"), 1);
  // A content attribute (onclick="...") compiles and runs too.
  ASSERT_TRUE(EvalBool(R"(
    document.getElementById('main').setAttribute('onclick', 'window._n = 7;');
    return true;
  )"));
  EXPECT_TRUE(binder_->DispatchMouseEvent(*main, "click", 10, 20, 0));
  EXPECT_EQ(EvalNumber("return window._n;"), 7);
}

// MouseEvent carries client coordinates and the mouse button.
TEST_F(EventDispatchTest, MouseEventCoordinatesAndButton)
{
  ASSERT_TRUE(EvalBool(R"(
    var b = document.getElementById('main');
    window._ev = null;
    b.addEventListener('click', function(ev){ window._ev = ev; });
    return true;
  )"));
  dom::Element* main = ById("main");
  ASSERT_NE(main, nullptr);
  EXPECT_TRUE(binder_->DispatchMouseEvent(*main, "click", 12.5, 34, 2));
  EXPECT_EQ(EvalNumber("return window._ev.clientX;"), 12.5);
  EXPECT_EQ(EvalNumber("return window._ev.clientY;"), 34);
  EXPECT_EQ(EvalNumber("return window._ev.button;"), 2);
}

// The dispatch result reflects preventDefault on the mouse events too.
TEST_F(EventDispatchTest, MouseEventPreventDefaultCancels)
{
  ASSERT_TRUE(EvalBool(R"(
    document.getElementById('main').addEventListener('mousedown',
      function(ev){ ev.preventDefault(); });
    return true;
  )"));
  dom::Element* main = ById("main");
  ASSERT_NE(main, nullptr);
  EXPECT_FALSE(binder_->DispatchMouseEvent(*main, "mousedown", 5, 5, 0));
}

// KeyboardEvent exposes the legacy keyCode alongside key/code.
TEST_F(EventDispatchTest, KeyboardEventKeyCode)
{
  ASSERT_TRUE(EvalBool(R"(
    window._k = 0;
    document.body.addEventListener('keydown', function(ev){ window._k = ev.keyCode; });
    return true;
  )"));
  dom::Element* body = dom::QuerySelector(*document_, "body");
  ASSERT_NE(body, nullptr);
  EXPECT_TRUE(binder_->DispatchKeyboardEvent(*body, "keydown", "Enter", "Enter"));
  EXPECT_EQ(EvalNumber("return window._k;"), 13);
  EXPECT_TRUE(binder_->DispatchKeyboardEvent(*body, "keydown", "a", "KeyA"));
  EXPECT_EQ(EvalNumber("return window._k;"), 97);
  EXPECT_TRUE(binder_->DispatchKeyboardEvent(*body, "keydown", "ArrowDown", "ArrowDown"));
  EXPECT_EQ(EvalNumber("return window._k;"), 40);
}

// focus/blur are non-bubbling events firing on the focused element.
TEST_F(EventDispatchTest, FocusBlurEvents)
{
  ASSERT_TRUE(EvalBool(R"(
    var b = document.getElementById('main');
    window._seq = [];
    b.onfocus = function(){ window._seq.push('focus'); };
    b.onblur = function(){ window._seq.push('blur'); };
    return true;
  )"));
  dom::Element* main = ById("main");
  ASSERT_NE(main, nullptr);
  binder_->DispatchFocusEvent(*main, "focus");
  binder_->DispatchFocusEvent(*main, "blur");
  EXPECT_EQ(EvalString("return window._seq.join(',');"), "focus,blur");
}

// The bubbling "input" event fires the element's oninput handler.
TEST_F(EventDispatchTest, InputEventFires)
{
  ASSERT_TRUE(EvalBool(R"(
    var b = document.getElementById('main');
    window._n = 0;
    b.oninput = function(){ window._n++; };
    b.addEventListener('input', function(){ window._n += 10; });
    return true;
  )"));
  dom::Element* main = ById("main");
  ASSERT_NE(main, nullptr);
  binder_->DispatchInputEvent(*main);
  // oninput (1) + bubbling listener (10), both fired.
  EXPECT_EQ(EvalNumber("return window._n;"), 11);
}

TEST_F(EventDispatchTest, InputHandlerGetterDoesNotReenterAccessor)
{
  ASSERT_TRUE(EvalBool(R"(
    var b = document.getElementById('main');
    var handler = function(){ window._n = 1; };
    b.oninput = handler;
    return b.oninput === handler;
  )"));
  dom::Element* main = ById("main");
  ASSERT_NE(main, nullptr);
  binder_->DispatchInputEvent(*main);
  EXPECT_EQ(EvalNumber("return window._n;"), 1);
}

// The cancelable wheel event carries the vertical delta.
TEST_F(EventDispatchTest, WheelEventDelta)
{
  ASSERT_TRUE(EvalBool(R"(
    window._d = 0;
    document.body.onwheel = function(ev){ window._d = ev.deltaY; };
    return true;
  )"));
  dom::Element* body = dom::QuerySelector(*document_, "body");
  ASSERT_NE(body, nullptr);
  EXPECT_TRUE(binder_->DispatchWheelEvent(*body, "wheel", 40));
  EXPECT_EQ(EvalNumber("return window._d;"), 40);
  // preventDefault on wheel cancels the event.
  ASSERT_TRUE(EvalBool(R"(
    document.body.addEventListener('wheel', function(ev){ ev.preventDefault(); });
    return true;
  )"));
  EXPECT_FALSE(binder_->DispatchWheelEvent(*body, "wheel", 40));
}

} // namespace
} // namespace neko::javascript
