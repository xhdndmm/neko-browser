#pragma once

#include "neko/base/status.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neko::javascript {

// An import map (HTML `<script type="importmap">`): maps module specifiers
// to URLs so pages can remap bare names ("react" -> vendored bundle) or
// rewrite paths.  See the HTML spec §4.12.11 "Import maps".
//
// Supported subset (documented): top-level "imports" and "scopes".  Matching
// is textual on the raw specifier (exact key first, then the longest
// "/'-terminated prefix key whose remainder is appended to the value);
// values resolve against the document URL.  Scope selection picks the
// longest scope key that prefixes the importing module's URL.
struct ImportMap
{
  // (specifier key, value) pairs in document order; keys may be bare
  // ("react"), relative ("./lib/x.js") or absolute URLs.  A key ending in
  // "/" is a prefix key and requires a value ending in "/".
  std::vector<std::pair<std::string, std::string>> imports;

  // (scope URL prefix, specifier map) pairs; within a scope the same
  // matching rules as |imports| apply, checked before the top-level map.
  std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> scopes;
};

// Parses the JSON text of one `<script type="importmap">`.  Fails with
// Error::Parse on malformed JSON and Error::InvalidArgument on structural
// violations (non-object roots, non-string values, prefix-key/value slash
// mismatches).
base::Result<ImportMap> ParseImportMap(std::string_view json);

// Resolves |specifier| for a module imported from |importer_url|.
// Returns the mapped value resolved against |document_base| (which must be
// an absolute URL), or nullopt when nothing matches (the caller then falls
// back to the built-in relative-resolution rules).
std::optional<std::string> ResolveImportMap(const ImportMap& map,
                                            const std::string& importer_url,
                                            const std::string& document_base,
                                            const std::string& specifier);

} // namespace neko::javascript
