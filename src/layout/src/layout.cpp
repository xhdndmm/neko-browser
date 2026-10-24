#include "neko/base/string_util.h"
#include "neko/base/utf8.h"
#include "neko/dom/element.h"
#include "neko/graphics/font_registry.h"
#include "neko/graphics/font_selector.h"
#include "neko/html/input_value.h"
#include "neko/image/image.h"
#include "neko/layout/form_control.h"
#include "neko/layout/layout_tree.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neko::layout {
#include "layout_helpers.inc"
LayoutEngine::BuildLayoutTree(dom::Document& document, float viewport_width, float viewport_height)
{
#include "layout_builder.inc"

  dom::Element* root = document.document_element();
  if (root == nullptr) {
    return nullptr;
  }
  Builder builder{styles_, registry_, images_, viewport_width, viewport_height};
  return builder.BuildBlock(*root,
                            viewport_width,
                            0,
                            0, /*cb=initial containing block*/
                            0,
                            0,
                            viewport_width,
                            viewport_height,
                            kNoFloats,
                            /*percent_base_h=*/viewport_height);
}

} // namespace neko::layout
