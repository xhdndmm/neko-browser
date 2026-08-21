#pragma once

#include "neko/base/status.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace neko::javascript {

// Owns the JSRuntime + JSContext (defined in script_engine.cpp).  Kept via
// shared_ptr so a ScriptValue keeps the runtime alive after the engine dies.
struct RuntimeCore;

// The kind of a JavaScript value.
enum class ValueKind
{
  kInvalid, // default-constructed or moved-from handle
  kUndefined,
  kNull,
  kBoolean,
  kNumber,
  kString,
  kObject,
  kFunction,
  kSymbol,
  kBigInt,
};

std::string_view ToString(ValueKind kind);

// Owning handle to a JavaScript value produced by a ScriptEngine.
//
// Values are reference-counted and tied to the engine's runtime: each value
// keeps the underlying runtime alive, so a value may safely outlive the
// ScriptEngine that created it.  Values from different engines must never be
// mixed.  Threading: use a value from the same thread as its engine.
class ScriptValue
{
public:
  ScriptValue();
  ScriptValue(const ScriptValue& other); // duplicates the reference
  ScriptValue& operator=(const ScriptValue& other);
  ScriptValue(ScriptValue&& other) noexcept;
  ScriptValue& operator=(ScriptValue&& other) noexcept;
  ~ScriptValue();

  bool IsValid() const;
  ValueKind Kind() const;

  // JS ToString / ToNumber / ToBoolean semantics.  Stringification of
  // objects follows JS ("[object Object]"); use JsonStringify() for a
  // structured representation.  Errors may carry a pending JS exception.
  base::Result<std::string> ToString() const;
  base::Result<double> ToNumber() const;
  base::Result<bool> ToBoolean() const;
  // JSON.stringify(value); fails on non-serializable values (e.g. cycles).
  base::Result<std::string> JsonStringify() const;

private:
  friend class ScriptEngine;

  ScriptValue(std::shared_ptr<RuntimeCore> core, void* js_value);
  void Reset();

  std::shared_ptr<RuntimeCore> core_;
  void* js_value_ = nullptr; // heap-allocated JSValue (owned)
};

// A sandboxed JavaScript execution context: one runtime, one global scope.
//
// Only the core language and a project-provided `console` binding are
// available; the QuickJS std/os modules (file, process, network access) are
// deliberately not compiled in.
//
// Threading: thread-confined.  Use a ScriptEngine from one thread at a time;
// the interrupt handler runs on the calling thread, so no locking is needed.
class ScriptEngine
{
public:
  ScriptEngine();
  ~ScriptEngine();

  ScriptEngine(const ScriptEngine&) = delete;
  ScriptEngine& operator=(const ScriptEngine&) = delete;

  using ConsoleSink = std::function<void(std::string_view level, std::string_view text)>;

  // Redirects console.log/info/warn/error output.  Default: dropped.
  void SetConsoleSink(ConsoleSink sink);

  // Synchronously loads the source text of the ES module at |url| (already
  // normalized to an absolute URL by the engine).  Wired by the embedder to
  // its network stack; when unset, importing a module throws a
  // ReferenceError ("module loading is not enabled").
  using ModuleFetcher = std::function<base::Result<std::string>(const std::string& url)>;

  // Enables ES module loading with |fetcher| as the remote-source provider.
  // The fetcher must be thread-compatible with the engine (called on the
  // engine's thread) and may be invoked re-entrantly while a module
  // instantiates (one call per distinct imported URL).
  void SetModuleFetcher(ModuleFetcher fetcher);

  // Scripts running longer than |limit| are aborted with an "interrupted"
  // error (checked from the QuickJS interrupt handler).  Default: 10 s.
  void SetExecutionLimit(std::chrono::milliseconds limit);

  // Upper bound for the runtime heap (JS_SetMemoryLimit); 0 disables the
  // limit.  Default: 128 MiB.
  void SetMemoryLimit(std::size_t bytes);

  // Evaluates a script in the global scope; returns the completion value.
  // Syntax errors surface as Error::Parse, runtime / limit errors as
  // Error::Javascript.  After evaluation, the runtime's job queue (promise
  // reactions / microtasks) is drained, so async functions started by the
  // script progress to completion (or until the execution limit).
  base::Result<ScriptValue> Evaluate(std::string_view source, std::string_view filename = "eval");

  // Evaluates |source| as an ES module (import/export allowed, strict mode,
  // its own top-level scope) named |url|.  |url| should be the absolute URL
  // of the module: it anchors relative import specifiers and becomes
  // import.meta.url.  Static imports instantiate synchronously through the
  // module loader (SetModuleFetcher); after evaluation the job queue is
  // drained like Evaluate().
  base::Result<ScriptValue> EvaluateModule(std::string_view source, std::string_view url);

  base::Result<ScriptValue> CallGlobal(const std::string& name,
                                       const std::vector<ScriptValue>& args = {});
  base::Result<ScriptValue> GetGlobal(const std::string& name);
  base::Result<void> SetGlobal(const std::string& name, const ScriptValue& value);

  // Runs every pending job (promise .then continuations, microtasks) to
  // completion, honouring the execution limit via the interrupt handler.  A
  // job that throws (e.g. an unhandled rejection) is reported through the
  // console sink and does not stop the remaining jobs.  QuickJS does not run
  // jobs automatically; Evaluate/CallGlobal call this, and callers that pump
  // timers or dispatch events should call it too so promise continuations
  // created by callbacks make progress.
  void RunPendingJobs();

  // Value constructors, tied to this engine.
  base::Result<ScriptValue> MakeUndefined();
  base::Result<ScriptValue> MakeNull();
  base::Result<ScriptValue> MakeBoolean(bool b);
  base::Result<ScriptValue> MakeNumber(double d);
  base::Result<ScriptValue> MakeString(std::string_view s);

  // Human-readable engine name and version.
  static std::string RuntimeName();
  static std::string Version();

private:
  // Internal access for the DOM binder (same module); see
  // script_engine_internal.h.
  friend void* ScriptEngineContext(ScriptEngine&);

  std::shared_ptr<RuntimeCore> core_;
  std::chrono::milliseconds execution_limit_ = std::chrono::seconds(10);
};

} // namespace neko::javascript
