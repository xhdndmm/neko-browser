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
