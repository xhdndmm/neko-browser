#include "neko/browser/hyperlink.h"

#include "neko/dom/element.h"
#include "neko/url/url.h"

#include <cctype>
#include <optional>
#include <string>
#include <string_view>

namespace neko::browser {
namespace {

// True when |ref| begins with a URL scheme (RFC 3986 §3.1):
// ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":".
bool HasScheme(std::string_view ref)
{
  if (ref.empty() || std::isalpha(static_cast<unsigned char>(ref.front())) == 0) {
    return false;
  }
  for (std::size_t i = 1; i < ref.size(); ++i) {
    const char c = ref[i];
    if (c == ':') {
      return true;
    }
    const bool scheme_char =
        std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '+' || c == '-' || c == '.';
    if (!scheme_char) {
      return false;
    }
  }
  return false;
}

} // namespace

std::optional<std::string> ResolveReference(std::string_view ref, std::string_view base_url)
{
  if (ref.empty()) {
    return std::string(base_url);
  }
  // An absolute reference carries its own scheme and is used unchanged (it must
  // not be joined onto the base: a file:// href on a file:// page used to end
  // up concatenated onto the base's directory).
  if (HasScheme(ref)) {
    return std::string(ref);
  }
  // A file:// URL or a bare local path base (no scheme): the URL parser cannot
  // resolve relative references against these, so use string concatenation.
  // Absolute references (/x) resolve to the same root; relative references (x)
  // resolve to the base's directory.
  const bool is_file = base_url.rfind("file://", 0) == 0;
  const bool is_bare_path = base_url.find(':') == std::string_view::npos;
  if (is_file || is_bare_path) {
    if (ref.front() == '/') {
      return is_file ? "file://" + std::string(ref) : std::string(ref);
    }
    const std::size_t slash = base_url.find_last_of('/');
    return std::string(
               base_url.substr(0, slash != std::string_view::npos ? slash + 1 : base_url.size())) +
           std::string(ref);
  }
  const auto base = url::Url::Parse(base_url);
  if (base.has_value()) {
    const auto resolved = url::Url::Parse(ref, base.value());
    if (resolved.has_value()) {
      return resolved.value().Serialize(/*include_fragment=*/true);
    }
  }
  return std::nullopt;
}

std::optional<std::string> HyperlinkTarget(const dom::Node* node, std::string_view base_url)
{
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
    // The resolution keeps the fragment so in-page anchors keep their full
    // target URL.
    return ResolveReference(*href, base_url);
  }
  return std::nullopt;
}

} // namespace neko::browser
