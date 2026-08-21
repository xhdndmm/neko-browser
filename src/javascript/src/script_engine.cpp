// neko::javascript — a sandboxed JavaScript runtime, wrapping QuickJS
// (quickjs-ng) behind a project-owned interface.  Only the core language and
// a project-provided `console` binding are exposed; the QuickJS std/os
// modules (file, process, network) are not compiled in.
//
// Threading: a ScriptEngine is thread-confined — use it from one thread at a
// time.  The QuickJS interrupt handler runs on the calling thread, so no
// locking is required.

#include "neko/javascript/script_engine.h"

#include "neko/javascript/script_engine_internal.h"
#include "neko/url/url.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <quickjs.h>
#include <string>
#include <utility>
#include <vector>

namespace neko::javascript {

// QuickJS calls the interrupt handler periodically during execution; return
// non-zero to abort the running script.
int InterruptHandler(JSRuntime* rt, void* opaque);

// Called by QuickJS when a promise rejection becomes unhandled (is_handled
// false) or is later handled (is_handled true).  quickjs fires the false
// case synchronously at rejection time even when a handler is attached in the
// same turn, so instead of reporting immediately we record the rejection and
// report it at the microtask checkpoint (end of RunPendingJobs) only if it is
// still unhandled then — matching browsers' unhandledrejection timing.
void PromiseRejectionTracker(
    JSContext* ctx, JSValueConst promise, JSValueConst reason, bool is_handled, void* opaque);

// ES module support (see SetModuleFetcher / EvaluateModule): QuickJS calls
// ModuleNormalize to resolve an import specifier against the importing
// module's name, then ModuleLoader to obtain the JSModuleDef for the
// normalized URL.
char* ModuleNormalize(JSContext* ctx, const char* base_name, const char* name, void* opaque);
JSModuleDef* ModuleLoader(JSContext* ctx, const char* module_name, void* opaque);

// Owns the JSRuntime + JSContext.  One per ScriptEngine; shared with the
// ScriptValues it produces so a value keeps the runtime alive.
struct RuntimeCore
{
  JSRuntime* rt = nullptr;
  JSContext* ctx = nullptr;
  ScriptEngine::ConsoleSink console_sink;
  // Remote module source provider (ES modules); unset until SetModuleFetcher.
  ScriptEngine::ModuleFetcher module_fetcher;
  // Optional import-map resolver consulted before the built-in specifier
  // rules (see SetModuleSpecifierResolver).
  ScriptEngine::SpecifierResolver module_specifier_resolver;
  std::chrono::steady_clock::time_point deadline{};
  bool interrupted = false;
  // Promises whose rejections are currently unreported: (promise, reason),
  // both Dup'd.  Cleared at each microtask checkpoint (see RunPendingJobs).
  std::vector<std::pair<JSValue, JSValue>> pending_rejections;

  RuntimeCore()
  {
    rt = JS_NewRuntime();
    if (rt == nullptr)
      return;
    ctx = JS_NewContext(rt);
    if (ctx == nullptr) {
      JS_FreeRuntime(rt);
      rt = nullptr;
      return;
    }
    JS_SetContextOpaque(ctx, this);
    JS_SetInterruptHandler(rt, &InterruptHandler, this);
    JS_SetHostPromiseRejectionTracker(rt, &PromiseRejectionTracker, this);
    // The loader is registered unconditionally; without a fetcher it throws
    // "module loading is not enabled", so importing fails loudly instead of
    // silently falling back to file access (which must never happen here).
    JS_SetModuleLoaderFunc(rt, &ModuleNormalize, &ModuleLoader, this);
  }

  ~RuntimeCore()
  {
    if (ctx != nullptr) {
      for (auto& p : pending_rejections) {
        JS_FreeValue(ctx, p.first);
        JS_FreeValue(ctx, p.second);
      }
      pending_rejections.clear();
      JS_FreeContext(ctx);
    }
    if (rt != nullptr)
      JS_FreeRuntime(rt);
  }
};

int InterruptHandler(JSRuntime* /*rt*/, void* opaque)
{
  auto* core = static_cast<RuntimeCore*>(opaque);
  if (std::chrono::steady_clock::now() >= core->deadline) {
    core->interrupted = true;
    return 1;
  }
  return 0;
}

void PromiseRejectionTracker(
    JSContext* ctx, JSValueConst promise, JSValueConst reason, bool is_handled, void* opaque)
{
  auto* core = static_cast<RuntimeCore*>(opaque);
  if (core == nullptr)
    return;
  if (is_handled) {
    // A handler was attached: drop any pending report for this promise.
    for (auto it = core->pending_rejections.begin(); it != core->pending_rejections.end(); ++it) {
      if (JS_IsStrictEqual(ctx, it->first, promise)) {
        JS_FreeValue(ctx, it->first);
        JS_FreeValue(ctx, it->second);
        core->pending_rejections.erase(it);
        break;
      }
    }
    return;
  }
  core->pending_rejections.emplace_back(JS_DupValue(ctx, promise), JS_DupValue(ctx, reason));
}

// ---------------------------------------------------------------------------
// ES module loading.
// ---------------------------------------------------------------------------

namespace {

// Copies |name| into a js_malloc'd C string (the ownership contract of
// JSModuleNormalizeFunc).
char* AllocModuleName(JSContext* ctx, std::string_view name)
{
  char* out = static_cast<char*>(js_malloc(ctx, name.size() + 1));
  if (out == nullptr) {
    JS_ThrowOutOfMemory(ctx);
    return nullptr;
  }
  std::memcpy(out, name.data(), name.size());
  out[name.size()] = '\0';
  return out;
}

// Sets import.meta.url = |url| on the module carried by |func_val| (a
// JS_EVAL_FLAG_COMPILE_ONLY result).  QuickJS creates the meta object empty;
// without this, import.meta.url would be undefined for every module.
void SetModuleMetaUrl(JSContext* ctx, JSValueConst func_val, const char* url)
{
  if (JS_VALUE_GET_TAG(func_val) != JS_TAG_MODULE) {
    return;
  }
  auto* m = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(func_val));
  JSValue meta = JS_GetImportMeta(ctx, m);
  if (JS_IsException(meta)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return;
  }
  JS_SetPropertyStr(ctx, meta, "url", JS_NewString(ctx, url)); // steals the string
  JS_FreeValue(ctx, meta);
}

} // namespace

char* ModuleNormalize(JSContext* ctx, const char* base_name, const char* name, void* opaque)
{
  auto* core = static_cast<RuntimeCore*>(opaque);
  const std::string_view specifier(name);
  // Import-map resolution (when a resolver is installed) runs first: an
  // exact/prefix mapping wins over the built-in rules.
  if (core != nullptr && core->module_specifier_resolver) {
    std::optional<std::string> mapped = core->module_specifier_resolver(base_name, name);
    if (mapped.has_value()) {
      return AllocModuleName(ctx, mapped.value());
    }
  }
  // Absolute URL (has a scheme): use as-is.  Module names in this engine are
  // always URLs, so "://" unambiguously marks an absolute specifier.
  if (specifier.find("://") != std::string_view::npos) {
    return AllocModuleName(ctx, specifier);
  }
  // WHATWG modules: only "./", "../" and "/"-rooted specifiers resolve
  // against the importing module; bare specifiers ("react") require an
  // import map (not implemented — documented limitation).
  const bool relative =
      specifier.starts_with("./") || specifier.starts_with("../") || specifier.starts_with("/");
  if (!relative) {
    JS_ThrowReferenceError(
        ctx, "cannot resolve module specifier \"%s\" without an import map", name);
    return nullptr;
  }
  const base::Result<url::Url> base = url::Url::Parse(base_name);
  if (!base.has_value()) {
    JS_ThrowReferenceError(
        ctx, "cannot resolve module specifier \"%s\": base \"%s\" is not a URL", name, base_name);
    return nullptr;
  }
  const base::Result<url::Url> resolved = url::Url::Parse(name, base.value());
  if (!resolved.has_value()) {
    JS_ThrowReferenceError(ctx, "could not resolve module specifier \"%s\"", name);
    return nullptr;
  }
  return AllocModuleName(ctx, resolved.value().Serialize());
}

JSModuleDef* ModuleLoader(JSContext* ctx, const char* module_name, void* opaque)
{
  auto* core = static_cast<RuntimeCore*>(opaque);
  if (core == nullptr || !core->module_fetcher) {
    JS_ThrowReferenceError(
        ctx, "could not load module '%s': module loading is not enabled", module_name);
    return nullptr;
  }
  const base::Result<std::string> source = core->module_fetcher(module_name);
  if (!source.has_value()) {
    JS_ThrowReferenceError(
        ctx, "could not load module '%s': %s", module_name, source.error().message().c_str());
    return nullptr;
  }
  // Compile (not run) the fetched source as a module under its URL.  The
  // importer already references the returned def, so free our handle and
  // return the raw pointer (same pattern as quickjs-libc's js_module_load).
  JSValue func = JS_Eval(core->ctx,
                         source.value().data(),
                         source.value().size(),
                         module_name,
                         JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  if (JS_IsException(func)) {
    return nullptr;
  }
  SetModuleMetaUrl(ctx, func, module_name);
  auto* m = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(func));
  JS_FreeValue(ctx, func);
  return m;
}

namespace {

constexpr std::size_t kDefaultMemoryLimit = 128u * 1024u * 1024u; // 128 MiB

// Appends the string form of |v| to |out| (console.log-style formatting:
// strings as-is, objects as JSON, everything else via JS ToString).
void AppendConsoleArg(JSContext* ctx, std::string& out, JSValueConst v)
{
  if (JS_IsObject(v)) {
    JSValue json = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(json)) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      out += "[object]";
      return;
    }
    const char* s = JS_ToCString(ctx, json);
    out += s != nullptr ? s : "[object]";
    if (s != nullptr)
      JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, json);
    return;
  }
  const char* s = JS_ToCString(ctx, v);
  if (s == nullptr) {
    JS_FreeValue(ctx, JS_GetException(ctx)); // e.g. Symbol -> ToString throws
    out += "[value]";
    return;
  }
  out += s;
  JS_FreeCString(ctx, s);
}

// console.log/info/warn/error binding.  The magic argument selects the level.
JSValue
JsConsole(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv, int magic)
{
  auto* core = static_cast<RuntimeCore*>(JS_GetContextOpaque(ctx));
  if (core == nullptr || !core->console_sink)
    return JS_UNDEFINED;
  static constexpr const char* kLevels[] = {"log", "info", "warning", "error"};
  const char* level = (magic >= 0 && magic < 4) ? kLevels[magic] : "log";
  std::string text;
  for (int i = 0; i < argc; ++i) {
    if (i > 0)
      text += ' ';
    AppendConsoleArg(ctx, text, argv[i]);
  }
  core->console_sink(level, text);
  return JS_UNDEFINED;
}

// Converts a pending JS exception into a project Error.  Syntax errors are
// reported as Error::Parse; everything else as Error::Javascript.
base::Error MakeErrorFromException(JSContext* ctx, bool interrupted)
{
  if (interrupted) {
    return base::Error::Javascript("script execution interrupted (limit exceeded)");
  }
  JSValue exc = JS_GetException(ctx);
  std::string message;
  // Primary text: the exception's own string form ("Error: boom" /
  // "SyntaxError: ...").  This can itself fail (e.g. a Symbol was thrown),
  // in which case a pending exception is set again and must be cleared.
  const char* s = JS_ToCString(ctx, exc);
  if (s != nullptr) {
    message = s;
    JS_FreeCString(ctx, s);
  } else {
    JS_FreeValue(ctx, JS_GetException(ctx));
  }
  // Secondary: append the stack traceback if present.
  JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
  if (JS_IsString(stack)) {
    const char* ss = JS_ToCString(ctx, stack);
    if (ss != nullptr) {
      if (!message.empty())
        message += '\n';
      message += ss;
      JS_FreeCString(ctx, ss);
    }
  }
  JS_FreeValue(ctx, stack);
  JS_FreeValue(ctx, exc);
  if (message.rfind("SyntaxError", 0) == 0) {
    return base::Error::Parse(std::move(message));
  }
  return base::Error::Javascript(std::move(message));
}

} // namespace

std::string_view ToString(ValueKind kind)
{
  switch (kind) {
  case ValueKind::kInvalid:
    return "invalid";
  case ValueKind::kUndefined:
    return "undefined";
  case ValueKind::kNull:
    return "null";
  case ValueKind::kBoolean:
    return "boolean";
  case ValueKind::kNumber:
    return "number";
  case ValueKind::kString:
    return "string";
  case ValueKind::kObject:
    return "object";
  case ValueKind::kFunction:
    return "function";
  case ValueKind::kSymbol:
    return "symbol";
  case ValueKind::kBigInt:
    return "bigint";
  }
  return "invalid";
}

// ---------------------------------------------------------------------------
// RuntimeCore lives above; ScriptValue / ScriptEngine below.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// ScriptValue
// ---------------------------------------------------------------------------
ScriptValue::ScriptValue() = default;

ScriptValue::ScriptValue(std::shared_ptr<RuntimeCore> core, void* js_value)
    : core_(std::move(core)), js_value_(js_value)
{}

ScriptValue::ScriptValue(const ScriptValue& other) : core_(other.core_), js_value_(nullptr)
{
  if (other.js_value_ != nullptr && core_ != nullptr && core_->ctx != nullptr) {
    js_value_ = new JSValue(JS_DupValue(core_->ctx, *static_cast<JSValue*>(other.js_value_)));
  }
}

ScriptValue& ScriptValue::operator=(const ScriptValue& other)
{
  if (this == &other)
    return *this;
  Reset();
  core_ = other.core_;
  if (other.js_value_ != nullptr && core_ != nullptr && core_->ctx != nullptr) {
    js_value_ = new JSValue(JS_DupValue(core_->ctx, *static_cast<JSValue*>(other.js_value_)));
  }
  return *this;
}

ScriptValue::ScriptValue(ScriptValue&& other) noexcept
    : core_(std::move(other.core_)), js_value_(other.js_value_)
{
  other.js_value_ = nullptr;
}

ScriptValue& ScriptValue::operator=(ScriptValue&& other) noexcept
{
  if (this == &other)
    return *this;
  Reset();
  core_ = std::move(other.core_);
  js_value_ = other.js_value_;
  other.js_value_ = nullptr;
  return *this;
}

ScriptValue::~ScriptValue()
{
  Reset();
}

void ScriptValue::Reset()
{
  if (js_value_ != nullptr) {
    if (core_ != nullptr && core_->ctx != nullptr) {
      JS_FreeValue(core_->ctx, *static_cast<JSValue*>(js_value_));
    }
    delete static_cast<JSValue*>(js_value_);
    js_value_ = nullptr;
  }
  core_.reset();
}

bool ScriptValue::IsValid() const
{
  return js_value_ != nullptr && core_ != nullptr && core_->ctx != nullptr;
}

ValueKind ScriptValue::Kind() const
{
  if (!IsValid())
    return ValueKind::kInvalid;
  const JSValue& v = *static_cast<JSValue*>(js_value_);
  if (JS_IsUndefined(v))
    return ValueKind::kUndefined;
  if (JS_IsNull(v))
    return ValueKind::kNull;
  if (JS_IsBool(v))
    return ValueKind::kBoolean;
  if (JS_IsNumber(v))
    return ValueKind::kNumber;
  if (JS_IsString(v))
    return ValueKind::kString;
  if (JS_IsSymbol(v))
    return ValueKind::kSymbol;
  if (JS_IsBigInt(v))
    return ValueKind::kBigInt;
  if (JS_IsFunction(core_->ctx, v))
    return ValueKind::kFunction;
  if (JS_IsObject(v))
    return ValueKind::kObject;
  return ValueKind::kInvalid;
}

base::Result<std::string> ScriptValue::ToString() const
{
  if (!IsValid())
    return base::Err(base::Error::InvalidArgument("invalid script value"));
  const char* s = JS_ToCString(core_->ctx, *static_cast<JSValue*>(js_value_));
  if (s == nullptr) {
    JS_FreeValue(core_->ctx, JS_GetException(core_->ctx));
    return base::Err(base::Error::Javascript("value cannot be converted to string"));
  }
  std::string out(s);
  JS_FreeCString(core_->ctx, s);
  return out;
}

base::Result<double> ScriptValue::ToNumber() const
{
  if (!IsValid())
    return base::Err(base::Error::InvalidArgument("invalid script value"));
  double d = 0;
  if (JS_ToFloat64(core_->ctx, &d, *static_cast<JSValue*>(js_value_)) != 0) {
    JS_FreeValue(core_->ctx, JS_GetException(core_->ctx));
    return base::Err(base::Error::Javascript("value cannot be converted to number"));
  }
  return d;
}

base::Result<bool> ScriptValue::ToBoolean() const
{
  if (!IsValid())
    return base::Err(base::Error::InvalidArgument("invalid script value"));
  return JS_ToBool(core_->ctx, *static_cast<JSValue*>(js_value_)) != 0;
}

base::Result<std::string> ScriptValue::JsonStringify() const
{
  if (!IsValid())
    return base::Err(base::Error::InvalidArgument("invalid script value"));
  JSValue json =
      JS_JSONStringify(core_->ctx, *static_cast<JSValue*>(js_value_), JS_UNDEFINED, JS_UNDEFINED);
  if (JS_IsException(json)) {
    JSValue exc = JS_GetException(core_->ctx);
    std::string message;
    const char* s = JS_ToCString(core_->ctx, exc);
    if (s != nullptr) {
      message = s;
      JS_FreeCString(core_->ctx, s);
    } else {
      JS_FreeValue(core_->ctx, JS_GetException(core_->ctx));
    }
    JS_FreeValue(core_->ctx, exc);
    return base::Err(base::Error::Javascript(message.empty() ? "value cannot be serialized to JSON"
                                                             : std::move(message)));
  }
  const char* s = JS_ToCString(core_->ctx, json);
  std::string out = s != nullptr ? s : "";
  if (s != nullptr)
    JS_FreeCString(core_->ctx, s);
  JS_FreeValue(core_->ctx, json);
  return out;
}

// ---------------------------------------------------------------------------
// ScriptEngine
// ---------------------------------------------------------------------------
ScriptEngine::ScriptEngine() : core_(std::make_shared<RuntimeCore>())
{
  if (core_->ctx == nullptr)
    return;
  SetMemoryLimit(kDefaultMemoryLimit);

  // Install the project-provided `console` binding on the global object.
  JSValue global = JS_GetGlobalObject(core_->ctx);
  JSValue console = JS_NewObject(core_->ctx);
  static const JSCFunctionListEntry kConsoleFuncs[] = {
      JS_CFUNC_MAGIC_DEF("log", 0, JsConsole, 0),
      JS_CFUNC_MAGIC_DEF("info", 0, JsConsole, 1),
      JS_CFUNC_MAGIC_DEF("warn", 0, JsConsole, 2),
      JS_CFUNC_MAGIC_DEF("error", 0, JsConsole, 3),
  };
  JS_SetPropertyFunctionList(core_->ctx,
                             console,
                             kConsoleFuncs,
                             static_cast<int>(sizeof(kConsoleFuncs) / sizeof(kConsoleFuncs[0])));
  JS_SetPropertyStr(core_->ctx, global, "console", console); // steals the reference
  JS_FreeValue(core_->ctx, global);
}

ScriptEngine::~ScriptEngine() = default;

void ScriptEngine::SetConsoleSink(ConsoleSink sink)
{
  core_->console_sink = std::move(sink);
}

void ScriptEngine::SetModuleFetcher(ModuleFetcher fetcher)
{
  core_->module_fetcher = std::move(fetcher);
}

void ScriptEngine::SetModuleSpecifierResolver(SpecifierResolver resolver)
{
  core_->module_specifier_resolver = std::move(resolver);
}

void ScriptEngine::SetExecutionLimit(std::chrono::milliseconds limit)
{
  execution_limit_ = limit;
}

void ScriptEngine::SetMemoryLimit(std::size_t bytes)
{
  if (core_->rt == nullptr)
    return;
  // 0 disables the limit; JS_SetMemoryLimit takes -1 for "no limit".
  JS_SetMemoryLimit(core_->rt, bytes == 0 ? static_cast<std::size_t>(-1) : bytes);
}

base::Result<ScriptValue> ScriptEngine::Evaluate(std::string_view source, std::string_view filename)
{
  if (core_->ctx == nullptr) {
    return base::Err(base::Error::Javascript("script runtime failed to initialize"));
  }
  const std::string name(filename.empty() ? "eval" : filename);
  core_->deadline = std::chrono::steady_clock::now() + execution_limit_;
  core_->interrupted = false;
  JSValue result =
      JS_Eval(core_->ctx, source.data(), source.size(), name.c_str(), JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(result)) {
    const base::Error err = MakeErrorFromException(core_->ctx, core_->interrupted);
    // Async work scheduled by the failing script still runs (a rejected
    // promise's handlers, for example) before the error is reported.
    RunPendingJobs();
    return base::Err(std::move(err));
  }
  // Drain the job queue so async functions started by the script progress
  // (e.g. an `async function` invoked at top level with an `await` chain).
  RunPendingJobs();
  return ScriptValue(core_, new JSValue(result));
}

base::Result<ScriptValue> ScriptEngine::EvaluateModule(std::string_view source,
                                                       std::string_view url)
{
  if (core_->ctx == nullptr) {
    return base::Err(base::Error::Javascript("script runtime failed to initialize"));
  }
  const std::string name(url.empty() ? "module" : url);
  core_->deadline = std::chrono::steady_clock::now() + execution_limit_;
  core_->interrupted = false;
  // Compile the entry module first so import.meta.url can be populated, then
  // run it: instantiation resolves static imports synchronously through
  // ModuleLoader, evaluation runs the module bodies in post-order.
  JSValue func = JS_Eval(core_->ctx,
                         source.data(),
                         source.size(),
                         name.c_str(),
                         JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  if (JS_IsException(func)) {
    const base::Error err = MakeErrorFromException(core_->ctx, core_->interrupted);
    RunPendingJobs();
    return base::Err(std::move(err));
  }
  SetModuleMetaUrl(core_->ctx, func, name.c_str());
  JSValue result = JS_EvalFunction(core_->ctx, func); // consumes |func|
  if (JS_IsException(result)) {
    const base::Error err = MakeErrorFromException(core_->ctx, core_->interrupted);
    // Async work scheduled by the failing module still runs before the error
    // is reported.
    RunPendingJobs();
    return base::Err(std::move(err));
  }
  RunPendingJobs();
  return ScriptValue(core_, new JSValue(result));
}

void ScriptEngine::RunPendingJobs()
{
  if (core_->rt == nullptr)
    return;
  core_->deadline = std::chrono::steady_clock::now() + execution_limit_;
  core_->interrupted = false;
  while (JS_IsJobPending(core_->rt) && !core_->interrupted) {
    JSContext* job_ctx = core_->ctx;
    if (JS_ExecutePendingJob(core_->rt, &job_ctx) < 0) {
      // An exception escaped a job.  Report it and keep draining.
      const std::string message = MakeErrorFromException(job_ctx, false).message();
      if (core_->console_sink) {
        core_->console_sink("error", message);
      }
    }
  }
  // Microtask checkpoint: report rejections that are still unhandled.
  for (auto& p : core_->pending_rejections) {
    if (core_->console_sink) {
      std::string message = "Uncaught (in promise) ";
      const char* s = JS_ToCString(core_->ctx, p.second);
      if (s != nullptr) {
        message += s;
        JS_FreeCString(core_->ctx, s);
      } else {
        JS_FreeValue(core_->ctx, JS_GetException(core_->ctx));
      }
      core_->console_sink("error", message);
    }
    JS_FreeValue(core_->ctx, p.first);
    JS_FreeValue(core_->ctx, p.second);
  }
  core_->pending_rejections.clear();
}

base::Result<ScriptValue> ScriptEngine::CallGlobal(const std::string& name,
                                                   const std::vector<ScriptValue>& args)
{
  if (core_->ctx == nullptr) {
    return base::Err(base::Error::Javascript("script runtime failed to initialize"));
  }
  JSValue global = JS_GetGlobalObject(core_->ctx);
  JSValue fn = JS_GetPropertyStr(core_->ctx, global, name.c_str());
  JS_FreeValue(core_->ctx, global);
  if (!JS_IsFunction(core_->ctx, fn)) {
    JS_FreeValue(core_->ctx, fn);
    return base::Err(base::Error::Javascript("'" + name + "' is not a function"));
  }
  std::vector<JSValue> argv;
  argv.reserve(args.size());
  for (const auto& arg : args) {
    if (!arg.IsValid()) {
      JS_FreeValue(core_->ctx, fn);
      return base::Err(base::Error::InvalidArgument("argument is not a valid script value"));
    }
    if (arg.core_ != core_) {
      JS_FreeValue(core_->ctx, fn);
      return base::Err(base::Error::InvalidArgument("argument belongs to a different engine"));
    }
    argv.push_back(*static_cast<JSValue*>(arg.js_value_));
  }
  core_->deadline = std::chrono::steady_clock::now() + execution_limit_;
  core_->interrupted = false;
  JSValue result =
      JS_Call(core_->ctx, fn, JS_UNDEFINED, static_cast<int>(args.size()), argv.data());
  JS_FreeValue(core_->ctx, fn);
  if (JS_IsException(result)) {
    const base::Error err = MakeErrorFromException(core_->ctx, core_->interrupted);
    RunPendingJobs();
    return base::Err(std::move(err));
  }
  RunPendingJobs();
  return ScriptValue(core_, new JSValue(result));
}

base::Result<ScriptValue> ScriptEngine::GetGlobal(const std::string& name)
{
  if (core_->ctx == nullptr) {
    return base::Err(base::Error::Javascript("script runtime failed to initialize"));
  }
  JSValue global = JS_GetGlobalObject(core_->ctx);
  JSValue v = JS_GetPropertyStr(core_->ctx, global, name.c_str());
  JS_FreeValue(core_->ctx, global);
  if (JS_IsException(v)) {
    JS_FreeValue(core_->ctx, JS_GetException(core_->ctx));
    return base::Err(base::Error::Javascript("failed to read global '" + name + "'"));
  }
  return ScriptValue(core_, new JSValue(v));
}

base::Result<void> ScriptEngine::SetGlobal(const std::string& name, const ScriptValue& value)
{
  if (core_->ctx == nullptr) {
    return base::Err(base::Error::Javascript("script runtime failed to initialize"));
  }
  if (!value.IsValid() || value.core_ != core_) {
    return base::Err(base::Error::InvalidArgument("value is not valid for this engine"));
  }
  JSValue global = JS_GetGlobalObject(core_->ctx);
  JSValue dup = JS_DupValue(core_->ctx, *static_cast<JSValue*>(value.js_value_));
  const int rc = JS_SetPropertyStr(core_->ctx, global, name.c_str(), dup); // steals |dup|
  JS_FreeValue(core_->ctx, global);
  if (rc < 0) {
    JS_FreeValue(core_->ctx, JS_GetException(core_->ctx));
    return base::Err(base::Error::Javascript("failed to set global '" + name + "'"));
  }
  return base::Ok();
}

base::Result<ScriptValue> ScriptEngine::MakeUndefined()
{
  if (core_->ctx == nullptr) {
    return base::Err(base::Error::Javascript("script runtime failed to initialize"));
  }
  return ScriptValue(core_, new JSValue(JS_UNDEFINED));
}

base::Result<ScriptValue> ScriptEngine::MakeNull()
{
  if (core_->ctx == nullptr) {
    return base::Err(base::Error::Javascript("script runtime failed to initialize"));
  }
  return ScriptValue(core_, new JSValue(JS_NULL));
}

base::Result<ScriptValue> ScriptEngine::MakeBoolean(bool b)
{
  if (core_->ctx == nullptr) {
    return base::Err(base::Error::Javascript("script runtime failed to initialize"));
  }
  return ScriptValue(core_, new JSValue(b ? JS_TRUE : JS_FALSE));
}

base::Result<ScriptValue> ScriptEngine::MakeNumber(double d)
{
  if (core_->ctx == nullptr) {
    return base::Err(base::Error::Javascript("script runtime failed to initialize"));
  }
  JSValue v;
  if (d == static_cast<double>(static_cast<int64_t>(d)) && d >= -9.007199254740992e15 &&
      d <= 9.007199254740992e15) {
    v = JS_NewInt64(core_->ctx, static_cast<int64_t>(d));
  } else {
    v = JS_NewFloat64(core_->ctx, d);
  }
  return ScriptValue(core_, new JSValue(v));
}

base::Result<ScriptValue> ScriptEngine::MakeString(std::string_view s)
{
  if (core_->ctx == nullptr) {
    return base::Err(base::Error::Javascript("script runtime failed to initialize"));
  }
  return ScriptValue(core_, new JSValue(JS_NewStringLen(core_->ctx, s.data(), s.size())));
}

std::string ScriptEngine::RuntimeName()
{
  return "QuickJS";
}

std::string ScriptEngine::Version()
{
  return std::to_string(QJS_VERSION_MAJOR) + "." + std::to_string(QJS_VERSION_MINOR) + "." +
         std::to_string(QJS_VERSION_PATCH);
}

void* ScriptEngineContext(ScriptEngine& engine)
{
  return engine.core_ != nullptr ? engine.core_->ctx : nullptr;
}

} // namespace neko::javascript
