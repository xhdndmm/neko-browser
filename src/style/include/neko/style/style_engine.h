#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "neko/css/stylesheet.h"
#include "neko/dom/element.h"
#include "neko/style/computed_style.h"

namespace neko::style {

// Precomputed cascade index entry: one stylesheet rule with its per-selector
// specificities (computed once instead of per element) and its global order.
struct IndexedCascadeRule
{
  const css::StyleRule* rule = nullptr;
  std::vector<css::Specificity> specificities;
  int order = 0;
};

// Buckets rules by the key of their rightmost compound selector so per-element
// matching only considers candidate rules instead of every rule in every
// sheet (the standard CSS engine rule-hash).  A rule is added to the bucket of
// each of its selectors' rightmost keys (id, or each class, or tag, or the
// universal bucket), which never misses a match; full matching still runs on
// the candidates.
struct CascadeBuckets
{
  std::vector<IndexedCascadeRule> rules;
  std::unordered_map<std::string, std::vector<int>> by_id;
  std::unordered_map<std::string, std::vector<int>> by_class;
  std::unordered_map<std::string, std::vector<int>> by_tag;
  std::vector<int> universal;
};

// Computes per-element computed styles for a document.
//
// Pipeline: UA stylesheet + <style> author sheets + external sheets + inline
// style attribute -> selector matching -> cascade (importance > specificity >
// order) -> inheritance -> computed style.  See docs/design/style.md.
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
  void BuildCascadeIndex(dom::Document& document);

  std::unordered_map<const dom::Element*, ComputedStyle> styles_;
  std::vector<css::StyleSheet> author_sheets_;
  std::vector<css::StyleSheet> external_sheets_;
  std::unique_ptr<CascadeBuckets> buckets_;
};

}  // namespace neko::style
