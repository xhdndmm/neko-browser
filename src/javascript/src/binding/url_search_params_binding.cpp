#include "binding_internal.h"

#include <algorithm>
#include <cctype>
#include <quickjs.h>
#include <string>
#include <utility>
#include <vector>

namespace neko::javascript {

namespace {

struct UrlSearchParamsWrapper
{
  std::vector<std::pair<std::string, std::string>> entries;
};

JSClassID g_url_search_params_class_id = 0;
std::mutex g_url_search_params_class_mutex;
std::unordered_set<JSRuntime*> g_url_search_params_class_registered;

void UrlSearchParamsFinalizer(JSRuntime* /*rt*/, JSValue obj)
{
  delete static_cast<UrlSearchParamsWrapper*>(JS_GetOpaque(obj, g_url_search_params_class_id));
}

UrlSearchParamsWrapper* Unwrap(JSValueConst value)
{
  auto* wrapper =
      static_cast<UrlSearchParamsWrapper*>(JS_GetOpaque(value, g_url_search_params_class_id));
  return wrapper != nullptr ? wrapper : nullptr;
}

int HexValue(char ch)
{
  if (ch >= '0' && ch <= '9')
    return ch - '0';
  if (ch >= 'a' && ch <= 'f')
    return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F')
    return ch - 'A' + 10;
  return -1;
}

std::string Decode(std::string_view input)
{
  std::string output;
  for (std::size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '+') {
      output.push_back(' ');
    } else if (input[i] == '%' && i + 2 < input.size()) {
      const int high = HexValue(input[i + 1]);
      const int low = HexValue(input[i + 2]);
      if (high >= 0 && low >= 0) {
        output.push_back(static_cast<char>((high << 4) | low));
        i += 2;
      } else {
        output.push_back('%');
      }
    } else {
      output.push_back(input[i]);
    }
  }
  return output;
}

bool IsFormSafe(unsigned char ch)
{
  return std::isalnum(ch) != 0 || ch == '*' || ch == '-' || ch == '.' || ch == '_';
}

std::string Encode(std::string_view input)
{
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string output;
  for (const char raw_ch : input) {
    const auto ch = static_cast<unsigned char>(raw_ch);
    if (ch == ' ') {
      output.push_back('+');
    } else if (IsFormSafe(ch)) {
      output.push_back(static_cast<char>(ch));
    } else {
      output.push_back('%');
      output.push_back(kHex[ch >> 4]);
      output.push_back(kHex[ch & 0x0f]);
    }
  }
  return output;
}

void Parse(std::string_view input, UrlSearchParamsWrapper& wrapper)
{
  if (!input.empty() && input.front() == '?')
    input.remove_prefix(1);
  std::size_t start = 0;
  while (start <= input.size()) {
    const std::size_t end = input.find('&', start);
    const auto field =
        input.substr(start, end == std::string_view::npos ? input.size() - start : end - start);
    const std::size_t equals = field.find('=');
    wrapper.entries.emplace_back(
        Decode(field.substr(0, equals)),
        equals == std::string_view::npos ? std::string() : Decode(field.substr(equals + 1)));
    if (end == std::string_view::npos)
      break;
    start = end + 1;
  }
}

JSValue NewInstance(JSContext* ctx, UrlSearchParamsWrapper* wrapper, JSValueConst proto)
{
  JSValue object = JS_NewObjectProtoClass(ctx, proto, g_url_search_params_class_id);
  if (JS_IsException(object)) {
    delete wrapper;
    return object;
  }
  JS_SetOpaque(object, wrapper);
  return object;
}

JSValue Constructor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
  auto* wrapper = new UrlSearchParamsWrapper();
  if (argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
    bool ok = false;
    const std::string input = ArgString(ctx, argv[0], &ok);
    if (!ok) {
      delete wrapper;
      return JS_EXCEPTION;
    }
    Parse(input, *wrapper);
  }
  JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
  JSValue object = NewInstance(ctx, wrapper, proto);
  JS_FreeValue(ctx, proto);
  return object;
}

JSValue ToString(JSContext* ctx, JSValueConst this_val, int, JSValueConst*)
{
  auto* wrapper = Unwrap(this_val);
  if (wrapper == nullptr)
    return JS_ThrowTypeError(ctx, "illegal invocation");
  std::string output;
  for (const auto& [key, value] : wrapper->entries) {
    if (!output.empty())
      output.push_back('&');
    output += Encode(key) + '=' + Encode(value);
  }
  return JS_NewString(ctx, output.c_str());
}

JSValue Get(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  auto* wrapper = Unwrap(this_val);
  if (wrapper == nullptr || argc < 1)
    return JS_ThrowTypeError(ctx, "illegal invocation");
  bool ok = false;
  const std::string key = ArgString(ctx, argv[0], &ok);
  if (!ok)
    return JS_EXCEPTION;
  for (const auto& entry : wrapper->entries)
    if (entry.first == key)
      return JS_NewString(ctx, entry.second.c_str());
  return JS_NULL;
}

JSValue GetAll(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  auto* wrapper = Unwrap(this_val);
  if (wrapper == nullptr || argc < 1)
    return JS_ThrowTypeError(ctx, "illegal invocation");
  bool ok = false;
  const std::string key = ArgString(ctx, argv[0], &ok);
  if (!ok)
    return JS_EXCEPTION;
  JSValue array = JS_NewArray(ctx);
  uint32_t index = 0;
  for (const auto& entry : wrapper->entries)
    if (entry.first == key)
      JS_SetPropertyUint32(ctx, array, index++, JS_NewString(ctx, entry.second.c_str()));
  return array;
}

JSValue Has(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  auto* wrapper = Unwrap(this_val);
  if (wrapper == nullptr || argc < 1)
    return JS_ThrowTypeError(ctx, "illegal invocation");
  bool ok = false;
  const std::string key = ArgString(ctx, argv[0], &ok);
  if (!ok)
    return JS_EXCEPTION;
  return JS_NewBool(
      ctx, std::any_of(wrapper->entries.begin(), wrapper->entries.end(), [&](const auto& entry) {
        return entry.first == key;
      }));
}

JSValue Append(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  auto* wrapper = Unwrap(this_val);
  if (wrapper == nullptr || argc < 2)
    return JS_ThrowTypeError(ctx, "illegal invocation");
  bool key_ok = false;
  bool value_ok = false;
  const std::string key = ArgString(ctx, argv[0], &key_ok);
  const std::string value = ArgString(ctx, argv[1], &value_ok);
  if (!key_ok || !value_ok)
    return JS_EXCEPTION;
  wrapper->entries.emplace_back(key, value);
  return JS_UNDEFINED;
}

JSValue Set(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  auto* wrapper = Unwrap(this_val);
  if (wrapper == nullptr || argc < 2)
    return JS_ThrowTypeError(ctx, "illegal invocation");
  bool key_ok = false;
  bool value_ok = false;
  const std::string key = ArgString(ctx, argv[0], &key_ok);
  const std::string value = ArgString(ctx, argv[1], &value_ok);
  if (!key_ok || !value_ok)
    return JS_EXCEPTION;
  const auto first = std::find_if(wrapper->entries.begin(),
                                  wrapper->entries.end(),
                                  [&](const auto& entry) { return entry.first == key; });
  if (first == wrapper->entries.end()) {
    wrapper->entries.emplace_back(key, value);
  } else {
    first->second = value;
    wrapper->entries.erase(std::remove_if(std::next(first),
                                          wrapper->entries.end(),
                                          [&](const auto& entry) { return entry.first == key; }),
                           wrapper->entries.end());
  }
  return JS_UNDEFINED;
}

JSValue Delete(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  auto* wrapper = Unwrap(this_val);
  if (wrapper == nullptr || argc < 1)
    return JS_ThrowTypeError(ctx, "illegal invocation");
  bool ok = false;
  const std::string key = ArgString(ctx, argv[0], &ok);
  if (!ok)
    return JS_EXCEPTION;
  wrapper->entries.erase(std::remove_if(wrapper->entries.begin(),
                                        wrapper->entries.end(),
                                        [&](const auto& entry) { return entry.first == key; }),
                         wrapper->entries.end());
  return JS_UNDEFINED;
}

JSValue Sort(JSContext* ctx, JSValueConst this_val, int, JSValueConst*)
{
  auto* wrapper = Unwrap(this_val);
  if (wrapper == nullptr)
    return JS_ThrowTypeError(ctx, "illegal invocation");
  std::stable_sort(wrapper->entries.begin(),
                   wrapper->entries.end(),
                   [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
  return JS_UNDEFINED;
}

JSValue Size(JSContext* ctx, JSValueConst this_val)
{
  auto* wrapper = Unwrap(this_val);
  if (wrapper == nullptr)
    return JS_ThrowTypeError(ctx, "illegal invocation");
  return JS_NewInt64(ctx, static_cast<int64_t>(wrapper->entries.size()));
}

} // namespace

void EnsureUrlSearchParamsClassRegistered(JSRuntime* rt)
{
  std::lock_guard<std::mutex> lock(g_url_search_params_class_mutex);
  JS_NewClassID(rt, &g_url_search_params_class_id);
  if (g_url_search_params_class_registered.insert(rt).second) {
    JSClassDef definition{};
    definition.class_name = "URLSearchParams";
    definition.finalizer = &UrlSearchParamsFinalizer;
    JS_NewClass(rt, g_url_search_params_class_id, &definition);
  }
}

void ForgetUrlSearchParamsRuntime(JSRuntime* rt)
{
  std::lock_guard<std::mutex> lock(g_url_search_params_class_mutex);
  g_url_search_params_class_registered.erase(rt);
}

void InstallUrlSearchParamsGlobal(JSContext* ctx, JSValue global)
{
  EnsureUrlSearchParamsClassRegistered(JS_GetRuntime(ctx));
  JSValue proto = JS_NewObject(ctx);
  static const std::array<JSCFunctionListEntry, 9> methods = {{
      JS_CFUNC_DEF("append", 2, Append),
      JS_CFUNC_DEF("set", 2, Set),
      JS_CFUNC_DEF("delete", 1, Delete),
      JS_CFUNC_DEF("get", 1, Get),
      JS_CFUNC_DEF("getAll", 1, GetAll),
      JS_CFUNC_DEF("has", 1, Has),
      JS_CFUNC_DEF("sort", 0, Sort),
      JS_CFUNC_DEF("toString", 0, ToString),
      JS_CGETSET_DEF("size", Size, nullptr),
  }};
  JS_SetPropertyFunctionList(ctx, proto, methods.data(), static_cast<int>(methods.size()));
  JSValue ctor = JS_NewCFunction2(ctx, Constructor, "URLSearchParams", 1, JS_CFUNC_constructor, 0);
  JS_SetPropertyStr(ctx, ctor, "prototype", JS_DupValue(ctx, proto));
  JS_SetPropertyStr(ctx, proto, "constructor", JS_DupValue(ctx, ctor));
  JS_SetPropertyStr(ctx, global, "URLSearchParams", ctor);
  JS_FreeValue(ctx, proto);
}

} // namespace neko::javascript