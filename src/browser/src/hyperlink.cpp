#include "neko/browser/hyperlink.h"

#include <optional>
#include <string>
#include <string_view>

#include "neko/dom/element.h"
#include "neko/url/url.h"

namespace neko::browser {

std::optional<std::string> HyperlinkTarget(const dom::Node* node,
                                           std::string_view base_url) {
  // Walk up to the nearest <a> ancestor.
  for (const dom::Node* current = node; current != nullptr; current = current->parent()) {
    if (current->node_type() != dom::NodeType::kElement) {
      continue;
    }
    const auto& element = static_cast<const dom::Element&>(*current);
    if (element.tag_name() != "a") {
      continue;
    }
    const std::optional<std::string_view> href = element.GetAttribute("href");
    if (!href.has_value()) {
      // An <a> without href is not a hyperlink.
      return std::nullopt;
    }

    // Resolve the (possibly relative) href against the page URL.  The fragment
    // is preserved so in-page anchors keep their full target URL.
    const std::string_view ref = *href;

    // A file:// URL or a bare local path base (no scheme): the URL parser
    // cannot resolve relative references against these, so use string
    // concatenation.  Absolute hrefs (/x) resolve to the same root; relative
    // hrefs (x) resolve to the base's directory.
    const bool is_file = base_url.rfind("file://", 0) == 0;
    const bool is_bare_path = base_url.find(':') == std::string_view::npos;
    if (is_file || is_bare_path) {
      if (!ref.empty() && ref[0] == '/') {
        return is_file ? "file://" + std::string(ref) : std::string(ref);
      }
      const std::size_t slash = base_url.find_last_of('/');
      const std::string dir = std::string(
          base_url.substr(0, slash != std::string_view::npos ? slash + 1 : base_url.size()));
      return dir + std::string(ref);
    }

    const auto base = url::Url::Parse(base_url);
    if (base.has_value()) {
      const auto resolved = url::Url::Parse(ref, base.value());
      if (resolved.has_value()) {
        return resolved.value().Serialize(/*include_fragment=*/true);
      }
    }
    // No parseable base (e.g. a local file path): try the href as absolute.
    const auto absolute = url::Url::Parse(ref);
    if (absolute.has_value()) {
      return absolute.value().Serialize(/*include_fragment=*/true);
    }
    return std::nullopt;
  }
  return std::nullopt;
}

}  // namespace neko::browser
