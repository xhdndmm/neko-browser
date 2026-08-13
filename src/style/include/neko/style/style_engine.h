#pragma once

#include <string_view>
#include <unordered_map>
#include <vector>

#include "neko/css/stylesheet.h"
#include "neko/dom/element.h"
#include "neko/style/computed_style.h"

namespace neko::style {

// Computes per-element computed styles for a document.
//
// Pipeline: UA stylesheet + <style> author sheets + inline style attribute ->
// selector matching -> cascade (importance > specificity > order) ->
// inheritance -> computed style.  See docs/design/style.md.
class StyleEngine {
 public:
  StyleEngine() = default;

  // Computes styles for every element in |document|.  Author sheets are
  // collected from <style> elements; inline styles come from the style
  // attribute.  Idempotent for a fresh document.
  void ApplyStyles(dom::Document& document);

  // Returns the computed style for |element|.  The element must belong to a
  // document that ApplyStyles() was called on; otherwise a default style is
  // returned.
  const ComputedStyle& StyleFor(const dom::Element& element) const;

 private:
  void ComputeElement(dom::Element& element, const ComputedStyle& inherited, float root_font_size);

  std::unordered_map<const dom::Element*, ComputedStyle> styles_;
  std::vector<css::StyleSheet> author_sheets_;
};

}  // namespace neko::style
