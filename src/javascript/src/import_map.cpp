// neko::javascript — import maps (HTML §4.12.11).
//
// Parses `<script type="importmap">` JSON and resolves module specifiers
// through it.  JSON parsing reuses the runtime's JS_ParseJSON on a
// throwaway engine: the project has no standalone C++ JSON parser, and the
// import-map grammar (nested objects of strings) is exactly what the tested
// QuickJS parser handles.  No page code runs during parsing.

#include "neko/javascript/import_map.h"

#include "neko/javascript/script_engine.h"
#include "neko/javascript/script_engine_internal.h"
#include "neko/url/url.h"

#include <algorithm>
#include <quickjs.h>
#include <string>

namespace neko::javascript {
namespace {

// Flattens one specifier map object ({key: "value", ...}) into |out|.
base::Result<void> ParseSpecifierMap(JSContext* ctx,
                                     JSValueConst obj,
                                     std::vector<std::pair<std::string, std::string>>* out)
{
  JSPropertyEnum* props = nullptr;
  uint32_t count = 0;
  if (JS_GetOwnPropertyNames(ctx, &props, &count, obj, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) !=
      0) {
    return base::Err(base::Error::Parse("import map: cannot enumerate specifier keys"));
  }
  for (uint32_t i = 0; i < count; ++i) {
    const char* key = JS_AtomToCString(ctx, props[i].atom);
    JSValue value = JS_GetProperty(ctx, obj, props[i].atom);
    const char* value_str = JS_IsString(value) ? JS_ToCString(ctx, value) : nullptr;
    const bool ok = key != nullptr && value_str != nullptr;
    if (ok) {
      out->emplace_back(key, value_str);
    }
    if (value_str != nullptr) {
      JS_FreeCString(ctx, value_str);
    }
    JS_FreeCString(ctx, key);
    JS_FreeValue(ctx, value);
    if (!ok) {
      JS_FreePropertyEnum(ctx, props, count);
      return base::Err(base::Error::InvalidArgument("import map: values must be strings"));
    }
  }
  JS_FreePropertyEnum(ctx, props, count);
  return base::Ok();
}

// Appends a parsed scope entry, validating the prefix-key/value slash rule.
base::Result<void> ValidateEntry(const std::string& key, const std::string& value)
{
  // A prefix key ("./lib/") remaps everything under it; the remainder is
  // appended to the value, so the value must also end with "/".
  if (!key.empty() && key.back() == '/' && (value.empty() || value.back() != '/')) {
    return base::Err(base::Error::InvalidArgument("import map: prefix key \"" + key +
                                                  "\" requires a value ending with \"/\""));
  }
  return base::Ok();
}

} // namespace

base::Result<ImportMap> ParseImportMap(std::string_view json)
{
  ScriptEngine engine;
  auto* ctx = static_cast<JSContext*>(ScriptEngineContext(engine));
  if (ctx == nullptr) {
    return base::Err(base::Error::Javascript("script runtime failed to initialize"));
  }
  JSValue root = JS_ParseJSON(ctx, json.data(), json.size(), "<importmap>");
  if (JS_IsException(root)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return base::Err(base::Error::Parse("import map: invalid JSON"));
  }
  ImportMap map;
  base::Result<void> result = base::Ok();
  // Arrays are JS objects too; the spec requires an ordinary object root.
  if (!JS_IsObject(root) || JS_IsArray(root)) {
    result = base::Err(base::Error::InvalidArgument("import map: root must be an object"));
  } else if (JS_HasProperty(ctx, root, JS_NewAtom(ctx, "imports"))) {
    JSValue imports = JS_GetPropertyStr(ctx, root, "imports");
    if (!JS_IsUndefined(imports)) {
      if (JS_IsObject(imports)) {
        result = ParseSpecifierMap(ctx, imports, &map.imports);
      } else {
        result =
            base::Err(base::Error::InvalidArgument("import map: \"imports\" must be an object"));
      }
    }
    JS_FreeValue(ctx, imports);
  }
  if (result.has_value() && JS_HasProperty(ctx, root, JS_NewAtom(ctx, "scopes"))) {
    JSValue scopes = JS_GetPropertyStr(ctx, root, "scopes");
    if (!JS_IsUndefined(scopes)) {
      if (JS_IsObject(scopes)) {
        JSPropertyEnum* props = nullptr;
        uint32_t count = 0;
        if (JS_GetOwnPropertyNames(
                ctx, &props, &count, scopes, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0) {
          result = base::Err(base::Error::Parse("import map: cannot enumerate scopes"));
        } else {
          for (uint32_t i = 0; i < count && result.has_value(); ++i) {
            const char* scope_key = JS_AtomToCString(ctx, props[i].atom);
            JSValue scope_map = JS_GetProperty(ctx, scopes, props[i].atom);
            if (scope_key != nullptr && JS_IsObject(scope_map)) {
              std::vector<std::pair<std::string, std::string>> entries;
              result = ParseSpecifierMap(ctx, scope_map, &entries);
              if (result.has_value()) {
                map.scopes.emplace_back(scope_key, std::move(entries));
              }
            } else {
              result = base::Err(
                  base::Error::InvalidArgument("import map: scope entries must be objects"));
            }
            JS_FreeValue(ctx, scope_map);
            JS_FreeCString(ctx, scope_key);
          }
          JS_FreePropertyEnum(ctx, props, count);
        }
      } else {
        result =
            base::Err(base::Error::InvalidArgument("import map: \"scopes\" must be an object"));
      }
    }
    JS_FreeValue(ctx, scopes);
  }
  JS_FreeValue(ctx, root);
  if (!result.has_value()) {
    return base::Err(result.error());
  }
  for (const auto& [key, value] : map.imports) {
    base::Result<void> valid = ValidateEntry(key, value);
    if (!valid.has_value()) {
      return base::Err(valid.error());
    }
  }
  for (const auto& [scope, entries] : map.scopes) {
    for (const auto& [key, value] : entries) {
      base::Result<void> valid = ValidateEntry(key, value);
      if (!valid.has_value()) {
        return base::Err(valid.error());
      }
    }
  }
  return base::Ok(std::move(map));
}

namespace {

// True when |text| starts with |prefix| on a path-segment boundary.
bool PrefixMatch(std::string_view text, std::string_view prefix)
{
  if (!text.starts_with(prefix)) {
    return false;
  }
  return text.size() == prefix.size() ||
         (prefix.empty() || prefix.back() == '/' ? true : text[prefix.size()] == '/');
}

struct Match
{
  std::string value;
};

// Finds the exact match, then the longest prefix match, in |entries|.
std::optional<Match> MatchEntries(const std::vector<std::pair<std::string, std::string>>& entries,
                                  const std::string& specifier)
{
  for (const auto& [key, value] : entries) {
    if (key == specifier) {
      return Match{value};
    }
  }
  const std::pair<std::string, std::string>* best = nullptr;
  for (const auto& entry : entries) {
    const std::string& key = entry.first;
    if (!key.empty() && key.back() == '/' && PrefixMatch(specifier, key)) {
      if (best == nullptr || key.size() > best->first.size()) {
        best = &entry;
      }
    }
  }
  if (best != nullptr) {
    return Match{best->second + specifier.substr(best->first.size())};
  }
  return std::nullopt;
}

} // namespace

std::optional<std::string> ResolveImportMap(const ImportMap& map,
                                            const std::string& importer_url,
                                            const std::string& document_base,
                                            const std::string& specifier)
{
  if (specifier.empty()) {
    return std::nullopt;
  }
  // Scope selection: scope keys resolve against the document base into
  // absolute URLs first (HTML §4.12.11), then the longest key that prefixes
  // the importing module's URL wins; its map is checked before the
  // top-level imports.
  const base::Result<url::Url> base = url::Url::Parse(document_base);
  if (!base.has_value()) {
    return std::nullopt;
  }
  const std::pair<std::string, std::vector<std::pair<std::string, std::string>>>* scope = nullptr;
  std::string scope_url;
  for (const auto& scope_entry : map.scopes) {
    const base::Result<url::Url> normalized = url::Url::Parse(scope_entry.first, base.value());
    if (!normalized.has_value()) {
      continue;
    }
    const std::string candidate = normalized.value().Serialize();
    if (PrefixMatch(importer_url, candidate) &&
        (scope == nullptr || candidate.size() > scope_url.size())) {
      scope = &scope_entry;
      scope_url = candidate;
    }
  }
  std::optional<Match> matched;
  if (scope != nullptr) {
    matched = MatchEntries(scope->second, specifier);
  }
  if (!matched.has_value()) {
    matched = MatchEntries(map.imports, specifier);
  }
  if (!matched.has_value()) {
    return std::nullopt;
  }
  // Values resolve against the document URL (the import map's base).
  const base::Result<url::Url> resolved = url::Url::Parse(matched.value().value, base.value());
  if (!resolved.has_value()) {
    return std::nullopt;
  }
  return resolved.value().Serialize();
}

} // namespace neko::javascript
