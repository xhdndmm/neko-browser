#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace neko::browser {

// One search engine: a stable id (stored in preferences), a display name for
// the settings UI, and a URL template whose "%s" is replaced with the
// percent-encoded query.
struct SearchEngine
{
  std::string id;
  std::string name;
  std::string url_template;
};

// The built-in engines, DuckDuckGo first (the default).
const std::vector<SearchEngine>& BuiltinSearchEngines();

// The engine with |id|; the default engine when |id| is empty or unknown.
const SearchEngine& DefaultSearchEngine(std::string_view id);

// Builds a search URL for |query| from |url_template|: the query is
// percent-encoded and substituted for the first "%s".  A template without
// "%s" gets the encoded query appended as a query parameter, so a
// misconfigured custom template still searches instead of losing the query.
std::string BuildSearchUrl(std::string_view url_template, std::string_view query);

} // namespace neko::browser
