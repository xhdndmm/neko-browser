// neko::javascript::DomBinder — public API entry points.
//
// Binds a DOM document into a JavaScript runtime (Phase 8 M2).  The
// implementation is split by API family across src/binding/*.cpp around the
// internal binding/binding_internal.h header; this file keeps only the
// DomBinder surface (construction, script evaluation, event dispatch entry
// points, timer pump).  See dom_binding.h for the full contract.

#include "neko/javascript/dom_binding.h"

#include "neko/javascript/script_engine_internal.h"

#include "binding/binding_internal.h"

#include <chrono>
#include <memory>
#include <optional>
#include <quickjs.h>
#include <string>
#include <string_view>
#include <utility>

namespace neko::javascript {

// ---------------------------------------------------------------------------
// DomBinder (public API).
// ---------------------------------------------------------------------------

DomBinder::DomBinder(dom::Document& document) : impl_(std::make_unique<Impl>(document, PageApis{}))
{}

DomBinder::DomBinder(dom::Document& document, const PageApis& apis)
    : impl_(std::make_unique<Impl>(document, apis))
{}

DomBinder::~DomBinder() = default;

base::Result<ScriptValue> DomBinder::Evaluate(std::string_view source, std::string_view filename)
{
  const base::Result<ScriptValue> result = impl_->engine.Evaluate(source, filename);
  impl_->DeliverMutationObservers();
  return result;
}

base::Result<ScriptValue> DomBinder::EvaluateModule(std::string_view source, std::string_view url)
{
  const base::Result<ScriptValue> result = impl_->engine.EvaluateModule(source, url);
  impl_->DeliverMutationObservers();
  return result;
}

void DomBinder::SetCurrentScript(dom::Element* element)
{
  impl_->SetCurrentScript(element);
}

void DomBinder::SetConsoleSink(ScriptEngine::ConsoleSink sink)
{
  impl_->engine.SetConsoleSink(std::move(sink));
}

void DomBinder::SetModuleSpecifierResolver(ScriptEngine::SpecifierResolver resolver)
{
  impl_->engine.SetModuleSpecifierResolver(std::move(resolver));
}

int DomBinder::RunPendingTimers()
{
  return impl_->RunPendingTimers();
}

std::optional<std::chrono::steady_clock::time_point> DomBinder::NextTimerDeadline() const
{
  return impl_->NextTimerDeadline();
}

void DomBinder::DispatchEvent(dom::Element& element, std::string_view type)
{
  impl_->DispatchEvent(element, type);
}

bool DomBinder::DispatchCancelableEvent(dom::Element& element, std::string_view type)
{
  return impl_->DispatchCancelableToNode(&element, type);
}

bool DomBinder::DispatchKeyboardEvent(dom::Element& element,
                                      std::string_view type,
                                      std::string_view key,
                                      std::string_view code)
{
  return impl_->DispatchKeyboardToNode(&element, type, key, code);
}

bool DomBinder::DispatchMouseEvent(
    dom::Element& element, std::string_view type, double client_x, double client_y, int button)
{
  return impl_->DispatchMouseToNode(&element, type, client_x, client_y, button);
}

bool DomBinder::DispatchWheelEvent(dom::Element& element, std::string_view type, double delta_y)
{
  return impl_->DispatchWheelToNode(&element, type, delta_y);
}

void DomBinder::DispatchFocusEvent(dom::Element& element, std::string_view type)
{
  impl_->DispatchFocusToNode(&element, type);
}

void DomBinder::DispatchInputEvent(dom::Element& element)
{
  impl_->DispatchInputToNode(&element);
}

bool DomBinder::TakeDomDirty()
{
  return impl_->TakeDomDirty();
}

void DomBinder::DispatchDocumentEvent(std::string_view type)
{
  impl_->DispatchDocumentEvent(type);
}

ScriptEngine& DomBinder::engine()
{
  return impl_->engine;
}

dom::Document& DomBinder::document() const
{
  return impl_->document;
}

} // namespace neko::javascript
