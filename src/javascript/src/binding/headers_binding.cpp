#include "binding_internal.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <mutex>
#include <quickjs.h>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace neko::javascript {
namespace {

struct HeadersWrapper
{
  std::vector<std::pair<std::string, std::string>> entries;
};

JSClassID g_headers_class_id = 0;
std::mutex g_headers_class_mutex;
std::unordered_set<JSRuntime*> g_headers_class_registered;

void HeadersFinalizer(JSRuntime* /*rt*/, JSValue value)
{
  delete static_cast<HeadersWrapper*>(JS_GetOpaque(value, g_headers_class_id));
}

HeadersWrapper* Unwrap(JSValueConst value)
{
  return static_cast<HeadersWrapper*>(JS_GetOpaque(value, g_headers_class_id));
}

std::string NormalizeName(std::string name)
{
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return name;
}

bool ReadArgument(JSContext* ctx, JSValueConst value, std::string* output)
{
  bool ok = false;
  *output = ArgString(ctx, value, &ok);
  return ok;
}

void AppendEntry(HeadersWrapper& wrapper, std::string name, std::string value)
{
  name = NormalizeName(std::move(name));
  const auto found = std::find_if(wrapper.entries.begin(),
                                  wrapper.entries.end(),
                                  [&](const auto& entry) { return entry.first == name; });
  if (found == wrapper.entries.end()) {
    wrapper.entries.emplace_back(std::move(name), std::move(value));
  } else {
    found->second += ", " + value;
  }
}

void EnsureClassRegistered(JSRuntime* rt)
{
  std::lock_guard<std::mutex> lock(g_headers_class_mutex);
  if (g_headers_class_id == 0) {
    JS_NewClassID(rt, &g_headers_class_id);
  }
  if (!g_headers_class_registered.insert(rt).second) {
    return;
  }
  JSClassDef definition{};
  definition.class_name = "Headers";
  definition.finalizer = HeadersFinalizer;
  JS_NewClass(rt, g_headers_class_id, &definition);
}

JSValue Prototype(JSContext* ctx)
{
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue constructor = JS_GetPropertyStr(ctx, global, "Headers");
  JSValue prototype = JS_GetPropertyStr(ctx, constructor, "prototype");
  JS_FreeValue(ctx, constructor);
  JS_FreeValue(ctx, global);
  return prototype;
}

JSValue NewInstance(JSContext* ctx, HeadersWrapper* wrapper, JSValueConst prototype)
{
  JSValue object = JS_NewObjectProtoClass(ctx, prototype, g_headers_class_id);
  if (JS_IsException(object)) {
    delete wrapper;
    return object;
  }
  JS_SetOpaque(object, wrapper);
  return object;
}

JSValue Constructor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
  auto* wrapper = new HeadersWrapper();
  if (argc > 0 && JS_IsObject(argv[0]) && Unwrap(argv[0]) != nullptr) {
    wrapper->entries = Unwrap(argv[0])->entries;
  }
  JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
  JSValue object = NewInstance(ctx, wrapper, prototype);
  JS_FreeValue(ctx, prototype);
  return object;
}

JSValue Append(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  HeadersWrapper* wrapper = Unwrap(this_val);
  if (wrapper == nullptr || argc < 2) {
    return JS_ThrowTypeError(ctx, "illegal invocation");
  }
  std::string name;
  std::string value;
  if (!ReadArgument(ctx, argv[0], &name) || !ReadArgument(ctx, argv[1], &value)) {
    return JS_EXCEPTION;
  }
  AppendEntry(*wrapper, std::move(name), std::move(value));
  return JS_UNDEFINED;
}

JSValue Set(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  HeadersWrapper* wrapper = Unwrap(this_val);
  if (wrapper == nullptr || argc < 2) {
    return JS_ThrowTypeError(ctx, "illegal invocation");
  }
  std::string name;
  std::string value;
  if (!ReadArgument(ctx, argv[0], &name) || !ReadArgument(ctx, argv[1], &value)) {
    return JS_EXCEPTION;
  }
  name = NormalizeName(std::move(name));
  const auto found = std::find_if(wrapper->entries.begin(),
                                  wrapper->entries.end(),
                                  [&](const auto& entry) { return entry.first == name; });
  if (found == wrapper->entries.end()) {
    wrapper->entries.emplace_back(std::move(name), std::move(value));
  } else {
    found->second = std::move(value);
  }
  return JS_UNDEFINED;
}

JSValue Get(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  HeadersWrapper* wrapper = Unwrap(this_val);
  if (wrapper == nullptr || argc < 1) {
    return JS_ThrowTypeError(ctx, "illegal invocation");
  }
  std::string name;
  if (!ReadArgument(ctx, argv[0], &name)) {
    return JS_EXCEPTION;
  }
  name = NormalizeName(std::move(name));
  const auto found = std::find_if(wrapper->entries.begin(),
                                  wrapper->entries.end(),
                                  [&](const auto& entry) { return entry.first == name; });
  return found == wrapper->entries.end() ? JS_NULL : JS_NewString(ctx, found->second.c_str());
}

JSValue Has(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  HeadersWrapper* wrapper = Unwrap(this_val);
  if (wrapper == nullptr || argc < 1) {
    return JS_ThrowTypeError(ctx, "illegal invocation");
  }
  std::string name;
  if (!ReadArgument(ctx, argv[0], &name)) {
    return JS_EXCEPTION;
  }
  name = NormalizeName(std::move(name));
  return JS_NewBool(
      ctx, std::any_of(wrapper->entries.begin(), wrapper->entries.end(), [&](const auto& entry) {
        return entry.first == name;
      }));
}

JSValue Delete(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  HeadersWrapper* wrapper = Unwrap(this_val);
  if (wrapper == nullptr || argc < 1) {
    return JS_ThrowTypeError(ctx, "illegal invocation");
  }
  std::string name;
  if (!ReadArgument(ctx, argv[0], &name)) {
    return JS_EXCEPTION;
  }
  name = NormalizeName(std::move(name));
  wrapper->entries.erase(std::remove_if(wrapper->entries.begin(),
                                        wrapper->entries.end(),
                                        [&](const auto& entry) { return entry.first == name; }),
                         wrapper->entries.end());
  return JS_UNDEFINED;
}

JSValue Entries(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  HeadersWrapper* wrapper = Unwrap(this_val);
  if (wrapper == nullptr) {
    return JS_ThrowTypeError(ctx, "illegal invocation");
  }
  JSValue array = JS_NewArray(ctx);
  uint32_t index = 0;
  for (const auto& [name, value] : wrapper->entries) {
    JSValue pair = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, pair, 0, JS_NewString(ctx, name.c_str()));
    JS_SetPropertyUint32(ctx, pair, 1, JS_NewString(ctx, value.c_str()));
    JS_SetPropertyUint32(ctx, array, index++, pair);
  }
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue symbol = JS_GetPropertyStr(ctx, global, "Symbol");
  JSValue iterator_key = JS_GetPropertyStr(ctx, symbol, "iterator");
  JSAtom atom = JS_ValueToAtom(ctx, iterator_key);
  JSValue iterator = JS_GetProperty(ctx, array, atom);
  JS_FreeAtom(ctx, atom);
  JS_FreeValue(ctx, iterator_key);
  JS_FreeValue(ctx, symbol);
  JS_FreeValue(ctx, global);
  JSValue result = JS_Call(ctx, iterator, array, 0, nullptr);
  JS_FreeValue(ctx, iterator);
  JS_FreeValue(ctx, array);
  return result;
}

} // namespace

void InstallHeadersGlobal(JSContext* ctx, JSValue global)
{
  EnsureClassRegistered(JS_GetRuntime(ctx));
  JSValue prototype = JS_NewObject(ctx);
  static const std::array<JSCFunctionListEntry, 6> kMethods = {{
      JS_CFUNC_DEF("append", 2, Append),
      JS_CFUNC_DEF("set", 2, Set),
      JS_CFUNC_DEF("get", 1, Get),
      JS_CFUNC_DEF("has", 1, Has),
      JS_CFUNC_DEF("delete", 1, Delete),
      JS_CFUNC_DEF("entries", 0, Entries),
  }};
  JS_SetPropertyFunctionList(ctx, prototype, kMethods.data(), static_cast<int>(kMethods.size()));
  JSValue entries = JS_GetPropertyStr(ctx, prototype, "entries");
  JSValue symbol = JS_GetPropertyStr(ctx, global, "Symbol");
  JSValue iterator_key = JS_GetPropertyStr(ctx, symbol, "iterator");
  JSAtom atom = JS_ValueToAtom(ctx, iterator_key);
  JS_SetProperty(ctx, prototype, atom, entries);
  JS_FreeAtom(ctx, atom);
  JS_FreeValue(ctx, iterator_key);
  JS_FreeValue(ctx, symbol);

  JSValue constructor = JS_NewCFunction2(ctx, Constructor, "Headers", 1, JS_CFUNC_constructor, 0);
  JS_SetPropertyStr(ctx, constructor, "prototype", JS_DupValue(ctx, prototype));
  JS_SetPropertyStr(ctx, prototype, "constructor", JS_DupValue(ctx, constructor));
  JS_SetPropertyStr(ctx, global, "Headers", constructor);
  JS_FreeValue(ctx, prototype);
}

void ForgetHeadersRuntime(JSRuntime* rt)
{
  std::lock_guard<std::mutex> lock(g_headers_class_mutex);
  g_headers_class_registered.erase(rt);
}

JSValue MakeHeaders(JSContext* ctx, const std::vector<std::pair<std::string, std::string>>& entries)
{
  auto* wrapper = new HeadersWrapper();
  for (const auto& [name, value] : entries) {
    AppendEntry(*wrapper, name, value);
  }
  JSValue prototype = Prototype(ctx);
  JSValue object = NewInstance(ctx, wrapper, prototype);
  JS_FreeValue(ctx, prototype);
  return object;
}

} // namespace neko::javascript