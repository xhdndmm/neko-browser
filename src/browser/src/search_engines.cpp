#include "neko/browser/search_engines.h"

#include "neko/url/url.h"

namespace neko::browser {

const std::vector<SearchEngine>& BuiltinSearchEngines()
{
  static const std::vector<SearchEngine> engines = {
      {"duckduckgo", "DuckDuckGo", "https://duckduckgo.com/?q=%s"},
      {"bing", "Bing", "https://www.bing.com/search?q=%s"},
      {"google", "Google", "https://www.google.com/search?q=%s"},
      {"baidu", "百度", "https://www.baidu.com/s?wd=%s"},
  };
  return engines;
}

const SearchEngine& DefaultSearchEngine(std::string_view id)
{
  for (const SearchEngine& engine : BuiltinSearchEngines()) {
    if (engine.id == id) {
      return engine;
    }
  }
  return BuiltinSearchEngines().front();
}

std::string BuildSearchUrl(std::string_view url_template, std::string_view query)
{
  const std::string encoded = url::PercentEncode(query);
  std::string out;
  const std::size_t placeholder = url_template.find("%s");
  if (placeholder == std::string_view::npos) {
    // A custom template without the placeholder: append the query as a
    // parameter rather than dropping it.
    out.assign(url_template);
    out += out.find('?') == std::string::npos ? "?q=" : "&q=";
    out += encoded;
    return out;
  }
  out.reserve(url_template.size() + encoded.size());
  out.append(url_template.substr(0, placeholder));
  out += encoded;
  out.append(url_template.substr(placeholder + 2));
  return out;
}

} // namespace neko::browser
