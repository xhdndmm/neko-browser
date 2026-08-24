#include "binding_internal.h"

#include <algorithm>
#include <array>
#include <quickjs.h>
#include <string>
#include <utility>
#include <vector>

namespace neko::javascript {

namespace {

struct FormDataWrapper
{
  std::vector<std::pair<std::string, std::string>> entries;
};

JSClassID g_form_data_class_id = 0;
std::mutex g_form_data_class_mutex;
std::unordered_set<JSRuntime*> g_form_data_class_registered;

void FormDataFinalizer(JSRuntime* /*runtime*/, JSValue object)
{
  delete static_cast<FormDataWrapper*>(JS_GetOpaque(object, g_form_data_class_id));
}

FormDataWrapper* UnwrapFormData(JSValueConst value)
{
  return static_cast<FormDataWrapper*>(JS_GetOpaque(value, g_form_data_class_id));
}

bool ReadString(JSContext* ctx, JSValueConst value, std::string* output)
{
  bool ok = false;
  *output = ArgString(ctx, value, &ok);
  return ok;
}

JSValue FormDataConstructor(JSContext* ctx, JSValueConst new_target, int, JSValueConst*)
{
  JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
  JSValue object = JS_NewObjectProtoClass(ctx, proto, g_form_data_class_id);
  JS_FreeValue(ctx, proto);
  if (JS_IsException(object))
    return object;
  JS_SetOpaque(object, new FormDataWrapper());
  return object;
}

JSValue FormDataAppend(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  auto* form_data = UnwrapFormData(this_val);
  if (form_data == nullptr || argc < 2)
    return JS_ThrowTypeError(ctx, "illegal invocation");
  std::string name;
  std::string value;
  if (!ReadString(ctx, argv[0], &name) || !ReadString(ctx, argv[1], &value))
    return JS_EXCEPTION;
  form_data->entries.emplace_back(std::move(name), std::move(value));
  return JS_UNDEFINED;
}

JSValue FormDataGet(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  auto* form_data = UnwrapFormData(this_val);
  if (form_data == nullptr || argc < 1)
    return JS_ThrowTypeError(ctx, "illegal invocation");
  std::string name;
  if (!ReadString(ctx, argv[0], &name))
    return JS_EXCEPTION;
  const auto entry = std::find_if(form_data->entries.begin(),
                                  form_data->entries.end(),
                                  [&name](const auto& item) { return item.first == name; });
  return entry == form_data->entries.end() ? JS_NULL : JS_NewString(ctx, entry->second.c_str());
}

JSValue FormDataHas(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  auto* form_data = UnwrapFormData(this_val);
  if (form_data == nullptr || argc < 1)
    return JS_ThrowTypeError(ctx, "illegal invocation");
  std::string name;
  if (!ReadString(ctx, argv[0], &name))
    return JS_EXCEPTION;
  return JS_NewBool(ctx,
                    std::any_of(form_data->entries.begin(),
                                form_data->entries.end(),
                                [&name](const auto& item) { return item.first == name; }));
}

JSValue FormDataDelete(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  auto* form_data = UnwrapFormData(this_val);
  if (form_data == nullptr || argc < 1)
    return JS_ThrowTypeError(ctx, "illegal invocation");
  std::string name;
  if (!ReadString(ctx, argv[0], &name))
    return JS_EXCEPTION;
  std::erase_if(form_data->entries, [&name](const auto& item) { return item.first == name; });
  return JS_UNDEFINED;
}

JSValue FormDataSet(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  auto* form_data = UnwrapFormData(this_val);
  if (form_data == nullptr || argc < 2)
    return JS_ThrowTypeError(ctx, "illegal invocation");
  std::string name;
  std::string value;
  if (!ReadString(ctx, argv[0], &name) || !ReadString(ctx, argv[1], &value))
    return JS_EXCEPTION;
  const auto first = std::find_if(form_data->entries.begin(),
                                  form_data->entries.end(),
                                  [&name](const auto& item) { return item.first == name; });
  if (first == form_data->entries.end()) {
    form_data->entries.emplace_back(std::move(name), std::move(value));
  } else {
    first->second = std::move(value);
    form_data->entries.erase(
        std::remove_if(std::next(first),
                       form_data->entries.end(),
                       [&name](const auto& item) { return item.first == name; }),
        form_data->entries.end());
  }
  return JS_UNDEFINED;
}

JSValue FormDataEntries(JSContext* ctx, JSValueConst this_val, int, JSValueConst*)
{
  auto* form_data = UnwrapFormData(this_val);
  if (form_data == nullptr)
    return JS_ThrowTypeError(ctx, "illegal invocation");
  JSValue entries = JS_NewArray(ctx);
  for (uint32_t index = 0; index < form_data->entries.size(); ++index) {
    JSValue pair = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, pair, 0, JS_NewString(ctx, form_data->entries[index].first.c_str()));
    JS_SetPropertyUint32(ctx, pair, 1, JS_NewString(ctx, form_data->entries[index].second.c_str()));
    JS_SetPropertyUint32(ctx, entries, index, pair);
  }
  JSValue values_function = JS_GetPropertyStr(ctx, entries, "values");
  JSValue iterator = JS_Call(ctx, values_function, entries, 0, nullptr);
  JS_FreeValue(ctx, values_function);
  JS_FreeValue(ctx, entries);
  return iterator;
}

} // namespace

void ForgetFormDataRuntime(JSRuntime* runtime)
{
  std::lock_guard<std::mutex> lock(g_form_data_class_mutex);
  g_form_data_class_registered.erase(runtime);
}

void InstallFormDataGlobal(JSContext* ctx, JSValue global)
{
  JSRuntime* runtime = JS_GetRuntime(ctx);
  {
    std::lock_guard<std::mutex> lock(g_form_data_class_mutex);
    JS_NewClassID(runtime, &g_form_data_class_id);
    if (g_form_data_class_registered.insert(runtime).second) {
      JSClassDef definition{};
      definition.class_name = "FormData";
      definition.finalizer = &FormDataFinalizer;
      JS_NewClass(runtime, g_form_data_class_id, &definition);
    }
  }

  JSValue proto = JS_NewObject(ctx);
  static const std::array<JSCFunctionListEntry, 6> methods = {{
      JS_CFUNC_DEF("append", 2, FormDataAppend),
      JS_CFUNC_DEF("set", 2, FormDataSet),
      JS_CFUNC_DEF("delete", 1, FormDataDelete),
      JS_CFUNC_DEF("get", 1, FormDataGet),
      JS_CFUNC_DEF("has", 1, FormDataHas),
      JS_CFUNC_DEF("entries", 0, FormDataEntries),
  }};
  JS_SetPropertyFunctionList(ctx, proto, methods.data(), static_cast<int>(methods.size()));

  JSValue constructor =
      JS_NewCFunction2(ctx, FormDataConstructor, "FormData", 0, JS_CFUNC_constructor, 0);
  JS_SetPropertyStr(ctx, constructor, "prototype", JS_DupValue(ctx, proto));
  JS_SetPropertyStr(ctx, proto, "constructor", JS_DupValue(ctx, constructor));
  JS_SetPropertyStr(ctx, global, "FormData", constructor);
  JS_FreeValue(ctx, proto);
}

} // namespace neko::javascript