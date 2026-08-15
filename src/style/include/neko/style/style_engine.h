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
  // collected from <style> elements plus any stylesheets registered via
  // SetExternalStylesheets (fetched from <link rel=stylesheet> by the browser
  // layer); inline styles come from the style attribute.  Idempotent for a
  // fresh document.
  void ApplyStyles(dom::Document& document);

  // Registers author stylesheets loaded from external <link rel=stylesheet>
  // resources.  They are applied after the document's <style> elements and
  // persist across ApplyStyles/ReapplyStyles calls.  The browser layer fetches
  // and parses the CSS, then hands the parsed sheets here.
  void SetExternalStylesheets(std::vector<css::StyleSheet> sheets);

  // Returns the computed style for |element|.  The element must belong to a
  // document that ApplyStyles() was called on; otherwise a default style is
  // returned.
  const ComputedStyle& StyleFor(const dom::Element& element) const;

 private:
  void ComputeElement(dom::Element& element, const ComputedStyle& inherited, float root_font_size);

  std::unordered_map<const dom::Element*, ComputedStyle> styles_;
  std::vector<css::StyleSheet> author_sheets_;
  std::vector<css::StyleSheet> external_sheets_;
};

}  // namespace neko::style
