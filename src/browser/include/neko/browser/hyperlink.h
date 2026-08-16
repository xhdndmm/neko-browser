#pragma once

#include "neko/dom/node.h"

#include <optional>
#include <string>
#include <string_view>

namespace neko::browser {

// Resolves a hyperlink click to a navigation target.
//
// Walks from |node| up its ancestors to the nearest <a> element (per WHATWG
// HTML §4.6.5 "following hyperlinks", clicking any descendant of an <a>
// activates that hyperlink).  The element's href is then resolved against
// |base_url| using the WHATWG URL parser.
//
// Returns nullopt when |node| is null, no ancestor is an <a>, the <a> has no
// href, or the href cannot be resolved (e.g. an empty href with no base).
std::optional<std::string> HyperlinkTarget(const dom::Node* node, std::string_view base_url);

} // namespace neko::browser
