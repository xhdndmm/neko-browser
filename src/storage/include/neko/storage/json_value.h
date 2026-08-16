#pragma once

#include "neko/base/status.h"

#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace neko::storage {

// ---------------------------------------------------------------------------
// A minimal JSON value model (RFC 8259 subset used by the storage layer).
//
// Numbers are doubles (JSON makes no integer distinction); strings are
// UTF-8.  Object member order is insertion order (std::map over a string
// key).  Used by IndexedDB as the structured-clone wire format.
// ---------------------------------------------------------------------------

struct JsonValue;
using JsonArray = std::vector<JsonValue>;
using JsonObject = std::map<std::string, JsonValue>;

struct JsonValue
{
  using Value = std::variant<std::nullptr_t, bool, double, std::string, JsonArray, JsonObject>;
  Value value;

  JsonValue() = default;
  explicit JsonValue(Value v) : value(std::move(v)) {}

  bool IsNull() const
  {
    return std::holds_alternative<std::nullptr_t>(value);
  }
  bool IsNumber() const
  {
    return std::holds_alternative<double>(value);
  }
  bool IsString() const
  {
    return std::holds_alternative<std::string>(value);
  }
  bool IsArray() const
  {
    return std::holds_alternative<JsonArray>(value);
  }
  bool IsObject() const
  {
    return std::holds_alternative<JsonObject>(value);
  }
};

// Parses JSON text.  Strict about syntax (no trailing data, no comments);
// numbers may be integers or reals.  Depth is limited to 128 and the input
// to 64 MiB so hostile input cannot exhaust the stack or heap.
base::Result<JsonValue> ParseJson(std::string_view text);

// Serializes to compact JSON text.
std::string SerializeJson(const JsonValue& value);

// Object member lookup by key (returns nullptr when absent or not an object).
const JsonValue* JsonFind(const JsonValue& object, std::string_view key);

} // namespace neko::storage
