// neko::javascript DOM bindings — platform APIs that pages and frameworks use
// before touching the DOM: TextEncoder / TextDecoder, atob / btoa,
// queueMicrotask, structuredClone, reportError and CSS (escape / supports).
//
// Everything here is implemented on top of the engine's own subsystems:
//   TextEncoder    -> the runtime's strings are UTF-8
//   TextDecoder    -> neko::base::encoding (WHATWG Encoding Standard decoders,
//                     so every label the engine can decode is supported)
//   atob/btoa      -> a local RFC 4648 codec (Latin-1 semantics, like browsers)
//   structuredClone-> a real recursive clone (cycles, Date, ArrayBuffer and
//                     typed arrays included); Map/Set/Blob/File are rejected
//                     with a DataCloneError, as the spec permits for
//                     non-cloneable values
//   CSS.escape     -> the CSSOM ident-escape algorithm
//   CSS.supports   -> the engine's own CSS value parser (a declaration is
//                     "supported" when it parses)
//
// Documented limitations: TextDecoder is non-streaming (decode(bytes,
// {stream:true}) decodes what it was given and ignores the option), and
// structuredClone does not transfer (transfer lists are not implemented).

#include "neko/base/encoding.h"
#include "neko/base/utf8.h"
#include "neko/css/parser.h"
#include "neko/css/value.h"

#include "binding_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <quickjs.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace neko::javascript {
namespace {

JSValue NewString(JSContext* ctx, std::string_view text)
{
  return JS_NewStringLen(ctx, text.data(), text.size());
}

bool ToString(JSContext* ctx, JSValueConst value, std::string* out)
{
  const char* text = JS_ToCString(ctx, value);
  if (text == nullptr) {
    return false;
  }
  out->assign(text);
  JS_FreeCString(ctx, text);
  return true;
}

// Wraps |bytes| in a Uint8Array (the view, not the raw buffer).
JSValue NewUint8Array(JSContext* ctx, std::string_view bytes)
{
  JSValue buffer =
      JS_NewArrayBufferCopy(ctx, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
  if (JS_IsException(buffer)) {
    return buffer;
  }
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue ctor = JS_GetPropertyStr(ctx, global, "Uint8Array");
  JSValue args[1] = {buffer};
  JSValue array = JS_CallConstructor(ctx, ctor, 1, args);
  JS_FreeValue(ctx, ctor);
  JS_FreeValue(ctx, global);
  JS_FreeValue(ctx, buffer);
  return array;
}

// Reads a BufferSource (ArrayBuffer or any typed-array/data view) as bytes.
bool ReadBytes(JSContext* ctx, JSValueConst value, std::string* out)
{
  std::size_t size = 0;
  if (std::uint8_t* data = JS_GetArrayBuffer(ctx, &size, value); data != nullptr) {
    out->assign(reinterpret_cast<const char*>(data), size);
    return true;
  }
  std::size_t byte_offset = 0;
  std::size_t length = 0;
  JSValue buffer = JS_GetTypedArrayBuffer(ctx, value, &byte_offset, &length, nullptr);
  if (JS_IsException(buffer)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return false;
  }
  std::size_t buffer_size = 0;
  std::uint8_t* buffer_data = JS_GetArrayBuffer(ctx, &buffer_size, buffer);
  JS_FreeValue(ctx, buffer);
  if (buffer_data == nullptr) {
    return false;
  }
  out->assign(reinterpret_cast<const char*>(buffer_data + byte_offset), length);
  return true;
}

// ---------------------------------------------------------------------------
// TextEncoder / TextDecoder
// ---------------------------------------------------------------------------

// Class ids are registered once per runtime; the registry is cleared on binder
// teardown (ForgetPlatformRuntime) because JSRuntime addresses get reused.
JSClassID g_text_encoder_class_id = 0;
JSClassID g_text_decoder_class_id = 0;
std::mutex g_platform_class_mutex;
std::unordered_set<JSRuntime*> g_text_classes_registered;

struct TextDecoderState
{
  base::encoding::Charset charset = base::encoding::Charset::kUtf8;
  std::string label = "utf-8";
};

void TextEncoderFinalizer(JSRuntime* /*rt*/, JSValue /*obj*/)
{
  // No owned state.
}

void TextDecoderFinalizer(JSRuntime* rt, JSValue obj)
{
  delete static_cast<TextDecoderState*>(JS_GetOpaque(obj, g_text_decoder_class_id));
}

JSValue TextEncoderConstructor(JSContext* ctx,
                               JSValueConst /*new_target*/,
                               int /*argc*/,
                               JSValueConst* /*argv*/)
{
  return JS_NewObjectClass(ctx, g_text_encoder_class_id);
}

JSValue TextEncoderEncode(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  std::string input;
  if (argc > 0 && !JS_IsUndefined(argv[0]) && !ToString(ctx, argv[0], &input)) {
    return JS_EXCEPTION;
  }
  // Runtime strings are UTF-8 already, so the encoding is the identity.
  return NewUint8Array(ctx, input);
}

JSValue
TextEncoderEncodeInto(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  std::string input;
  if (argc > 0 && !ToString(ctx, argv[0], &input)) {
    return JS_EXCEPTION;
  }
  if (argc < 2) {
    return JS_ThrowTypeError(ctx, "encodeInto requires a Uint8Array destination");
  }
  std::size_t size = 0;
  std::uint8_t* data = nullptr;
  std::size_t byte_offset = 0;
  std::size_t length = 0;
  if (std::uint8_t* raw = JS_GetArrayBuffer(ctx, &size, argv[1]); raw != nullptr) {
    data = raw;
  } else {
    JSValue buffer = JS_GetTypedArrayBuffer(ctx, argv[1], &byte_offset, &length, nullptr);
    if (JS_IsException(buffer)) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      return JS_ThrowTypeError(ctx, "encodeInto requires a Uint8Array destination");
    }
    std::size_t buffer_size = 0;
    std::uint8_t* buffer_data = JS_GetArrayBuffer(ctx, &buffer_size, buffer);
    JS_FreeValue(ctx, buffer);
    if (buffer_data == nullptr) {
      return JS_ThrowTypeError(ctx, "encodeInto requires a Uint8Array destination");
    }
    data = buffer_data + byte_offset;
    size = length;
  }
  // Only whole UTF-8 sequences are written, so the result stays decodable.
  std::size_t written = 0;
  while (written < size && written < input.size()) {
    const auto lead = static_cast<unsigned char>(input[written]);
    std::size_t sequence = 1;
    if (lead >= 0xf0) {
      sequence = 4;
    } else if (lead >= 0xe0) {
      sequence = 3;
    } else if (lead >= 0xc0) {
      sequence = 2;
    }
    if (written + sequence > size) {
      break;
    }
    std::memcpy(data + written, input.data() + written, sequence);
    written += sequence;
  }
  JSValue result = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, result, "read", JS_NewInt32(ctx, static_cast<int>(written)));
  JS_SetPropertyStr(ctx, result, "written", JS_NewInt32(ctx, static_cast<int>(written)));
  return result;
}

JSValue
TextDecoderConstructor(JSContext* ctx, JSValueConst /*new_target*/, int argc, JSValueConst* argv)
{
  std::string label = "utf-8";
  if (argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
    if (!ToString(ctx, argv[0], &label)) {
      return JS_EXCEPTION;
    }
  }
  const auto charset = base::encoding::CharsetFromLabel(label);
  if (!charset.has_value()) {
    return JS_ThrowRangeError(ctx, "unsupported TextDecoder label '%s'", label.c_str());
  }
  JSValue obj = JS_NewObjectClass(ctx, g_text_decoder_class_id);
  if (JS_IsException(obj)) {
    return obj;
  }
  auto* state = new TextDecoderState;
  state->charset = charset.value();
  state->label = base::encoding::CharsetName(charset.value()).empty()
                     ? label
                     : std::string(base::encoding::CharsetName(charset.value()));
  // Browsers report the canonical name lowercased ("gb18030", "iso-8859-2").
  for (char& c : state->label) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  JS_SetOpaque(obj, state);
  return obj;
}

JSValue TextDecoderDecode(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  auto* state = static_cast<TextDecoderState*>(JS_GetOpaque(this_val, g_text_decoder_class_id));
  if (state == nullptr) {
    return JS_ThrowTypeError(ctx, "not a TextDecoder");
  }
  std::string bytes;
  if (argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0]) &&
      !ReadBytes(ctx, argv[0], &bytes)) {
    return JS_ThrowTypeError(ctx, "TextDecoder.decode expects a BufferSource");
  }
  return NewString(ctx, base::encoding::DecodeToUtf8(bytes, state->charset));
}

JSValue EncodingGetterImpl(JSContext* ctx, JSValueConst this_val)
{
  auto* state = static_cast<TextDecoderState*>(JS_GetOpaque(this_val, g_text_decoder_class_id));
  if (state != nullptr) {
    return NewString(ctx, state->label);
  }
  return NewString(ctx, "utf-8"); // TextEncoder
}

// ---------------------------------------------------------------------------
// atob / btoa
// ---------------------------------------------------------------------------

std::string Base64Encode(std::string_view data)
{
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((data.size() + 2) / 3) * 4);
  const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
  std::size_t i = 0;
  for (; i + 3 <= data.size(); i += 3) {
    const std::uint32_t group =
        (std::uint32_t(bytes[i]) << 16) | (std::uint32_t(bytes[i + 1]) << 8) | bytes[i + 2];
    out.push_back(kAlphabet[(group >> 18) & 0x3f]);
    out.push_back(kAlphabet[(group >> 12) & 0x3f]);
    out.push_back(kAlphabet[(group >> 6) & 0x3f]);
    out.push_back(kAlphabet[group & 0x3f]);
  }
  const std::size_t rest = data.size() - i;
  if (rest == 1) {
    const std::uint32_t group = std::uint32_t(bytes[i]) << 16;
    out.push_back(kAlphabet[(group >> 18) & 0x3f]);
    out.push_back(kAlphabet[(group >> 12) & 0x3f]);
    out += "==";
  } else if (rest == 2) {
    const std::uint32_t group =
        (std::uint32_t(bytes[i]) << 16) | (std::uint32_t(bytes[i + 1]) << 8);
    out.push_back(kAlphabet[(group >> 18) & 0x3f]);
    out.push_back(kAlphabet[(group >> 12) & 0x3f]);
    out.push_back(kAlphabet[(group >> 6) & 0x3f]);
    out.push_back('=');
  }
  return out;
}

// Returns nullopt for input a browser would reject (atob throws).
std::optional<std::string> Base64Decode(std::string_view text)
{
  std::string cleaned;
  cleaned.reserve(text.size());
  std::size_t padding = 0;
  for (const char c : text) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
      continue;
    }
    if (c == '=') {
      ++padding;
      cleaned.push_back(c);
      continue;
    }
    if (padding > 0) {
      return std::nullopt; // data after the padding
    }
    cleaned.push_back(c);
  }
  if (padding > 2 || cleaned.size() % 4 == 1) {
    return std::nullopt;
  }
  while (!cleaned.empty() && cleaned.back() == '=') {
    cleaned.pop_back();
  }
  const auto value = [](char c) -> int {
    if (c >= 'A' && c <= 'Z')
      return c - 'A';
    if (c >= 'a' && c <= 'z')
      return c - 'a' + 26;
    if (c >= '0' && c <= '9')
      return c - '0' + 52;
    if (c == '+')
      return 62;
    if (c == '/')
      return 63;
    return -1;
  };
  std::string out;
  out.reserve((cleaned.size() * 3) / 4);
  std::uint32_t buffer = 0;
  int bits = 0;
  for (const char c : cleaned) {
    const int v = value(c);
    if (v < 0) {
      return std::nullopt;
    }
    buffer = (buffer << 6) | static_cast<std::uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((buffer >> bits) & 0xff));
    }
  }
  return out;
}

// Converts a UTF-8 string to Latin-1 bytes; nullopt when a code point is above
// U+00FF (what makes btoa throw in browsers).
std::optional<std::string> Utf8ToLatin1(std::string_view text)
{
  std::string out;
  out.reserve(text.size());
  std::size_t pos = 0;
  while (pos < text.size()) {
    char32_t code_point = 0;
    if (!base::DecodeUtf8Next(text, pos, code_point)) {
      return std::nullopt;
    }
    if (code_point > 0xff) {
      return std::nullopt;
    }
    out.push_back(static_cast<char>(code_point));
  }
  return out;
}

// Converts Latin-1 bytes to the UTF-8 string the runtime stores.
std::string Latin1ToUtf8(std::string_view bytes)
{
  std::string out;
  out.reserve(bytes.size());
  for (const char c : bytes) {
    out += base::EncodeUtf8(static_cast<char32_t>(static_cast<unsigned char>(c)));
  }
  return out;
}

JSValue Btoa(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  std::string input;
  if (argc < 1 || !ToString(ctx, argv[0], &input)) {
    return JS_EXCEPTION;
  }
  // btoa takes Latin-1 text: every code point must fit in one byte, like
  // browsers (UTF-8 text goes through TextEncoder instead).
  auto latin1 = Utf8ToLatin1(input);
  if (!latin1.has_value()) {
    return JS_ThrowTypeError(ctx, "btoa: the string contains characters outside Latin-1");
  }
  return NewString(ctx, Base64Encode(latin1.value()));
}

JSValue Atob(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  std::string input;
  if (argc < 1 || !ToString(ctx, argv[0], &input)) {
    return JS_EXCEPTION;
  }
  auto decoded = Base64Decode(input);
  if (!decoded.has_value()) {
    return JS_ThrowTypeError(ctx, "atob: the string is not valid base64");
  }
  // The decoded bytes are Latin-1 code units.
  return NewString(ctx, Latin1ToUtf8(decoded.value()));
}

// ---------------------------------------------------------------------------
// queueMicrotask
// ---------------------------------------------------------------------------

// Logs through the engine's own console binding (the same sink console.error
// uses), so uncaught microtask errors show up exactly like console errors.
void LogErrorToConsole(JSContext* ctx, JSValueConst text)
{
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue console = JS_GetPropertyStr(ctx, global, "console");
  JSValue error_fn = JS_GetPropertyStr(ctx, console, "error");
  if (JS_IsFunction(ctx, error_fn)) {
    JSValue result = JS_Call(ctx, error_fn, console, 1, &text);
    JS_FreeValue(ctx, result);
  }
  JS_FreeValue(ctx, error_fn);
  JS_FreeValue(ctx, console);
  JS_FreeValue(ctx, global);
}

JSValue MicrotaskJob(JSContext* ctx, int /*argc*/, JSValueConst* argv)
{
  if (argv == nullptr || !JS_IsFunction(ctx, argv[0])) {
    return JS_UNDEFINED;
  }
  JSValue result = JS_Call(ctx, argv[0], JS_UNDEFINED, 0, nullptr);
  if (JS_IsException(result)) {
    // A throwing microtask is reported (window "unhandled error" semantics)
    // instead of breaking the pump.
    JSValue error = JS_GetException(ctx);
    JSValue message = JS_NewString(ctx, "Uncaught ");
    const char* text = JS_ToCString(ctx, error);
    if (text != nullptr) {
      JSValue combined = JS_NewString(ctx, (std::string("Uncaught ") + text).c_str());
      JS_FreeValue(ctx, message);
      message = combined;
      JS_FreeCString(ctx, text);
    }
    LogErrorToConsole(ctx, message);
    JS_FreeValue(ctx, message);
    JS_FreeValue(ctx, error);
    return JS_UNDEFINED;
  }
  JS_FreeValue(ctx, result);
  return JS_UNDEFINED;
}

JSValue QueueMicrotask(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
    return JS_ThrowTypeError(ctx, "queueMicrotask requires a function");
  }
  JSValue callback = JS_DupValue(ctx, argv[0]);
  JS_EnqueueJob(ctx, MicrotaskJob, 1, &callback);
  JS_FreeValue(ctx, callback);
  return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// structuredClone
// ---------------------------------------------------------------------------

struct CloneContext
{
  JSContext* ctx = nullptr;
  // Original -> clone, so cycles and shared references stay shared.
  std::unordered_map<JSObject*, JSValue> seen;
};

// Throws a DOMException-style DataCloneError (the spec's error for
// non-cloneable values) and returns JS_EXCEPTION.
JSValue DataCloneError(JSContext* ctx, const char* what)
{
  JSValue error = JS_NewError(ctx);
  JS_SetPropertyStr(ctx, error, "name", NewString(ctx, "DataCloneError"));
  JS_SetPropertyStr(ctx, error, "message", NewString(ctx, what));
  return JS_Throw(ctx, error);
}

JSValue CloneValue(CloneContext& cx, JSValueConst value);

// Clones an ArrayBuffer or any view over one (typed array / DataView).
JSValue CloneArrayBufferLike(CloneContext& cx, JSValueConst value)
{
  JSContext* ctx = cx.ctx;
  std::size_t size = 0;
  if (std::uint8_t* data = JS_GetArrayBuffer(ctx, &size, value); data != nullptr) {
    return JS_NewArrayBufferCopy(ctx, data, size);
  }
  std::size_t byte_offset = 0;
  std::size_t length = 0;
  JSValue buffer = JS_GetTypedArrayBuffer(ctx, value, &byte_offset, &length, nullptr);
  if (JS_IsException(buffer)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_EXCEPTION; // not a buffer source
  }
  std::size_t buffer_size = 0;
  std::uint8_t* buffer_data = JS_GetArrayBuffer(ctx, &buffer_size, buffer);
  JS_FreeValue(ctx, buffer);
  if (buffer_data == nullptr) {
    return DataCloneError(ctx, "value is not cloneable");
  }
  // A typed array clones into the same class; a plain buffer clone keeps the
  // view's window into it.
  JSValue ctor = JS_GetPropertyStr(ctx, value, "constructor");
  const bool has_ctor = JS_IsFunction(ctx, ctor);
  JSValue is_view_check = has_ctor ? JS_GetPropertyStr(ctx, ctor, "name") : JS_UNDEFINED;
  const char* ctor_name = JS_IsString(is_view_check) ? JS_ToCString(ctx, is_view_check) : nullptr;
  const bool is_typed_array = ctor_name != nullptr && std::strcmp(ctor_name, "ArrayBuffer") != 0 &&
                              std::strcmp(ctor_name, "SharedArrayBuffer") != 0;
  if (ctor_name != nullptr) {
    JS_FreeCString(ctx, ctor_name);
  }
  JS_FreeValue(ctx, is_view_check);
  if (!has_ctor || !is_typed_array) {
    JS_FreeValue(ctx, ctor);
    // Nested ArrayBuffer: clone it whole.
    JSValue cloned = JS_NewArrayBufferCopy(ctx, buffer_data, buffer_size);
    return cloned;
  }
  JSValue cloned_buffer = JS_NewArrayBufferCopy(ctx, buffer_data, buffer_size);
  JSValue args[3] = {cloned_buffer,
                     JS_NewInt64(ctx, static_cast<int64_t>(byte_offset)),
                     JS_NewInt64(ctx, static_cast<int64_t>(length / std::max<std::size_t>(1, 1)))};
  // Call the view's own constructor over the cloned buffer.
  JSValue cloned_view = JS_CallConstructor(ctx, ctor, 3, args);
  JS_FreeValue(ctx, args[1]);
  JS_FreeValue(ctx, args[2]);
  JS_FreeValue(ctx, cloned_buffer);
  JS_FreeValue(ctx, ctor);
  return cloned_view;
}

JSValue CloneObject(CloneContext& cx, JSValueConst value)
{
  JSContext* ctx = cx.ctx;
  // Date: clone from its numeric time value.
  JSValue tag = JS_GetPropertyStr(ctx, value, "constructor");
  if (JS_IsFunction(ctx, tag)) {
    JSValue name = JS_GetPropertyStr(ctx, tag, "name");
    const char* ctor = JS_IsString(name) ? JS_ToCString(ctx, name) : nullptr;
    const std::string ctor_name = ctor != nullptr ? ctor : "";
    if (ctor != nullptr) {
      JS_FreeCString(ctx, ctor);
    }
    JS_FreeValue(ctx, name);
    if (ctor_name == "Date") {
      JSValue time_fn = JS_GetPropertyStr(ctx, value, "getTime");
      JSValue millis =
          JS_IsFunction(ctx, time_fn) ? JS_Call(ctx, time_fn, value, 0, nullptr) : JS_UNDEFINED;
      JS_FreeValue(ctx, time_fn);
      if (JS_IsException(millis)) {
        JS_FreeValue(ctx, tag);
        return millis;
      }
      JSValue args[1] = {millis};
      JSValue cloned = JS_CallConstructor(ctx, tag, 1, args);
      JS_FreeValue(ctx, millis);
      JS_FreeValue(ctx, tag);
      return cloned;
    }
    if (ctor_name == "Map" || ctor_name == "Set" || ctor_name == "WeakMap" ||
        ctor_name == "WeakSet" || ctor_name == "Promise" || ctor_name == "RegExp" ||
        ctor_name == "Error" || ctor_name.find("Function") != std::string::npos) {
      JS_FreeValue(ctx, tag);
      return DataCloneError(ctx, "value is not cloneable");
    }
    JS_FreeValue(ctx, tag);
  } else {
    JS_FreeValue(ctx, tag);
  }

  auto* object = JS_VALUE_GET_PTR(value);
  const auto as_key = static_cast<JSObject*>(object);
  if (const auto it = cx.seen.find(as_key); it != cx.seen.end()) {
    return JS_DupValue(ctx, it->second);
  }
  const bool is_array = JS_IsArray(value);
  JSValue cloned = is_array ? JS_NewArray(ctx) : JS_NewObjectProto(ctx, JS_UNDEFINED);
  if (JS_IsException(cloned)) {
    return cloned;
  }
  cx.seen.emplace(as_key, JS_DupValue(ctx, cloned));

  JSPropertyEnum* props = nullptr;
  std::uint32_t count = 0;
  if (JS_GetOwnPropertyNames(ctx, &props, &count, value, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) !=
      0) {
    JS_FreeValue(ctx, cloned);
    return JS_ThrowInternalError(ctx, "structuredClone: could not enumerate properties");
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    JSValue property = JS_GetProperty(ctx, value, props[i].atom);
    if (JS_IsException(property)) {
      JS_FreePropertyEnum(ctx, props, count);
      return property;
    }
    JSValue cloned_property = CloneValue(cx, property);
    JS_FreeValue(ctx, property);
    if (JS_IsException(cloned_property)) {
      JS_FreePropertyEnum(ctx, props, count);
      return cloned_property;
    }
    if (is_array) {
      JS_SetProperty(ctx, cloned, props[i].atom, cloned_property);
    } else {
      JS_DefinePropertyValue(ctx, cloned, props[i].atom, cloned_property, JS_PROP_C_W_E);
    }
  }
  JS_FreePropertyEnum(ctx, props, count);
  return cloned;
}

JSValue CloneValue(CloneContext& cx, JSValueConst value)
{
  JSContext* ctx = cx.ctx;
  if (JS_IsUndefined(value) || JS_IsNull(value) || JS_IsBool(value) || JS_IsNumber(value) ||
      JS_IsString(value) || JS_IsSymbol(value)) {
    return JS_DupValue(ctx, value);
  }
  if (!JS_IsObject(value)) {
    return DataCloneError(ctx, "value is not cloneable");
  }
  // Buffers and views (checked before plain objects) — BigInt values are
  // objects in QuickJS and are handled here as immutable.
  if (JS_IsBigInt(value)) {
    return JS_DupValue(ctx, value);
  }
  if (std::size_t size = 0; JS_GetArrayBuffer(ctx, &size, value) != nullptr) {
    return CloneArrayBufferLike(cx, value);
  }
  JSValue prototype_probe = JS_GetTypedArrayBuffer(ctx, value, nullptr, nullptr, nullptr);
  if (!JS_IsException(prototype_probe)) {
    JS_FreeValue(ctx, prototype_probe);
    return CloneArrayBufferLike(cx, value);
  }
  JS_FreeValue(ctx, JS_GetException(ctx));
  return CloneObject(cx, value);
}

JSValue StructuredClone(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "structuredClone requires a value");
  }
  CloneContext cx;
  cx.ctx = ctx;
  JSValue cloned = CloneValue(cx, argv[0]);
  for (auto& [key, clone] : cx.seen) {
    (void)key;
    JS_FreeValue(ctx, clone);
  }
  return cloned;
}

// ---------------------------------------------------------------------------
// reportError
// ---------------------------------------------------------------------------

JSValue ReportError(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "reportError requires a value");
  }
  // Browsers report through the console and the window's error handler; this
  // engine logs through the engine's own console sink (the same path
  // console.error uses) and fires the document's "error" event so
  // window.onerror/`error` listeners run.
  JSValue message = JS_ToString(ctx, argv[0]);
  if (JS_IsException(message)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    message = JS_NewString(ctx, "reportError");
  }
  LogErrorToConsole(ctx, message);
  JS_FreeValue(ctx, message);
  if (Impl* impl = ImplFor(ctx, JS_UNDEFINED); impl != nullptr) {
    // window.onerror / "error" listeners see the report too (no ErrorEvent
    // payload yet — documented).
    impl->DispatchDocumentEvent("error");
  }
  return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// CSS (escape / supports)
// ---------------------------------------------------------------------------

JSValue CssEscape(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  std::string input;
  if (argc < 1 || !ToString(ctx, argv[0], &input)) {
    return JS_EXCEPTION;
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(input.size());
  for (std::size_t i = 0; i < input.size(); ++i) {
    const auto code = static_cast<unsigned char>(input[i]);
    const bool is_control = code < 0x20 || code == 0x7f;
    const bool is_digit = code >= '0' && code <= '9';
    // A leading digit, a leading '-', and the second character after a leading
    // '-' are escaped (CSSOM "serialize an identifier").
    const bool leading_digit = i == 0 && is_digit;
    const bool leading_dash = i == 0 && input[i] == '-' && input.size() == 1;
    const bool dash_then_digit = i == 1 && is_digit && !input.empty() && input[0] == '-';
    if (code == 0) {
      out += "\ufffd";
      continue;
    }
    if (is_control || leading_digit || leading_dash || dash_then_digit) {
      out.push_back('\\');
      out.push_back(kHex[(code >> 4) & 0xf]);
      out.push_back(kHex[code & 0xf]);
      out.push_back(' ');
      continue;
    }
    if (i == 0 && input[i] == '-' && input.size() > 1) {
      out.push_back('-');
      continue;
    }
    if (code >= 0x80 || input[i] == '-' || input[i] == '_' || is_digit ||
        (input[i] >= 'a' && input[i] <= 'z') || (input[i] >= 'A' && input[i] <= 'Z')) {
      out.push_back(input[i]);
      continue;
    }
    out.push_back('\\');
    out.push_back(input[i]);
  }
  return NewString(ctx, out);
}

JSValue CssSupports(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  if (argc < 2) {
    // The single-string form CSS.supports("display: flex") is not implemented;
    // report false instead of guessing.
    return JS_NewBool(ctx, false);
  }
  std::string property;
  std::string value;
  if (!ToString(ctx, argv[0], &property) || !ToString(ctx, argv[1], &value)) {
    return JS_EXCEPTION;
  }
  // The engine answers with its own CSS parser: the declaration is wrapped in a
  // rule and "supported" means a declaration with that exact property (and a
  // non-empty value) comes back out.
  const css::StyleSheet sheet = css::ParseStyleSheet("x{" + property + ":" + value + "}");
  for (const css::StyleRule& rule : sheet.rules) {
    for (const css::Declaration& declaration : rule.declarations) {
      if (declaration.property == property && !declaration.value.empty()) {
        return JS_NewBool(ctx, true);
      }
    }
  }
  return JS_NewBool(ctx, false);
}

} // namespace

void DefinePlatformGlobals(JSContext* ctx)
{
  JSRuntime* rt = JS_GetRuntime(ctx);
  {
    std::lock_guard<std::mutex> lock(g_platform_class_mutex);
    if (g_text_classes_registered.count(rt) == 0) {
      JS_NewClassID(rt, &g_text_encoder_class_id);
      JSClassDef encoder_def = {};
      encoder_def.class_name = "TextEncoder";
      encoder_def.finalizer = TextEncoderFinalizer;
      JS_NewClass(rt, g_text_encoder_class_id, &encoder_def);

      JS_NewClassID(rt, &g_text_decoder_class_id);
      JSClassDef decoder_def = {};
      decoder_def.class_name = "TextDecoder";
      decoder_def.finalizer = TextDecoderFinalizer;
      JS_NewClass(rt, g_text_decoder_class_id, &decoder_def);
      g_text_classes_registered.insert(rt);
    }
  }

  JSValue global = JS_GetGlobalObject(ctx);

  JSValue encoder_proto = JS_NewObject(ctx);
  JS_SetPropertyStr(
      ctx, encoder_proto, "encode", JS_NewCFunction(ctx, TextEncoderEncode, "encode", 1));
  JS_SetPropertyStr(ctx,
                    encoder_proto,
                    "encodeInto",
                    JS_NewCFunction(ctx, TextEncoderEncodeInto, "encodeInto", 2));
  DefineGetter(ctx, encoder_proto, "encoding", MakeGetter(ctx, "encoding", &EncodingGetterImpl));
  JS_SetClassProto(ctx, g_text_encoder_class_id, encoder_proto);
  JSValue encoder_ctor =
      JS_NewCFunction2(ctx, TextEncoderConstructor, "TextEncoder", 0, JS_CFUNC_constructor, 0);
  JS_SetPropertyStr(ctx, encoder_ctor, "prototype", JS_DupValue(ctx, encoder_proto));
  JS_SetPropertyStr(ctx, global, "TextEncoder", encoder_ctor);

  JSValue decoder_proto = JS_NewObject(ctx);
  JS_SetPropertyStr(
      ctx, decoder_proto, "decode", JS_NewCFunction(ctx, TextDecoderDecode, "decode", 1));
  DefineGetter(ctx, decoder_proto, "encoding", MakeGetter(ctx, "encoding", &EncodingGetterImpl));
  JS_SetClassProto(ctx, g_text_decoder_class_id, decoder_proto);
  JSValue decoder_ctor =
      JS_NewCFunction2(ctx, TextDecoderConstructor, "TextDecoder", 0, JS_CFUNC_constructor, 0);
  JS_SetPropertyStr(ctx, decoder_ctor, "prototype", JS_DupValue(ctx, decoder_proto));
  JS_SetPropertyStr(ctx, global, "TextDecoder", decoder_ctor);

  JS_SetPropertyStr(ctx, global, "atob", JS_NewCFunction(ctx, Atob, "atob", 1));
  JS_SetPropertyStr(ctx, global, "btoa", JS_NewCFunction(ctx, Btoa, "btoa", 1));
  JS_SetPropertyStr(
      ctx, global, "queueMicrotask", JS_NewCFunction(ctx, QueueMicrotask, "queueMicrotask", 1));
  JS_SetPropertyStr(
      ctx, global, "structuredClone", JS_NewCFunction(ctx, StructuredClone, "structuredClone", 1));
  JS_SetPropertyStr(
      ctx, global, "reportError", JS_NewCFunction(ctx, ReportError, "reportError", 1));

  JSValue css = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, css, "escape", JS_NewCFunction(ctx, CssEscape, "escape", 1));
  JS_SetPropertyStr(ctx, css, "supports", JS_NewCFunction(ctx, CssSupports, "supports", 2));
  JS_SetPropertyStr(ctx, global, "CSS", css);

  JS_FreeValue(ctx, global);
}

void ForgetPlatformRuntime(JSRuntime* rt)
{
  std::lock_guard<std::mutex> lock(g_platform_class_mutex);
  g_text_classes_registered.erase(rt);
}

} // namespace neko::javascript
