// neko::javascript DOM bindings — storage and network API family.
//
// Part of the dom_binding split (see binding/binding_internal.h):
// window.localStorage, window.fetch (+ Response/Headers objects), Blob,
// URL.createObjectURL/revokeObjectURL, and the window.indexedDB subset
// (versioned databases, object stores, transactions).

#include "binding_internal.h"

#include <array>
#include <atomic>
#include <cstring>
#include <quickjs.h>
#include <string>

namespace neko::javascript {

// ---------------------------------------------------------------------------
// Page Web APIs: window.localStorage + window.fetch (Phase 8 M3 subset).
//
// localStorage is a synchronous per-origin key-value store wired from the
// browser layer through PageApis callbacks.  fetch(url) returns a Promise
// resolved with a minimal Response object (status/ok/statusText/url/headers
// with get(), text() and json()); network errors reject the promise.  The
// synchronous network call resolves the promise immediately, and the
// microtask pump makes `await fetch(...)` continuations progress.
// ---------------------------------------------------------------------------

// Returns a promise resolved with |value| (|value|'s reference is consumed).
JSValue ResolvePromise(JSContext* ctx, JSValue value)
{
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue ctor = JS_GetPropertyStr(ctx, global, "Promise");
  JSValue resolve_fn = JS_GetPropertyStr(ctx, ctor, "resolve");
  JSValue argv[] = {value};
  // Promise.resolve must be called with the Promise constructor as |this|.
  JSValue result = JS_Call(ctx, resolve_fn, ctor, 1, argv);
  JS_FreeValue(ctx, argv[0]);
  JS_FreeValue(ctx, resolve_fn);
  JS_FreeValue(ctx, ctor);
  JS_FreeValue(ctx, global);
  return result;
}

// Returns a promise rejected with an Error carrying |message|.
JSValue RejectPromise(JSContext* ctx, const std::string& message)
{
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue ctor = JS_GetPropertyStr(ctx, global, "Promise");
  JSValue reject_fn = JS_GetPropertyStr(ctx, ctor, "reject");
  JSValue err = JS_NewError(ctx);
  JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, message.c_str()));
  JSValue argv[] = {err};
  // Promise.reject must be called with the Promise constructor as |this|.
  JSValue result = JS_Call(ctx, reject_fn, ctor, 1, argv);
  JS_FreeValue(ctx, err);
  JS_FreeValue(ctx, reject_fn);
  JS_FreeValue(ctx, ctor);
  JS_FreeValue(ctx, global);
  return result;
}

// ---- localStorage ----------------------------------------------------------

JSValue LocalStorageGetItem(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.storage_get) {
    return JS_ThrowTypeError(ctx, "localStorage is not available");
  }
  bool ok = false;
  const std::string key = ArgString(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  const std::optional<std::string> value = impl->apis.storage_get(key);
  return value.has_value() ? JS_NewString(ctx, value->c_str()) : JS_NULL;
}

JSValue LocalStorageSetItem(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.storage_set) {
    return JS_ThrowTypeError(ctx, "localStorage is not available");
  }
  bool ok = false;
  const std::string key = ArgString(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  const std::string value = ArgString(ctx, argc >= 2 ? argv[1] : JS_UNDEFINED, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  impl->apis.storage_set(key, value);
  return JS_UNDEFINED;
}

JSValue LocalStorageRemoveItem(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.storage_remove) {
    return JS_ThrowTypeError(ctx, "localStorage is not available");
  }
  bool ok = false;
  const std::string key = ArgString(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  impl->apis.storage_remove(key);
  return JS_UNDEFINED;
}

JSValue
LocalStorageClear(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.storage_clear) {
    return JS_ThrowTypeError(ctx, "localStorage is not available");
  }
  impl->apis.storage_clear();
  return JS_UNDEFINED;
}

JSValue LocalStorageKey(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.storage_keys) {
    return JS_ThrowTypeError(ctx, "localStorage is not available");
  }
  int64_t index = 0;
  if (argc >= 1 && JS_ToInt64(ctx, &index, argv[0]) != 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_NULL;
  }
  const std::vector<std::string> keys = impl->apis.storage_keys();
  if (index < 0 || static_cast<std::size_t>(index) >= keys.size()) {
    return JS_NULL;
  }
  return JS_NewString(ctx, keys[static_cast<std::size_t>(index)].c_str());
}

JSValue LocalStorageLength(JSContext* ctx, JSValueConst this_val)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.storage_keys) {
    return JS_NewInt32(ctx, 0);
  }
  return JS_NewInt32(ctx, static_cast<int32_t>(impl->apis.storage_keys().size()));
}

JSValue BlobText(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  JSValue data = JS_GetPropertyStr(ctx, this_val, "_nekoData");
  JSValue promise = ResolvePromise(ctx, data);
  JS_FreeValue(ctx, data);
  return promise;
}

JSValue BlobArrayBuffer(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
  JSValue data = JS_GetPropertyStr(ctx, this_val, "_nekoData");
  const char* text = JS_ToCString(ctx, data);
  if (text == nullptr) {
    JS_FreeValue(ctx, data);
    return JS_EXCEPTION;
  }
  const std::size_t size = std::strlen(text);
  JSValue buffer = JS_NewArrayBufferCopy(ctx, reinterpret_cast<const uint8_t*>(text), size);
  JS_FreeCString(ctx, text);
  JS_FreeValue(ctx, data);
  return ResolvePromise(ctx, buffer);
}

JSValue BlobConstructor(JSContext* ctx, JSValueConst /*new_target*/, int argc, JSValueConst* argv)
{
  JSValue blob = JS_NewObject(ctx);
  std::string data;
  if (argc >= 1 && JS_IsArray(argv[0])) {
    JSValue length = JS_GetPropertyStr(ctx, argv[0], "length");
    int32_t count = 0;
    (void)JS_ToInt32(ctx, &count, length);
    JS_FreeValue(ctx, length);
    for (int32_t i = 0; i < count; ++i) {
      JSValue part = JS_GetPropertyUint32(ctx, argv[0], static_cast<uint32_t>(i));
      const char* text = JS_ToCString(ctx, part);
      if (text != nullptr) {
        data += text;
        JS_FreeCString(ctx, text);
      } else {
        JS_FreeValue(ctx, JS_GetException(ctx));
      }
      JS_FreeValue(ctx, part);
    }
  } else if (argc >= 1) {
    const char* text = JS_ToCString(ctx, argv[0]);
    if (text != nullptr) {
      data = text;
      JS_FreeCString(ctx, text);
    } else {
      JS_FreeValue(ctx, JS_GetException(ctx));
    }
  }
  JS_SetPropertyStr(ctx, blob, "_nekoData", JS_NewStringLen(ctx, data.data(), data.size()));
  JS_SetPropertyStr(ctx, blob, "size", JS_NewInt64(ctx, static_cast<int64_t>(data.size())));
  std::string type;
  if (argc >= 2 && JS_IsObject(argv[1])) {
    JSValue type_value = JS_GetPropertyStr(ctx, argv[1], "type");
    const char* text = JS_ToCString(ctx, type_value);
    if (text != nullptr) {
      type = text;
      JS_FreeCString(ctx, text);
    }
    JS_FreeValue(ctx, type_value);
  }
  JS_SetPropertyStr(ctx, blob, "type", JS_NewString(ctx, type.c_str()));
  JS_SetPropertyStr(ctx, blob, "text", JS_NewCFunction(ctx, BlobText, "text", 0));
  JS_SetPropertyStr(
      ctx, blob, "arrayBuffer", JS_NewCFunction(ctx, BlobArrayBuffer, "arrayBuffer", 0));
  return blob;
}

JSValue UrlCreateObjectUrl(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
  if (argc < 1 || !JS_IsObject(argv[0])) {
    return JS_ThrowTypeError(ctx, "createObjectURL requires an object");
  }
  static std::atomic<uint64_t> next_id{1};
  const std::string value = "blob:neko/" + std::to_string(next_id.fetch_add(1));
  return JS_NewString(ctx, value.c_str());
}

JSValue UrlRevokeObjectUrl(JSContext* /*ctx*/,
                           JSValueConst /*this_val*/,
                           int /*argc*/,
                           JSValueConst* /*argv*/)
{
  return JS_UNDEFINED;
}

// ---- fetch -----------------------------------------------------------------

// Response.text(): a promise resolved with the body (func_data[0]).
JSValue FetchResponseText(JSContext* ctx,
                          JSValueConst /*this_val*/,
                          int /*argc*/,
                          JSValueConst* /*argv*/,
                          int /*magic*/,
                          JSValueConst* func_data)
{
  return ResolvePromise(ctx, JS_DupValue(ctx, func_data[0]));
}

// Response.json(): parses the body (func_data[0]) as JSON; rejects on error.
JSValue FetchResponseJson(JSContext* ctx,
                          JSValueConst /*this_val*/,
                          int /*argc*/,
                          JSValueConst* /*argv*/,
                          int /*magic*/,
                          JSValueConst* func_data)
{
  const char* s = JS_ToCString(ctx, func_data[0]);
  if (s == nullptr) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return RejectPromise(ctx, "Failed to parse JSON response");
  }
  JSValue parsed = JS_ParseJSON(ctx, s, std::strlen(s), "<fetch>");
  JS_FreeCString(ctx, s);
  if (JS_IsException(parsed)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return RejectPromise(ctx, "Failed to parse JSON response");
  }
  return ResolvePromise(ctx, parsed);
}

// Headers.get(name): case-insensitive lookup over func_data[0], an object of
// lowercased header names.
JSValue FetchHeadersGet(JSContext* ctx,
                        JSValueConst /*this_val*/,
                        int argc,
                        JSValueConst* argv,
                        int /*magic*/,
                        JSValueConst* func_data)
{
  bool ok = false;
  const std::string name = ArgString(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  std::string lower;
  for (const char c : name) {
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  JSValue value = JS_GetPropertyStr(ctx, func_data[0], lower.c_str());
  if (JS_IsUndefined(value)) {
    JS_FreeValue(ctx, value);
    return JS_NULL;
  }
  return value;
}

JSValue JsFetch(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  if (!impl->apis.fetch) {
    return JS_ThrowTypeError(ctx, "fetch is not available");
  }
  bool ok = false;
  const std::string raw_url = ArgString(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  std::string url = impl->apis.resolve_url ? impl->apis.resolve_url(raw_url) : raw_url;
  if (url.empty()) {
    return RejectPromise(ctx, "Failed to parse URL: " + raw_url);
  }
  const base::Result<FetchResponse> response = impl->apis.fetch(url);
  if (!response.has_value()) {
    return RejectPromise(ctx, "Network error: " + response.error().message());
  }
  const FetchResponse& r = response.value();

  JSValue resp = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, resp, "status", JS_NewInt32(ctx, r.status));
  JS_SetPropertyStr(ctx, resp, "ok", JS_NewBool(ctx, r.status >= 200 && r.status < 300));
  JS_SetPropertyStr(ctx, resp, "statusText", JS_NewString(ctx, r.status_text.c_str()));
  JS_SetPropertyStr(
      ctx, resp, "url", JS_NewString(ctx, r.final_url.empty() ? url.c_str() : r.final_url.c_str()));

  // headers: { get(name) } backed by an object of lowercased names.
  JSValue headers_map = JS_NewObject(ctx);
  for (const auto& header : r.headers) {
    std::string lower;
    for (const char c : header.first) {
      lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    JS_SetPropertyStr(ctx, headers_map, lower.c_str(), JS_NewString(ctx, header.second.c_str()));
  }
  JSValue headers = JS_NewObject(ctx);
  JSValue headers_get = JS_NewCFunctionData(ctx, FetchHeadersGet, 1, 0, 1, &headers_map);
  JS_SetPropertyStr(ctx, headers, "get", headers_get); // steals headers_get
  JS_SetPropertyStr(ctx, resp, "headers", headers);    // steals headers
  JS_FreeValue(ctx, headers_map);                      // the function dup'd it

  // text()/json(): closures over the body.
  JSValue body = JS_NewString(ctx, r.body.c_str());
  JSValue text_fn = JS_NewCFunctionData(ctx, FetchResponseText, 0, 0, 1, &body);
  JS_SetPropertyStr(ctx, resp, "text", text_fn); // steals text_fn
  JSValue json_fn = JS_NewCFunctionData(ctx, FetchResponseJson, 0, 0, 1, &body);
  JS_SetPropertyStr(ctx, resp, "json", json_fn); // steals json_fn
  JS_FreeValue(ctx, body);                       // the functions dup'd it

  return ResolvePromise(ctx, resp);
}

// ---------------------------------------------------------------------------
// window.indexedDB — a practical subset of the IndexedDB API.  Databases
// with versionchange upgrades, object stores with key paths and
// auto-increment keys, and transactions carrying add/put/get/delete/clear/
// count/getAll.  Results are delivered through microtasks (QuickJS jobs)
// with onsuccess/onerror handler properties; the storage layer itself is
// updated synchronously when each request is issued.
//
// Documented deviations/limitations:
//   * no cursors, indexes, or key ranges;
//   * keys are numbers or strings only;
//   * values use the JSON clone subset (no Date/BinaryData; cycles fail the
//     request with DataCloneError);
//   * transactions auto-commit when their last request completes;
//   * errors are Error objects with a DOMException-like .name.
// ---------------------------------------------------------------------------

// Splits a storage-layer error of the form "IDB:<Name>:<message>".
void SplitIdbError(const base::Error& error, std::string& name, std::string& message)
{
  name = "UnknownError";
  message = error.message();
  if (message.rfind("IDB:", 0) == 0) {
    const std::size_t colon = message.find(':', 4);
    if (colon != std::string::npos) {
      name = message.substr(4, colon - 4);
      message = message.substr(colon + 1);
    }
  }
}

// An Error object carrying a DOMException-like .name.
JSValue MakeIdbErrorValue(JSContext* ctx, const std::string& name, const std::string& message)
{
  JSValue err = JS_NewError(ctx);
  JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, message.c_str()));
  JS_SetPropertyStr(ctx, err, "name", JS_NewString(ctx, name.c_str()));
  return err;
}

JSValue MakeIdbErrorFromBase(JSContext* ctx, const base::Error& error)
{
  std::string name, message;
  SplitIdbError(error, name, message);
  return MakeIdbErrorValue(ctx, name, message);
}

JSValue ThrowIdbError(JSContext* ctx, const std::string& name, const std::string& message)
{
  return JS_Throw(ctx, MakeIdbErrorValue(ctx, name, message));
}

// Invokes |target|'s |prop| handler (a function) with |event|, if set.
// Handler exceptions are consumed (mirroring the DOM event dispatch paths).
void CallIdbHandler(JSContext* ctx, JSValueConst target, const char* prop, JSValueConst event)
{
  JSValue handler = JS_GetPropertyStr(ctx, target, prop);
  if (!JS_IsFunction(ctx, handler)) {
    JS_FreeValue(ctx, handler);
    return;
  }
  JSValueConst argv[] = {event};
  JSValue ret = JS_Call(ctx, handler, target, 1, argv);
  if (JS_IsException(ret)) {
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, JS_GetException(ctx));
  } else {
    JS_FreeValue(ctx, ret);
  }
  JS_FreeValue(ctx, handler);
}

// Returns the IdbHandle addressed by |func_data[0]| (an int32 index).
std::shared_ptr<Impl::IdbHandle> IdbHandleFromData(JSContext* ctx, JSValueConst* func_data)
{
  Impl* impl = ImplFor(ctx, JS_UNDEFINED);
  if (impl == nullptr) {
    return nullptr;
  }
  int32_t idx = 0;
  if (JS_ToInt32(ctx, &idx, func_data[0]) != 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return nullptr;
  }
  if (idx < 0 || static_cast<std::size_t>(idx) >= impl->idb_handles.size()) {
    return nullptr;
  }
  return impl->idb_handles[static_cast<std::size_t>(idx)];
}

// JSON-serializes |value| for the structured-clone subset.  Returns nullopt
// when the value cannot be cloned (cycles etc.); the exception is consumed.
std::optional<std::string> IdbCloneToJson(JSContext* ctx, JSValueConst value)
{
  JSValue json = JS_JSONStringify(ctx, value, JS_UNDEFINED, JS_UNDEFINED);
  if (JS_IsException(json)) {
    JS_FreeValue(ctx, json);
    JS_FreeValue(ctx, JS_GetException(ctx));
    return std::nullopt;
  }
  const char* text = JS_ToCString(ctx, json);
  std::string out = text != nullptr ? text : "";
  JS_FreeCString(ctx, text);
  JS_FreeValue(ctx, json);
  return out;
}

// Serializes an explicit key argument (a JSON number or string).  Returns
// nullopt for unsupported key types.
std::optional<std::string> IdbKeyToJson(JSContext* ctx, JSValueConst key)
{
  if (!JS_IsNumber(key) && !JS_IsString(key)) {
    return std::nullopt;
  }
  return IdbCloneToJson(ctx, key);
}

// Parses JSON text back to a JS value; undefined when parsing fails.
JSValue IdbJsonToJs(JSContext* ctx, const std::string& json)
{
  JSValue parsed = JS_ParseJSON(ctx, json.c_str(), json.size(), "<indexeddb>");
  if (JS_IsException(parsed)) {
    JS_FreeValue(ctx, parsed);
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_UNDEFINED;
  }
  return parsed;
}

// Common completion of a request: sets result/error/readyState and fires the
// matching handler.  |result| and |error_value| are consumed.
void SettleIdbRequest(JSContext* ctx,
                      JSValueConst req_obj,
                      const char* handler_prop,
                      const char* event_type,
                      bool success,
                      JSValueConst result,
                      JSValueConst error_value)
{
  if (success) {
    JS_SetPropertyStr(ctx, req_obj, "result", result); // steals the dup
    JS_SetPropertyStr(ctx, req_obj, "error", JS_UNDEFINED);
  } else {
    JS_SetPropertyStr(ctx, req_obj, "error", error_value); // steals the dup
  }
  JS_SetPropertyStr(ctx, req_obj, "readyState", JS_NewString(ctx, "done"));
  JSValue event = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, event, "type", JS_NewString(ctx, event_type)); // steals
  JS_SetPropertyStr(ctx, event, "target", JS_DupValue(ctx, req_obj));
  CallIdbHandler(ctx, req_obj, handler_prop, event);
  JS_FreeValue(ctx, event);
}

// Completes a data request (add/put/get/delete/clear/count/getAll) and
// commits its transaction when it was the last outstanding request.
void SettleIdbDataRequest(Impl* impl,
                          JSContext* ctx,
                          JSValueConst req_obj,
                          const std::shared_ptr<Impl::IdbRequest>& request)
{
  if (!request->error_name.empty()) {
    JSValue err = MakeIdbErrorValue(ctx, request->error_name, request->error_message);
    SettleIdbRequest(ctx, req_obj, "onerror", "error", false, JS_UNDEFINED, err);
  } else {
    JSValue result = JS_UNDEFINED;
    if (request->has_result && request->result_json.has_value()) {
      result = IdbJsonToJs(ctx, *request->result_json);
    }
    SettleIdbRequest(ctx, req_obj, "onsuccess", "success", true, result, JS_UNDEFINED);
  }
  if (request->tx_handle >= 0 &&
      static_cast<std::size_t>(request->tx_handle) < impl->idb_handles.size()) {
    const std::shared_ptr<Impl::IdbHandle>& tx =
        impl->idb_handles[static_cast<std::size_t>(request->tx_handle)];
    if (tx->kind == Impl::IdbHandle::Kind::kTransaction && tx->pending > 0) {
      --tx->pending;
      if (tx->pending == 0 && !tx->completed && !tx->aborted) {
        tx->completed = true;
        if (!JS_IsUndefined(tx->object)) {
          JSValue event = JS_NewObject(ctx);
          JS_SetPropertyStr(ctx, event, "type", JS_NewString(ctx, "complete"));
          JS_SetPropertyStr(ctx, event, "target", JS_DupValue(ctx, tx->object));
          CallIdbHandler(ctx, tx->object, "oncomplete", event);
          JS_FreeValue(ctx, event);
        }
      }
    }
  }
}

// Creates the JS object model for a database handle (name/version/
// objectStoreNames + methods).
JSValue MakeIdbDatabaseObject(Impl* impl, JSContext* ctx, int handle_idx);

JSValue MakeIdbTransactionObject(Impl* impl, JSContext* ctx, int handle_idx);

JSValue MakeIdbStoreObject(Impl* impl, JSContext* ctx, int handle_idx);

int IdbHandleIndexFromData(JSContext* ctx, JSValueConst* func_data);

// Loads the object-store metadata of |db_name| into |handle->stores|.
void IdbLoadStoreMetas(Impl* impl,
                       const std::string& db_name,
                       const std::shared_ptr<Impl::IdbHandle>& handle)
{
  handle->stores.clear();
  const base::Result<std::vector<IdbStoreMeta>> metas = impl->apis.idb_store_names(db_name);
  if (!metas) {
    return;
  }
  for (const IdbStoreMeta& meta : metas.value()) {
    Impl::IdbStoreInfo info;
    info.name = meta.name;
    info.key_path = meta.key_path;
    info.auto_increment = meta.auto_increment;
    handle->stores.push_back(std::move(info));
  }
}

// Completes an open()/deleteDatabase() request: handles version-change
// upgrades (onupgradeneeded) and plain opens.
void SettleIdbOpenRequest(Impl* impl,
                          JSContext* ctx,
                          JSValueConst req_obj,
                          const std::shared_ptr<Impl::IdbRequest>& request)
{
  if (!request->error_name.empty()) {
    JSValue err = MakeIdbErrorValue(ctx, request->error_name, request->error_message);
    SettleIdbRequest(ctx, req_obj, "onerror", "error", false, JS_UNDEFINED, err);
    return;
  }

  auto fail = [&](const base::Error& error) {
    std::string name, message;
    SplitIdbError(error, name, message);
    JSValue err = MakeIdbErrorValue(ctx, name, message);
    SettleIdbRequest(ctx, req_obj, "onerror", "error", false, JS_UNDEFINED, err);
  };

  if (request->is_delete_db) {
    const base::Result<void> removed = impl->apis.idb_delete_db(request->db_name);
    if (!removed) {
      fail(removed.error());
      return;
    }
    SettleIdbRequest(ctx, req_obj, "onsuccess", "success", true, JS_UNDEFINED, JS_UNDEFINED);
    return;
  }

  const std::string& name = request->db_name;
  const base::Result<int64_t> current = impl->apis.idb_current_version(name);
  if (!current) {
    fail(current.error());
    return;
  }
  int64_t new_version = 0;
  bool upgrade = false;
  if (request->requested_version > 0) {
    if (request->requested_version < current.value()) {
      fail(base::Error::InvalidArgument(
          "IDB:VersionError:requested version is lower than the database version"));
      return;
    }
    if (request->requested_version > current.value()) {
      upgrade = true;
      new_version = request->requested_version;
    }
  } else if (current.value() == 0) {
    // Spec: opening a missing database creates version 1 and upgrades.
    upgrade = true;
    new_version = 1;
  }

  const int handle_idx = static_cast<int>(impl->idb_handles.size());
  auto handle = std::make_shared<Impl::IdbHandle>();
  handle->kind = Impl::IdbHandle::Kind::kDatabase;
  handle->db_name = name;
  handle->version = current.value();
  impl->idb_handles.push_back(handle);
  IdbLoadStoreMetas(impl, name, handle);

  if (upgrade) {
    if (current.value() == 0) {
      const base::Result<int64_t> created = impl->apis.idb_create_db(name);
      if (!created) {
        fail(created.error());
        return;
      }
    }
    handle->upgrade = true;
    JSValue db_obj = MakeIdbDatabaseObject(impl, ctx, handle_idx);
    // The version-change transaction.
    const int tx_idx = static_cast<int>(impl->idb_handles.size());
    auto tx = std::make_shared<Impl::IdbHandle>();
    tx->kind = Impl::IdbHandle::Kind::kTransaction;
    tx->db_name = name;
    tx->mode = "versionchange";
    tx->db_handle = handle_idx;
    impl->idb_handles.push_back(tx);
    JSValue tx_obj = MakeIdbTransactionObject(impl, ctx, tx_idx);

    JS_SetPropertyStr(ctx, req_obj, "result", db_obj); // steals db_obj
    JS_SetPropertyStr(ctx, req_obj, "transaction", JS_DupValue(ctx, tx_obj));
    JSValue event = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, event, "type", JS_NewString(ctx, "upgradeneeded"));
    JS_SetPropertyStr(ctx, event, "target", JS_DupValue(ctx, req_obj));
    JS_SetPropertyStr(ctx, event, "oldVersion", JS_NewInt64(ctx, current.value()));
    JS_SetPropertyStr(ctx, event, "newVersion", JS_NewInt64(ctx, new_version));
    JS_SetPropertyStr(ctx, event, "transaction", JS_DupValue(ctx, tx_obj));
    CallIdbHandler(ctx, req_obj, "onupgradeneeded", event);
    JS_FreeValue(ctx, event);
    JS_FreeValue(ctx, tx_obj); // the tx handle still dups it

    const base::Result<void> versioned = impl->apis.idb_set_version(name, new_version);
    if (!versioned) {
      fail(versioned.error());
      return;
    }
    handle->version = new_version;
    handle->upgrade = false;
    IdbLoadStoreMetas(impl, name, handle);

    // Refresh the db object's version + store names, then complete.
    JS_SetPropertyStr(ctx, db_obj, "version", JS_NewInt64(ctx, new_version));
    JSValue names = JS_NewArray(ctx);
    for (std::size_t i = 0; i < handle->stores.size(); ++i) {
      JS_SetPropertyUint32(
          ctx, names, static_cast<uint32_t>(i), JS_NewString(ctx, handle->stores[i].name.c_str()));
    }
    JS_SetPropertyStr(ctx, db_obj, "objectStoreNames", names); // steals names
    // The version-change transaction completes, then the request succeeds.
    JSValue complete_event = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, complete_event, "type", JS_NewString(ctx, "complete"));
    JS_SetPropertyStr(ctx, complete_event, "target", JS_DupValue(ctx, tx->object));
    CallIdbHandler(ctx, tx->object, "oncomplete", complete_event);
    JS_FreeValue(ctx, complete_event);
    SettleIdbRequest(
        ctx, req_obj, "onsuccess", "success", true, JS_DupValue(ctx, db_obj), JS_UNDEFINED);
  } else {
    JSValue db_obj = MakeIdbDatabaseObject(impl, ctx, handle_idx);
    JS_SetPropertyStr(ctx, req_obj, "transaction", JS_UNDEFINED);
    SettleIdbRequest(ctx, req_obj, "onsuccess", "success", true, db_obj, JS_UNDEFINED);
  }
}

// The QuickJS job entry for request completions.  argv[0] = request object,
// argv[1] = request index (int32).
JSValue IdbJobEntry(JSContext* ctx, int argc, JSValueConst* argv)
{
  if (argc < 2) {
    return JS_UNDEFINED;
  }
  Impl* impl = ImplFor(ctx, JS_UNDEFINED);
  if (impl == nullptr) {
    return JS_UNDEFINED;
  }
  int32_t idx = 0;
  if (JS_ToInt32(ctx, &idx, argv[1]) != 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_UNDEFINED;
  }
  if (idx < 0 || static_cast<std::size_t>(idx) >= impl->idb_requests.size()) {
    return JS_UNDEFINED;
  }
  const std::shared_ptr<Impl::IdbRequest> request =
      impl->idb_requests[static_cast<std::size_t>(idx)];
  if (request->is_open || request->is_delete_db) {
    SettleIdbOpenRequest(impl, ctx, argv[0], request);
  } else {
    SettleIdbDataRequest(impl, ctx, argv[0], request);
  }
  return JS_UNDEFINED;
}

// Allocates a request state + JS request object and enqueues its completion
// job.  The returned JSValue is owned by the caller (the API returns it to
// the script); the job queue holds its own dup.
JSValue MakeIdbRequest(Impl* impl,
                       JSContext* ctx,
                       std::shared_ptr<Impl::IdbRequest> request,
                       bool upgradeneeded_slot)
{
  JSValue req_obj = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, req_obj, "result", JS_UNDEFINED);
  JS_SetPropertyStr(ctx, req_obj, "error", JS_UNDEFINED);
  JS_SetPropertyStr(ctx, req_obj, "readyState", JS_NewString(ctx, "pending"));
  JS_SetPropertyStr(ctx, req_obj, "onsuccess", JS_UNDEFINED);
  JS_SetPropertyStr(ctx, req_obj, "onerror", JS_UNDEFINED);
  if (upgradeneeded_slot) {
    JS_SetPropertyStr(ctx, req_obj, "onupgradeneeded", JS_UNDEFINED);
    JS_SetPropertyStr(ctx, req_obj, "transaction", JS_UNDEFINED);
  }
  const int32_t idx = static_cast<int32_t>(impl->idb_requests.size());
  impl->idb_requests.push_back(std::move(request));
  JSValue args[2];
  args[0] = JS_DupValue(ctx, req_obj);
  args[1] = JS_NewInt32(ctx, idx);
  JS_EnqueueJob(ctx, IdbJobEntry, 2, args);
  JS_FreeValue(ctx, args[0]);
  JS_FreeValue(ctx, args[1]);
  return req_obj;
}

// ---- method dispatchers ----------------------------------------------------

JSValue IdbOpen(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.idb_current_version) {
    return JS_ThrowTypeError(ctx, "indexedDB is not available");
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "indexedDB.open requires a database name");
  }
  bool ok = false;
  const std::string name = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  int64_t version = 0;
  if (argc >= 2 && !JS_IsUndefined(argv[1])) {
    if (JS_ToInt64(ctx, &version, argv[1]) != 0) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      return JS_ThrowTypeError(ctx, "indexedDB.open version must be an integer");
    }
    if (version < 0) {
      return JS_ThrowTypeError(ctx, "indexedDB.open version must not be negative");
    }
  }
  auto request = std::make_shared<Impl::IdbRequest>();
  request->is_open = true;
  request->db_name = name;
  request->requested_version = version;
  return MakeIdbRequest(impl, ctx, std::move(request), /*upgradeneeded_slot=*/true);
}

JSValue IdbDeleteDatabase(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr || !impl->apis.idb_delete_db) {
    return JS_ThrowTypeError(ctx, "indexedDB is not available");
  }
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "indexedDB.deleteDatabase requires a database name");
  }
  bool ok = false;
  const std::string name = ArgString(ctx, argv[0], &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  auto request = std::make_shared<Impl::IdbRequest>();
  request->is_delete_db = true;
  request->db_name = name;
  return MakeIdbRequest(impl, ctx, std::move(request), /*upgradeneeded_slot=*/false);
}

JSValue IdbDatabaseCreateObjectStore(JSContext* ctx,
                                     JSValueConst /*this_val*/,
                                     int argc,
                                     JSValueConst* argv,
                                     int /*magic*/,
                                     JSValueConst* func_data)
{
  Impl* impl = ImplFor(ctx, JS_UNDEFINED);
  const std::shared_ptr<Impl::IdbHandle> handle = IdbHandleFromData(ctx, func_data);
  if (impl == nullptr || handle == nullptr) {
    return JS_ThrowTypeError(ctx, "stale indexedDB object");
  }
  if (!handle->upgrade) {
    return ThrowIdbError(
        ctx, "InvalidStateError", "createObjectStore is only allowed during a version change");
  }
  bool ok = false;
  const std::string name = ArgString(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  for (const Impl::IdbStoreInfo& info : handle->stores) {
    if (info.name == name) {
      return ThrowIdbError(ctx, "ConstraintError", "an object store with this name already exists");
    }
  }
  std::string key_path;
  bool auto_increment = false;
  if (argc >= 2 && JS_IsObject(argv[1])) {
    JSValue kp = JS_GetPropertyStr(ctx, argv[1], "keyPath");
    if (JS_IsString(kp)) {
      key_path = ArgString(ctx, kp, &ok);
      if (!ok) {
        JS_FreeValue(ctx, kp);
        return JS_EXCEPTION;
      }
    }
    JS_FreeValue(ctx, kp);
    JSValue ai = JS_GetPropertyStr(ctx, argv[1], "autoIncrement");
    if (JS_IsBool(ai)) {
      auto_increment = JS_ToBool(ctx, ai) != 0;
    }
    JS_FreeValue(ctx, ai);
  }
  const base::Result<void> created =
      impl->apis.idb_create_store(handle->db_name, name, key_path, auto_increment);
  if (!created) {
    return JS_Throw(ctx, MakeIdbErrorFromBase(ctx, created.error()));
  }
  Impl::IdbStoreInfo info;
  info.name = name;
  info.key_path = key_path;
  info.auto_increment = auto_increment;
  handle->stores.push_back(std::move(info));
  // Return a store object carrying the metadata (looked up through the
  // owning database handle).
  const int db_idx = IdbHandleIndexFromData(ctx, func_data);
  const int store_idx = static_cast<int>(impl->idb_handles.size());
  auto store_handle = std::make_shared<Impl::IdbHandle>();
  store_handle->kind = Impl::IdbHandle::Kind::kObjectStore;
  store_handle->db_name = handle->db_name;
  store_handle->store_name = name;
  store_handle->db_handle = db_idx;
  impl->idb_handles.push_back(store_handle);
  return MakeIdbStoreObject(impl, ctx, store_idx);
}

JSValue IdbDatabaseDeleteObjectStore(JSContext* ctx,
                                     JSValueConst /*this_val*/,
                                     int argc,
                                     JSValueConst* argv,
                                     int /*magic*/,
                                     JSValueConst* func_data)
{
  Impl* impl = ImplFor(ctx, JS_UNDEFINED);
  const std::shared_ptr<Impl::IdbHandle> handle = IdbHandleFromData(ctx, func_data);
  if (impl == nullptr || handle == nullptr) {
    return JS_ThrowTypeError(ctx, "stale indexedDB object");
  }
  if (!handle->upgrade) {
    return ThrowIdbError(
        ctx, "InvalidStateError", "deleteObjectStore is only allowed during a version change");
  }
  bool ok = false;
  const std::string name = ArgString(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  bool found = false;
  for (auto it = handle->stores.begin(); it != handle->stores.end(); ++it) {
    if (it->name == name) {
      handle->stores.erase(it);
      found = true;
      break;
    }
  }
  if (!found) {
    return ThrowIdbError(ctx, "NotFoundError", "no object store with this name");
  }
  const base::Result<void> removed = impl->apis.idb_delete_store(handle->db_name, name);
  if (!removed) {
    return JS_Throw(ctx, MakeIdbErrorFromBase(ctx, removed.error()));
  }
  return JS_UNDEFINED;
}

// Shared logic for object-store data operations.
JSValue IdbStoreOp(
    JSContext* ctx, int argc, JSValueConst* argv, JSValueConst* func_data, const std::string& op)
{
  Impl* impl = ImplFor(ctx, JS_UNDEFINED);
  const std::shared_ptr<Impl::IdbHandle> store = IdbHandleFromData(ctx, func_data);
  if (impl == nullptr || store == nullptr) {
    return JS_ThrowTypeError(ctx, "stale indexedDB object");
  }
  if (store->tx_handle < 0 ||
      static_cast<std::size_t>(store->tx_handle) >= impl->idb_handles.size()) {
    return ThrowIdbError(ctx, "InvalidStateError", "transaction is no longer active");
  }
  const std::shared_ptr<Impl::IdbHandle>& tx =
      impl->idb_handles[static_cast<std::size_t>(store->tx_handle)];
  if (tx->aborted) {
    return ThrowIdbError(ctx, "TransactionInactiveError", "transaction was aborted");
  }
  const bool is_write = op != "get" && op != "getAll" && op != "count";
  if (is_write && tx->mode == "readonly") {
    return ThrowIdbError(ctx, "ReadOnlyError", "transaction is read-only");
  }
  if ((op == "add" || op == "put" || op == "get" || op == "delete") &&
      (argc < 1 || JS_IsUndefined(argv[0]))) {
    return JS_ThrowTypeError(ctx, "missing argument");
  }

  auto request = std::make_shared<Impl::IdbRequest>();
  request->tx_handle = store->tx_handle;

  auto run = [&]() {
    // Serialize the value (add/put).
    std::string value_json;
    if (op == "add" || op == "put") {
      const std::optional<std::string> cloned = IdbCloneToJson(ctx, argv[0]);
      if (!cloned.has_value()) {
        request->error_name = "DataCloneError";
        request->error_message = "value could not be cloned";
        return;
      }
      value_json = *cloned;
    }
    // Serialize the key (add/put/get/delete).
    std::optional<std::string> key_json;
    if (op == "add" || op == "put") {
      if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        key_json = IdbKeyToJson(ctx, argv[1]);
        if (!key_json.has_value()) {
          request->error_name = "DataError";
          request->error_message = "key must be a number or a string";
          return;
        }
      }
    } else if (op == "get" || op == "delete") {
      key_json = IdbKeyToJson(ctx, argv[0]);
      if (!key_json.has_value()) {
        request->error_name = "DataError";
        request->error_message = "key must be a number or a string";
        return;
      }
    }
    // Execute against the storage layer (synchronous; results settle async).
    if (op == "add") {
      const base::Result<std::string> r =
          impl->apis.idb_add(store->db_name, store->store_name, key_json, value_json);
      if (!r) {
        SplitIdbError(r.error(), request->error_name, request->error_message);
        return;
      }
      request->result_json = r.value();
      request->has_result = true;
    } else if (op == "put") {
      const base::Result<std::string> r =
          impl->apis.idb_put(store->db_name, store->store_name, key_json, value_json);
      if (!r) {
        SplitIdbError(r.error(), request->error_name, request->error_message);
        return;
      }
      request->result_json = r.value();
      request->has_result = true;
    } else if (op == "get") {
      const base::Result<std::optional<std::string>> r =
          impl->apis.idb_get(store->db_name, store->store_name, *key_json);
      if (!r) {
        SplitIdbError(r.error(), request->error_name, request->error_message);
        return;
      }
      if (r.value().has_value()) {
        request->result_json = *r.value();
        request->has_result = true;
      }
    } else if (op == "delete") {
      const base::Result<void> r =
          impl->apis.idb_delete(store->db_name, store->store_name, *key_json);
      if (!r) {
        SplitIdbError(r.error(), request->error_name, request->error_message);
        return;
      }
    } else if (op == "clear") {
      const base::Result<void> r = impl->apis.idb_clear(store->db_name, store->store_name);
      if (!r) {
        SplitIdbError(r.error(), request->error_name, request->error_message);
        return;
      }
    } else if (op == "count") {
      const base::Result<int64_t> r = impl->apis.idb_count(store->db_name, store->store_name);
      if (!r) {
        SplitIdbError(r.error(), request->error_name, request->error_message);
        return;
      }
      request->result_json = std::to_string(r.value());
      request->has_result = true;
    } else if (op == "getAll") {
      const base::Result<std::vector<std::string>> r =
          impl->apis.idb_get_all(store->db_name, store->store_name);
      if (!r) {
        SplitIdbError(r.error(), request->error_name, request->error_message);
        return;
      }
      std::string array_json = "[";
      for (std::size_t i = 0; i < r.value().size(); ++i) {
        if (i != 0) {
          array_json.push_back(',');
        }
        array_json += r.value()[i];
      }
      array_json.push_back(']');
      request->result_json = std::move(array_json);
      request->has_result = true;
    }
  };
  run();
  ++tx->pending;
  return MakeIdbRequest(impl, ctx, std::move(request), /*upgradeneeded_slot=*/false);
}

#define NEKO_IDB_STORE_OP(fn_name, op_name, length)                                                \
  JSValue fn_name(JSContext* ctx,                                                                  \
                  JSValueConst /*this_val*/,                                                       \
                  int argc,                                                                        \
                  JSValueConst* argv,                                                              \
                  int /*magic*/,                                                                   \
                  JSValueConst* func_data)                                                         \
  {                                                                                                \
    (void)length;                                                                                  \
    return IdbStoreOp(ctx, argc, argv, func_data, op_name);                                        \
  }

NEKO_IDB_STORE_OP(IdbStoreAdd, "add", 2)
NEKO_IDB_STORE_OP(IdbStorePut, "put", 2)
NEKO_IDB_STORE_OP(IdbStoreGet, "get", 1)
NEKO_IDB_STORE_OP(IdbStoreDelete, "delete", 1)
NEKO_IDB_STORE_OP(IdbStoreClear, "clear", 0)
NEKO_IDB_STORE_OP(IdbStoreCount, "count", 0)
NEKO_IDB_STORE_OP(IdbStoreGetAll, "getAll", 0)

#undef NEKO_IDB_STORE_OP

JSValue IdbDatabaseTransaction(JSContext* ctx,
                               JSValueConst /*this_val*/,
                               int argc,
                               JSValueConst* argv,
                               int /*magic*/,
                               JSValueConst* func_data)
{
  Impl* impl = ImplFor(ctx, JS_UNDEFINED);
  const std::shared_ptr<Impl::IdbHandle> db = IdbHandleFromData(ctx, func_data);
  if (impl == nullptr || db == nullptr) {
    return JS_ThrowTypeError(ctx, "stale indexedDB object");
  }
  bool ok = false;
  std::string mode = "readonly";
  if (argc >= 2 && !JS_IsUndefined(argv[1])) {
    mode = ArgString(ctx, argv[1], &ok);
    if (!ok) {
      return JS_EXCEPTION;
    }
    if (mode != "readonly" && mode != "readwrite") {
      return JS_ThrowTypeError(ctx, "transaction mode must be 'readonly' or 'readwrite'");
    }
  }
  std::vector<std::string> names;
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "transaction requires store names");
  }
  if (JS_IsArray(argv[0])) {
    JSValue len_value = JS_GetPropertyStr(ctx, argv[0], "length");
    int64_t len = 0;
    JS_ToInt64(ctx, &len, len_value);
    JS_FreeValue(ctx, len_value);
    for (int64_t i = 0; i < len; ++i) {
      JSValue item = JS_GetPropertyUint32(ctx, argv[0], static_cast<uint32_t>(i));
      std::string name = ArgString(ctx, item, &ok);
      JS_FreeValue(ctx, item);
      if (!ok) {
        return JS_EXCEPTION;
      }
      names.push_back(name);
    }
  } else {
    std::string name = ArgString(ctx, argv[0], &ok);
    if (!ok) {
      return JS_EXCEPTION;
    }
    names.push_back(name);
  }
  for (const std::string& name : names) {
    bool found = false;
    for (const Impl::IdbStoreInfo& info : db->stores) {
      if (info.name == name) {
        found = true;
        break;
      }
    }
    if (!found) {
      return ThrowIdbError(ctx, "NotFoundError", "no object store with this name");
    }
  }
  const int db_idx = IdbHandleIndexFromData(ctx, func_data);
  const int tx_idx = static_cast<int>(impl->idb_handles.size());
  auto tx = std::make_shared<Impl::IdbHandle>();
  tx->kind = Impl::IdbHandle::Kind::kTransaction;
  tx->db_name = db->db_name;
  tx->mode = mode;
  tx->db_handle = db_idx;
  impl->idb_handles.push_back(tx);
  return MakeIdbTransactionObject(impl, ctx, tx_idx);
}

JSValue IdbTransactionObjectStore(JSContext* ctx,
                                  JSValueConst /*this_val*/,
                                  int argc,
                                  JSValueConst* argv,
                                  int /*magic*/,
                                  JSValueConst* func_data)
{
  Impl* impl = ImplFor(ctx, JS_UNDEFINED);
  const std::shared_ptr<Impl::IdbHandle> tx = IdbHandleFromData(ctx, func_data);
  if (impl == nullptr || tx == nullptr) {
    return JS_ThrowTypeError(ctx, "stale indexedDB object");
  }
  bool ok = false;
  const std::string name = ArgString(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED, &ok);
  if (!ok) {
    return JS_EXCEPTION;
  }
  if (tx->db_handle < 0 || static_cast<std::size_t>(tx->db_handle) >= impl->idb_handles.size()) {
    return ThrowIdbError(ctx, "InvalidStateError", "transaction is no longer active");
  }
  const std::shared_ptr<Impl::IdbHandle>& db =
      impl->idb_handles[static_cast<std::size_t>(tx->db_handle)];
  const Impl::IdbStoreInfo* info = nullptr;
  for (const Impl::IdbStoreInfo& candidate : db->stores) {
    if (candidate.name == name) {
      info = &candidate;
      break;
    }
  }
  if (info == nullptr) {
    return ThrowIdbError(ctx, "NotFoundError", "no object store with this name");
  }
  const int store_idx = static_cast<int>(impl->idb_handles.size());
  auto store = std::make_shared<Impl::IdbHandle>();
  store->kind = Impl::IdbHandle::Kind::kObjectStore;
  store->db_name = db->db_name;
  store->store_name = name;
  // db_handle: owning database (metadata lookups); tx_handle: the
  // transaction that requests against this store count against.
  store->db_handle = tx->db_handle;
  store->tx_handle = IdbHandleIndexFromData(ctx, func_data);
  impl->idb_handles.push_back(store);
  return MakeIdbStoreObject(impl, ctx, store_idx);
}

JSValue IdbTransactionAbort(JSContext* ctx,
                            JSValueConst /*this_val*/,
                            int /*argc*/,
                            JSValueConst* /*argv*/,
                            int /*magic*/,
                            JSValueConst* func_data)
{
  const std::shared_ptr<Impl::IdbHandle> tx = IdbHandleFromData(ctx, func_data);
  if (tx != nullptr) {
    tx->aborted = true;
    if (tx->kind == Impl::IdbHandle::Kind::kTransaction && !JS_IsUndefined(tx->object)) {
      JSValue event = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, event, "type", JS_NewString(ctx, "abort"));
      JS_SetPropertyStr(ctx, event, "target", JS_DupValue(ctx, tx->object));
      CallIdbHandler(ctx, tx->object, "onabort", event);
      JS_FreeValue(ctx, event);
    }
  }
  return JS_UNDEFINED;
}

// The transaction auto-commits when its requests settle; commit() is a
// documented no-op.
JSValue IdbTransactionCommit(JSContext* /*ctx*/,
                             JSValueConst /*this_val*/,
                             int /*argc*/,
                             JSValueConst* /*argv*/,
                             int /*magic*/,
                             JSValueConst* /*func_data*/)
{
  return JS_UNDEFINED;
}

JSValue IdbDatabaseClose(JSContext* /*ctx*/,
                         JSValueConst /*this_val*/,
                         int /*argc*/,
                         JSValueConst* /*argv*/,
                         int /*magic*/,
                         JSValueConst* /*func_data*/)
{
  return JS_UNDEFINED; // connections live for the page; close() is a no-op
}

int IdbHandleIndexFromData(JSContext* ctx, JSValueConst* func_data)
{
  int32_t idx = -1;
  if (JS_ToInt32(ctx, &idx, func_data[0]) != 0) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return -1;
  }
  return idx;
}

JSValue
BindIdbMethod(JSContext* ctx, JSValue obj, const char* name, JSCFunctionData* fn, int handle_idx)
{
  JSValue data[] = {JS_NewInt32(ctx, handle_idx)};
  JSValue bound = JS_NewCFunctionData(ctx, fn, 0, 0, 1, data);
  JS_FreeValue(ctx, data[0]);               // the function dup'd it
  JS_SetPropertyStr(ctx, obj, name, bound); // steals bound
  return obj;
}

JSValue MakeIdbDatabaseObject(Impl* impl, JSContext* ctx, int handle_idx)
{
  const std::shared_ptr<Impl::IdbHandle>& handle =
      impl->idb_handles[static_cast<std::size_t>(handle_idx)];
  JSValue obj = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, handle->db_name.c_str()));
  JS_SetPropertyStr(ctx, obj, "version", JS_NewInt64(ctx, handle->version));
  JSValue names = JS_NewArray(ctx);
  for (std::size_t i = 0; i < handle->stores.size(); ++i) {
    JS_SetPropertyUint32(
        ctx, names, static_cast<uint32_t>(i), JS_NewString(ctx, handle->stores[i].name.c_str()));
  }
  JS_SetPropertyStr(ctx, obj, "objectStoreNames", names); // steals names
  BindIdbMethod(ctx, obj, "createObjectStore", IdbDatabaseCreateObjectStore, handle_idx);
  BindIdbMethod(ctx, obj, "deleteObjectStore", IdbDatabaseDeleteObjectStore, handle_idx);
  BindIdbMethod(ctx, obj, "transaction", IdbDatabaseTransaction, handle_idx);
  BindIdbMethod(ctx, obj, "close", IdbDatabaseClose, handle_idx);
  if (!JS_IsUndefined(handle->object)) {
    JS_FreeValue(ctx, handle->object);
  }
  handle->object = JS_DupValue(ctx, obj);
  return obj;
}

JSValue MakeIdbTransactionObject(Impl* impl, JSContext* ctx, int handle_idx)
{
  const std::shared_ptr<Impl::IdbHandle>& handle =
      impl->idb_handles[static_cast<std::size_t>(handle_idx)];
  JSValue obj = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, obj, "mode", JS_NewString(ctx, handle->mode.c_str()));
  JS_SetPropertyStr(ctx, obj, "oncomplete", JS_UNDEFINED);
  JS_SetPropertyStr(ctx, obj, "onabort", JS_UNDEFINED);
  JS_SetPropertyStr(ctx, obj, "onerror", JS_UNDEFINED);
  BindIdbMethod(ctx, obj, "objectStore", IdbTransactionObjectStore, handle_idx);
  BindIdbMethod(ctx, obj, "abort", IdbTransactionAbort, handle_idx);
  BindIdbMethod(ctx, obj, "commit", IdbTransactionCommit, handle_idx);
  if (!JS_IsUndefined(handle->object)) {
    JS_FreeValue(ctx, handle->object);
  }
  handle->object = JS_DupValue(ctx, obj);
  return obj;
}

JSValue MakeIdbStoreObject(Impl* impl, JSContext* ctx, int handle_idx)
{
  const std::shared_ptr<Impl::IdbHandle>& handle =
      impl->idb_handles[static_cast<std::size_t>(handle_idx)];
  const Impl::IdbHandle* db = nullptr;
  if (handle->db_handle >= 0 &&
      static_cast<std::size_t>(handle->db_handle) < impl->idb_handles.size()) {
    db = impl->idb_handles[static_cast<std::size_t>(handle->db_handle)].get();
  }
  JSValue obj = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, handle->store_name.c_str()));
  std::string key_path;
  bool auto_increment = false;
  if (db != nullptr) {
    for (const Impl::IdbStoreInfo& info : db->stores) {
      if (info.name == handle->store_name) {
        key_path = info.key_path;
        auto_increment = info.auto_increment;
        break;
      }
    }
  }
  if (key_path.empty()) {
    JS_SetPropertyStr(ctx, obj, "keyPath", JS_NULL);
  } else {
    JS_SetPropertyStr(ctx, obj, "keyPath", JS_NewString(ctx, key_path.c_str()));
  }
  JS_SetPropertyStr(ctx, obj, "autoIncrement", JS_NewBool(ctx, auto_increment ? 1 : 0));
  BindIdbMethod(ctx, obj, "add", IdbStoreAdd, handle_idx);
  BindIdbMethod(ctx, obj, "put", IdbStorePut, handle_idx);
  BindIdbMethod(ctx, obj, "get", IdbStoreGet, handle_idx);
  BindIdbMethod(ctx, obj, "delete", IdbStoreDelete, handle_idx);
  BindIdbMethod(ctx, obj, "clear", IdbStoreClear, handle_idx);
  BindIdbMethod(ctx, obj, "count", IdbStoreCount, handle_idx);
  BindIdbMethod(ctx, obj, "getAll", IdbStoreGetAll, handle_idx);
  if (!JS_IsUndefined(handle->object)) {
    JS_FreeValue(ctx, handle->object);
  }
  handle->object = JS_DupValue(ctx, obj);
  return obj;
}

// ---------------------------------------------------------------------------
// Window extensions: requestAnimationFrame, scrolling, history,
// performance.now(), getComputedStyle.
// ---------------------------------------------------------------------------

JSValue
WindowRequestAnimationFrame(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr) {
    return JS_ThrowTypeError(ctx, "no page runtime");
  }
  if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
    return JS_ThrowTypeError(ctx, "requestAnimationFrame requires a callback");
  }
  Impl::RafEntry entry;
  entry.id = impl->next_raf_id++;
  entry.callback = JS_DupValue(ctx, argv[0]);
  const int64_t id = entry.id;
  impl->raf_queue.push_back(std::move(entry));
  return JS_NewInt64(ctx, id);
}

JSValue
WindowCancelAnimationFrame(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
  Impl* impl = ImplFor(ctx, this_val);
  if (impl == nullptr) {
    return JS_UNDEFINED;
  }
  int64_t id = 0;
  if (argc >= 1) {
    if (JS_ToInt64(ctx, &id, argv[0]) != 0) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      return JS_UNDEFINED;
    }
  }
  auto erase_by_id = [&](std::vector<Impl::RafEntry>& v) {
    for (auto it = v.begin(); it != v.end(); ++it) {
      if (it->id == id) {
        JS_FreeValue(ctx, it->callback);
        v.erase(it);
        break;
      }
    }
  };
  erase_by_id(impl->raf_queue);
  erase_by_id(impl->raf_pending);
  return JS_UNDEFINED;
}

} // namespace neko::javascript
