#include "neko/url/url.h"

#include "binding_internal.h"

#include <quickjs.h>
#include <string>

namespace neko::javascript {

namespace {

JSValue StringProperty(JSContext* ctx, std::string_view value)
{
  return JS_NewStringLen(ctx, value.data(), value.size());
}

JSValue Constructor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "URL constructor requires an input");
  }

  const char* input_text = JS_ToCString(ctx, argv[0]);
  if (input_text == nullptr)
    return JS_EXCEPTION;
  const std::string input(input_text);
  JS_FreeCString(ctx, input_text);

  base::Result<url::Url> parsed = url::Url::Parse(input);
  if (!parsed.has_value() && argc >= 2 && !JS_IsUndefined(argv[1])) {
    const char* base_text = JS_ToCString(ctx, argv[1]);
    if (base_text == nullptr)
      return JS_EXCEPTION;
    const auto base = url::Url::Parse(base_text);
    JS_FreeCString(ctx, base_text);
    if (base.has_value())
      parsed = url::Url::Parse(input, base.value());
  }
  if (!parsed.has_value()) {
    return JS_ThrowTypeError(ctx, "invalid URL");
  }

  const url::Url& value = parsed.value();
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue prototype = JS_GetPropertyStr(ctx, global, "URL");
  JS_FreeValue(ctx, global);
  JSValue object_prototype = JS_GetPropertyStr(ctx, prototype, "prototype");
  JS_FreeValue(ctx, prototype);
  JSValue object = JS_NewObjectProto(ctx, object_prototype);
  JS_FreeValue(ctx, object_prototype);

  const std::string href = value.Serialize(true);
  const std::string protocol = value.scheme() + ":";
  const std::string host =
      value.host() + (value.port().has_value() ? ":" + std::to_string(*value.port()) : "");
  const std::string search = value.has_query() ? "?" + value.query() : "";
  const std::string hash = value.has_fragment() ? "#" + value.fragment() : "";
  JS_SetPropertyStr(ctx, object, "href", StringProperty(ctx, href));
  JS_SetPropertyStr(ctx, object, "origin", StringProperty(ctx, value.Origin()));
  JS_SetPropertyStr(ctx, object, "protocol", StringProperty(ctx, protocol));
  JS_SetPropertyStr(ctx, object, "username", StringProperty(ctx, value.username()));
  JS_SetPropertyStr(ctx, object, "password", StringProperty(ctx, value.password()));
  JS_SetPropertyStr(ctx, object, "host", StringProperty(ctx, host));
  JS_SetPropertyStr(ctx, object, "hostname", StringProperty(ctx, value.host()));
  JS_SetPropertyStr(
      ctx,
      object,
      "port",
      StringProperty(ctx, value.port().has_value() ? std::to_string(*value.port()) : ""));
  JS_SetPropertyStr(ctx, object, "pathname", StringProperty(ctx, value.path()));
  JS_SetPropertyStr(ctx, object, "search", StringProperty(ctx, search));
  JS_SetPropertyStr(ctx, object, "hash", StringProperty(ctx, hash));
  return object;
}

JSValue ToString(JSContext* ctx, JSValueConst this_value, int, JSValueConst*)
{
  return JS_GetPropertyStr(ctx, this_value, "href");
}

} // namespace

void InstallUrlGlobal(JSContext* ctx, JSValue global)
{
  JSValue prototype = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, prototype, "toString", JS_NewCFunction(ctx, ToString, "toString", 0));
  JS_SetPropertyStr(ctx, prototype, "toJSON", JS_NewCFunction(ctx, ToString, "toJSON", 0));

  JSValue constructor = JS_NewCFunction2(ctx, Constructor, "URL", 1, JS_CFUNC_constructor, 0);
  JS_SetConstructor(ctx, constructor, prototype);
  JS_SetPropertyStr(ctx, prototype, "constructor", JS_DupValue(ctx, constructor));
  JS_SetPropertyStr(ctx,
                    constructor,
                    "createObjectURL",
                    JS_NewCFunction(ctx, UrlCreateObjectUrl, "createObjectURL", 1));
  JS_SetPropertyStr(ctx,
                    constructor,
                    "revokeObjectURL",
                    JS_NewCFunction(ctx, UrlRevokeObjectUrl, "revokeObjectURL", 1));
  JS_SetPropertyStr(ctx, global, "URL", constructor);
  JS_FreeValue(ctx, prototype);
}

} // namespace neko::javascript