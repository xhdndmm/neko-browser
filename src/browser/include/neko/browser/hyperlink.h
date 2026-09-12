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

// Resolves a possibly-relative URL reference (a hyperlink href, a form action,
// a redirect location) against |base_url| — the document's URL.
//
// An absolute reference is one that carries its own scheme (https:, file:,
// data:, ...) and is returned unchanged: resolving it against the base used to
// corrupt it (a file:// href on a file:// page was concatenated onto the
// base's directory).  For file:// and bare-path bases, which the URL parser
// cannot resolve against, references are joined by string concatenation —
// root-relative references keep the base's root.  An empty reference resolves
// to the base itself.  Returns nullopt when the reference cannot be resolved
// at all (no usable base and no scheme of its own).
std::optional<std::string> ResolveReference(std::string_view ref, std::string_view base_url);

} // namespace neko::browser
