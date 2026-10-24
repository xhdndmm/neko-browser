    // Builds one flex item's box.  For a row container |forced_content_width|
    // is the item's resolved flex main size; for a column container
    // |forced_content_height| is the main size and the width comes from the
    // item's own style or fills the container (stretch).  The box is laid out
    // with its margin-box origin at (0,0); the flex algorithm translates it
    // into place afterwards.
    std::unique_ptr<LayoutBox> BuildFlexItem(dom::Element& element,
                                             std::optional<float> forced_content_width,
                                             std::optional<float> forced_content_height,
                                             float containing_width,
                                             float cb_x,
                                             float cb_y,
                                             float cb_w,
                                             float cb_h)
    {
      auto box = std::make_unique<LayoutBox>();
      box->element = &element;
      box->style = styles.StyleFor(element);
      ResolveBoxEdges(*box, containing_width);

      const bool replaced_media = IsReplacedElement(element.tag_name());
      const image::Image* replaced_image = nullptr;
      float replaced_w = 0;
      float replaced_h = 0;
      if (replaced_media) {
        replaced_image = images != nullptr ? images->Find(element) : nullptr;
        ComputeReplacedSize(
            box->style, element, replaced_image, containing_width, cb_h, replaced_w, replaced_h);
      }

      const float border_padding_w =
          box->border_left + box->border_right + box->padding_left + box->padding_right;
      float content_width;
      if (forced_content_width.has_value()) {
        // The flex algorithm's main size is a content-box size.
        content_width = forced_content_width.value();
      } else if (box->style.width.has_value()) {
        const style::SizeSpec& width = box->style.width.value();
        content_width =
            SpecToContent(width, containing_width, border_padding_w, box->style.box_sizing);
      } else if (replaced_media) {
        content_width = replaced_w;
      } else {
        content_width = containing_width - box->margin_left - box->margin_right - border_padding_w;
        if (content_width < 0) {
          content_width = 0;
        }
      }
      // min/max-width clamp (CSS 2.2 §10.4); min wins over max.  The clamps
      // constrain the same box as the width (content-box or border-box).
      if (box->style.min_width.has_value()) {
        content_width = std::max(content_width,
                                 SpecToContent(box->style.min_width.value(),
                                               containing_width,
                                               border_padding_w,
                                               box->style.box_sizing));
      }
      if (box->style.max_width.has_value()) {
        content_width = std::min(content_width,
                                 SpecToContent(box->style.max_width.value(),
                                               containing_width,
                                               border_padding_w,
                                               box->style.box_sizing));
      }
      if (box->style.min_width.has_value()) {
        content_width = std::max(content_width,
                                 SpecToContent(box->style.min_width.value(),
                                               containing_width,
                                               border_padding_w,
                                               box->style.box_sizing));
      }
      box->width = content_width + border_padding_w;
      // Margin-box origin at (0,0); content is placed at +border+padding.
      box->x = box->margin_left - box->border_left - box->padding_left;
      box->y = box->margin_top - box->border_top - box->padding_top;

      const float avail = box->width - box->border_left - box->border_right - box->padding_left -
                          box->padding_right;
      const float border_padding_h =
          box->border_top + box->border_bottom + box->padding_top + box->padding_bottom;
      // The item's definite content height, known before content layout, so
      // that percentage-height descendants resolve against it (CSS 2.2 §10.5:
      // a percentage resolves against a specified containing block height).
      // Percentages resolve against the flex container's content height when
      // definite (the caller passes it as cb_h; 0 = indefinite -> auto).
      float definite_height = 0;
      if (box->style.height.has_value()) {
        const style::SizeSpec& height = box->style.height.value();
        if (!height.percent) {
          definite_height =
              SpecToContent(height, containing_width, border_padding_h, box->style.box_sizing);
        } else if (cb_h > 0) {
          definite_height = SpecToContent(height, cb_h, border_padding_h, box->style.box_sizing);
        }
      }
      std::vector<AbsoluteChild> absolute_children;
      float content_height = 0;
      if (replaced_media) {
        // Replaced content has no in-flow children: its content height is the
        // replaced size (the decoded pixels ride on the box for the painter).
        box->image = replaced_image;
        content_height = replaced_h;
        // Aspect-ratio transfer (CSS Images 3 §5.1): when the main size was
        // resolved by the flex algorithm and the cross size is indefinite
        // (auto, or a percentage against an indefinite containing block),
        // the replaced height follows the intrinsic ratio of the USED width.
        // A 976x550 slide image shrunk to a 240px flex line is 135px tall,
        // not its intrinsic 550px -- without this the image (a carousel
        // slide or a cover picture inside a percentage chain) sized itself
        // from its pixels and overflowed the clipped container.
        const bool cross_indefinite =
            !box->style.height.has_value() || (box->style.height.value().percent && cb_h <= 0);
        if (forced_content_width.has_value() && replaced_w > 0 && cross_indefinite) {
          content_height = replaced_h * (forced_content_width.value() / replaced_w);
        }
      } else {
        content_height = LayoutBlockContent(*box,
                                            element,
                                            avail,
                                            cb_x,
                                            cb_y,
                                            cb_w,
                                            cb_h,
                                            kNoFloats,
                                            absolute_children,
                                            /*percent_base_h=*/definite_height);
      }
      if (forced_content_height.has_value()) {
        content_height = forced_content_height.value();
      } else if (box->style.height.has_value()) {
        const style::SizeSpec& height = box->style.height.value();
        if (!height.percent) {
          content_height = std::max(content_height, definite_height);
        } else if (definite_height > 0) {
          // A definite percentage is exact: without this the item fell back
          // to its content height, breaking a 100%-height chain (the
          // carousel slide image sized itself from its intrinsic pixels
          // instead of the slide area).
          content_height = definite_height;
        }
      }
      // min/max-height clamp (CSS 2.2 §10.4); min wins over max.
      if (box->style.min_height.has_value()) {
        content_height = std::max(content_height,
                                  SpecToContent(box->style.min_height.value(),
                                                containing_width,
                                                border_padding_h,
                                                box->style.box_sizing));
      }
      if (box->style.max_height.has_value()) {
        content_height = std::min(content_height,
                                  SpecToContent(box->style.max_height.value(),
                                                containing_width,
                                                border_padding_h,
                                                box->style.box_sizing));
      }
      if (box->style.min_height.has_value()) {
        content_height = std::max(content_height,
                                  SpecToContent(box->style.min_height.value(),
                                                containing_width,
                                                border_padding_h,
                                                box->style.box_sizing));
      }
      box->height = content_height + border_padding_h;
      const float child_cb_h = box->height - box->border_top - box->border_bottom;
      for (const AbsoluteChild& child : absolute_children) {
        box->positioned_children.push_back(BuildAbsolute(*child.element,
                                                         cb_x,
                                                         cb_y,
                                                         cb_w,
                                                         child_cb_h,
                                                         child.static_x,
                                                         child.static_y,
                                                         kNoFloats));
      }
      return box;
    }

    // One in-flow flex item (CSS Flexbox 1 §9.2).
    struct FlexItemData
    {
      dom::Element* element = nullptr;
      const style::ComputedStyle* style = nullptr;
      // An anonymous flex item created from a bare text node child of the
      // container (CSS Flexbox 1 §4): |text| holds the collapsed text and
      // |style| points at the container's computed style (which the anonymous
      // block inherits).  |element| is the container element.
      bool is_text = false;
      std::string text;
      float border_padding_main = 0;
      float margin_main = 0;
      float border_padding_cross = 0;
      float margin_cross = 0;
      float base_main = 0;      // flex base size (content-box)
      float min_main = 0;       // clamp floor when shrinking
      float min_main_clamp = 0; // resolved min-width/height on the main axis
      float max_main_clamp = 0; // resolved max-width/height on the main axis (0 = none)
      float content_main = 0;   // final content-box main size
      float content_cross = 0;  // natural content-box cross size
      // Measured content height of a column item (auto main size fallback
      // when the container's height is auto and flex-basis cannot resolve).
      float auto_main_height = 0;
      bool base_from_spec = false;
      bool cross_auto = true;        // no explicit cross-size property
      int auto_main_margins = 0;     // number of auto margins on the main axis
      int auto_cross_margins = 0;    // number of auto margins on the cross axis
      bool auto_main_start = false;  // an auto margin on the main-start side
      bool auto_cross_start = false; // an auto margin on the cross-start side
      std::unique_ptr<LayoutBox> box;
    };

    // A flex line (CSS Flexbox 1 §9.3).
    struct FlexLineData
    {
      std::vector<FlexItemData*> items;
      float outer_main_sum = 0; // outer main sizes including gaps
    };

    // Builds the anonymous box for a bare text flex item: it inherits the
    // flex container's computed style and lays out the text as one line (no
    // borders/padding/margins).  |forced_content_width| is the flex main size
    // when the algorithm resolved one.
    std::unique_ptr<LayoutBox> BuildAnonymousTextItem(const FlexItemData& item,
                                                      std::optional<float> forced_content_width)
    {
      auto box = std::make_unique<LayoutBox>();
      box->element = item.element; // container element (hit-testing)
      box->style = *item.style;    // anonymous flex item inherits the container
      const float text_w = MeasureTextWidth(registry,
                                            item.style->font_family,
                                            item.style->font_weight,
                                            item.style->font_italic,
                                            item.text,
                                            item.style->font_size);
      const float content_width = forced_content_width.value_or(text_w);
      box->width = content_width;
      InlineItem inl{item.text, item.style, box->element};
      std::vector<InlineItem> items;
      items.push_back(std::move(inl));
      std::vector<Line> lines;
      float height = 0;
      // A generous available width keeps the text on one line (the flex main
      // size is the item's own box, so it must not wrap).
      LayoutLines(items,
                  std::max(content_width, text_w) + 1.0f,
                  0.0f,
                  0.0f,
                  registry,
                  *item.style,
                  {},
                  lines,
                  height);
      box->lines = std::move(lines);
      box->height = std::max(height, item.style->line_height);
      return box;
    }

    // Builds an element flex item (BuildFlexItem) or an anonymous text item.
    std::unique_ptr<LayoutBox> BuildFlexItemBox(const FlexItemData& item,
                                                std::optional<float> forced_content_width,
                                                std::optional<float> forced_content_height,
                                                float containing_width,
                                                float cb_x,
                                                float cb_y,
                                                float cb_w,
                                                float cb_h)
    {
      if (item.is_text) {
        return BuildAnonymousTextItem(item, forced_content_width);
      }
      return BuildFlexItem(*item.element,
                           forced_content_width,
                           forced_content_height,
                           containing_width,
                           cb_x,
                           cb_y,
                           cb_w,
                           cb_h);
    }

    // Collects and measures the in-flow flex items of |element|.  |row|
    // selects the main-axis interpretation.  Column items are laid out at
    // their cross width up front so the measured content height can serve as
    // the flex base size.
    void CollectFlexItems(dom::Element& element,
                          const style::ComputedStyle& container_style,
                          float avail_width,
                          bool row,
                          style::AlignItems align_items,
                          float cb_x,
                          float cb_y,
                          float cb_w,
                          float cb_h,
                          std::vector<AbsoluteChild>& absolute_children,
                          std::vector<FlexItemData>& items)
    {
      for (dom::Node* child : element.ChildNodes()) {
        // A closed <details> renders only its disclosure control (§4.11.1),
        // even when an author turns it into a flex container.
        if (!RendersChild(element, *child)) {
          continue;
        }
        // A bare text node inside a flex container becomes an anonymous flex
        // item (CSS Flexbox 1 §4) carrying the container's computed style.
        // Whitespace-only text between flex items is dropped.
        if (child->node_type() == dom::NodeType::kText) {
          const std::string collapsed = MeasuredText(static_cast<const dom::Text&>(*child).data(),
                                                     container_style.white_space);
          if (collapsed.empty()) {
            continue;
          }
          FlexItemData item;
          item.is_text = true;
          item.text = collapsed;
          item.element = &element; // container element (hit-testing)
          item.style = &container_style;
          if (row) {
            item.base_main = MeasureTextWidth(registry,
                                              container_style.font_family,
                                              container_style.font_weight,
                                              container_style.font_italic,
                                              collapsed,
                                              container_style.font_size);
            item.min_main = WidestWordWidth(registry,
                                            container_style.font_family,
                                            container_style.font_weight,
                                            container_style.font_italic,
                                            collapsed,
                                            container_style.font_size);
          }
          item.cross_auto = true;
          items.push_back(std::move(item));
          continue;
        }
        if (child->node_type() != dom::NodeType::kElement) {
          continue;
        }
        dom::Element& child_el = *static_cast<dom::Element*>(child);
        const style::ComputedStyle& s = styles.StyleFor(child_el);
        if (s.display == style::Display::kNone) {
          continue;
        }
        if (s.position == style::Position::kAbsolute || s.position == style::Position::kFixed) {
          AbsoluteChild absolute;
          absolute.element = &child_el;
          absolute_children.push_back(absolute);
          continue;
        }
        FlexItemData item;
        item.element = &child_el;
        item.style = &s;
        LayoutBox edges;
        edges.style = s;
        ResolveBoxEdges(edges, avail_width);
        if (row) {
          item.border_padding_main =
              edges.border_left + edges.border_right + edges.padding_left + edges.padding_right;
          item.margin_main = edges.margin_left + edges.margin_right;
          item.border_padding_cross =
              edges.border_top + edges.border_bottom + edges.padding_top + edges.padding_bottom;
          item.margin_cross = edges.margin_top + edges.margin_bottom;
          item.auto_main_margins = (s.margin_left_auto ? 1 : 0) + (s.margin_right_auto ? 1 : 0);
          item.auto_cross_margins = (s.margin_top_auto ? 1 : 0) + (s.margin_bottom_auto ? 1 : 0);
          item.auto_main_start = s.margin_left_auto;
          item.auto_cross_start = s.margin_top_auto;
        } else {
          item.border_padding_main =
              edges.border_top + edges.border_bottom + edges.padding_top + edges.padding_bottom;
          item.margin_main = edges.margin_top + edges.margin_bottom;
          item.border_padding_cross =
              edges.border_left + edges.border_right + edges.padding_left + edges.padding_right;
          item.margin_cross = edges.margin_left + edges.margin_right;
          item.auto_main_margins = (s.margin_top_auto ? 1 : 0) + (s.margin_bottom_auto ? 1 : 0);
          item.auto_cross_margins = (s.margin_left_auto ? 1 : 0) + (s.margin_right_auto ? 1 : 0);
          item.auto_main_start = s.margin_top_auto;
          item.auto_cross_start = s.margin_left_auto;
        }
        // Resolved min/max sizes on the main axis (clamp the flexible main
        // size; percentages resolve against the container content width).
        const style::SizeSpec* min_main_spec = nullptr;
        const style::SizeSpec* max_main_spec = nullptr;
        if (row) {
          if (s.min_width.has_value()) {
            min_main_spec = &s.min_width.value();
          }
          if (s.max_width.has_value()) {
            max_main_spec = &s.max_width.value();
          }
        } else {
          if (s.min_height.has_value()) {
            min_main_spec = &s.min_height.value();
          }
          if (s.max_height.has_value()) {
            max_main_spec = &s.max_height.value();
          }
        }
        if (min_main_spec != nullptr) {
          item.min_main_clamp =
              SpecToContent(*min_main_spec, avail_width, item.border_padding_main, s.box_sizing);
        }
        if (max_main_spec != nullptr) {
          item.max_main_clamp =
              SpecToContent(*max_main_spec, avail_width, item.border_padding_main, s.box_sizing);
        }
        // Flex base size: flex-basis, else the main-size property, else the
        // item's intrinsic content size.  Percentages resolve against the
        // container's content width.
        const style::SizeSpec* main_spec = nullptr;
        if (s.flex_basis.has_value()) {
          main_spec = &s.flex_basis.value();
        } else if (row ? s.width.has_value() : s.height.has_value()) {
          main_spec = row ? &s.width.value() : &s.height.value();
        }
        const bool replaced_media = IsReplacedElement(child_el.tag_name());
        if (main_spec != nullptr) {
          item.base_main =
              SpecToContent(*main_spec, avail_width, item.border_padding_main, s.box_sizing);
          item.base_from_spec = true;
        } else if (replaced_media) {
          // Replaced items without a CSS main size use their replaced size
          // (decoded intrinsic size, else the width/height attributes).
          // MeasureContent reports 0 for them, which collapsed an <img> in a
          // flex row to zero width until a relayout followed the image load.
          const image::Image* replaced_image = images != nullptr ? images->Find(child_el) : nullptr;
          float replaced_w = 0;
          float replaced_h = 0;
          ComputeReplacedSize(s,
                              child_el,
                              replaced_image,
                              avail_width,
                              /*containing_height=*/0,
                              replaced_w,
                              replaced_h);
          item.base_main = row ? replaced_w : replaced_h;
        } else if (row) {
          item.base_main = MeasureContent(child_el, styles, registry).max;
        }
        if (row) {
          // Shrink floor: the item's min-content main size, but never below
          // the resolved min-width (CSS Flexbox 1 §9.7).  A replaced item may
          // shrink below its intrinsic size: the content scales, and keeping
          // the intrinsic size as the floor left an <img> overflowing its
          // stretched flex container (bilibili's header banner, an inline-flex
          // picture holding the art image).  Documented approximation: the
          // spec's automatic minimum for replaced items is the smaller of the
          // specified and content size suggestions; treating it as 0 matches
          // the practical "image shrinks to the container" behaviour real
          // pages rely on.
          item.min_main = replaced_media ? 0.0f : MeasureContent(child_el, styles, registry).min;
          // CSS Flexbox 1 §4.5: the content-based minimum size is the SMALLER
          // of the content size suggestion and the specified size suggestion,
          // so a definite main size caps the floor.  Without the cap a
          // `width:100%` flex item whose subtree measures wide (a replaced
          // image with width/height attributes behind percentage-sized
          // wrappers) refused to shrink to its container and stretched the
          // whole carousel (bilibili's recommended-swipe area).  The cap
          // uses the width property only -- flex-basis is not a specified
          // size suggestion -- and only in a row, where the container width
          // makes percentages definite.
          if (row && s.width.has_value()) {
            const float spec_content =
                SpecToContent(s.width.value(), avail_width, item.border_padding_main, s.box_sizing);
            if (spec_content < item.min_main) {
              item.min_main = spec_content;
            }
          }
          if (item.min_main_clamp > item.min_main) {
            item.min_main = item.min_main_clamp;
          }
        }
        item.cross_auto = !(row ? s.height.has_value() : s.width.has_value());
        items.push_back(std::move(item));
      }

      // For a column, the base main size (height) is unknown until the item
      // is laid out at its cross width: build the boxes now and measure.
      if (!row) {
        for (FlexItemData& item : items) {
          float cross_width;
          if (item.is_text) {
            cross_width = MeasureTextWidth(registry,
                                           item.style->font_family,
                                           item.style->font_weight,
                                           item.style->font_italic,
                                           item.text,
                                           item.style->font_size);
          } else if (item.style->width.has_value()) {
            cross_width = SpecToContent(item.style->width.value(),
                                        avail_width,
                                        item.border_padding_cross,
                                        item.style->box_sizing);
          } else if (align_items == style::AlignItems::kStretch) {
            cross_width = avail_width - item.margin_cross - item.border_padding_cross;
            if (cross_width < 0) {
              cross_width = 0;
            }
          } else {
            cross_width = MeasureContent(*item.element, styles, registry).max;
          }
          item.box = BuildFlexItemBox(
              item, cross_width, std::nullopt, avail_width, cb_x, cb_y, cb_w, cb_h);
          item.auto_main_height = item.box->content_height();
          if (!item.base_from_spec) {
            item.base_main = item.box->content_height();
          }
          item.min_main = item.min_main_clamp;
          item.content_cross = item.box->content_width();
        }
      }
    }

    // Collects items into flex lines, wrapping when the container main size
    // is definite (CSS Flexbox 1 §9.3).
    static void PackFlexLines(const style::ComputedStyle& cs,
                              bool main_definite,
                              float container_main,
                              float main_gap,
                              std::vector<FlexItemData>& items,
                              std::vector<FlexLineData>& lines)
    {
      FlexLineData current;
      for (FlexItemData& item : items) {
        const float outer = item.base_main + item.border_padding_main + item.margin_main;
        if (cs.flex_wrap != style::FlexWrap::kNoWrap && main_definite && !current.items.empty() &&
            current.outer_main_sum + main_gap + outer > container_main) {
          lines.push_back(std::move(current));
          current = FlexLineData{};
        }
        if (!current.items.empty()) {
          current.outer_main_sum += main_gap;
        }
        current.outer_main_sum += outer;
        current.items.push_back(&item);
      }
      if (!current.items.empty()) {
        lines.push_back(std::move(current));
      }
    }

    // Resolves each item's flexible main size on every line (CSS Flexbox 1
    // §9.7).  Grow distributes positive free space by flex-grow; shrink takes
    // it back proportionally to flex-shrink × content-box flex base size,
    // clamped at the item's min-content main size.  Resolved min/max main
    // sizes are applied last (min wins over max, matching CSS 2.2 §10.4).
    static void ResolveFlexLengths(bool main_definite,
                                   bool row,
                                   float container_main,
                                   std::vector<FlexLineData>& lines)
    {
      for (FlexLineData& line : lines) {
        for (FlexItemData* it : line.items) {
          it->content_main = it->base_main;
        }
        if (!main_definite) {
          // Auto-height column: the container has no free space to hand out
          // and flex-basis cannot be resolved against an indefinite main
          // size.  A definite height property pins the item (the codeberg
          // logo's height:200px); otherwise an item that can grow falls back
          // to its measured content height (`flex:2 1 0` used to produce a
          // 0-height box -- codeberg's column-reverse header row collapsed
          // and its children landed 600px down the page).
          if (!row) {
            for (FlexItemData* it : line.items) {
              const style::SizeSpec* height_spec =
                  it->style->height.has_value() ? &it->style->height.value() : nullptr;
              if (height_spec != nullptr && !height_spec->percent && !height_spec->is_calc &&
                  !height_spec->is_extremum) {
                it->content_main = SpecToContent(
                    *height_spec, container_main, it->border_padding_main, it->style->box_sizing);
              } else if (it->style->flex_grow > 0.0f) {
                it->content_main = std::max(it->base_main, it->auto_main_height);
              }
            }
          }
          continue;
        }
        const float free = container_main - line.outer_main_sum;
        if (free > 0) {
          float total_grow = 0;
          for (const FlexItemData* it : line.items) {
            total_grow += it->style->flex_grow;
          }
          if (total_grow > 0) {
            for (FlexItemData* it : line.items) {
              it->content_main += free * it->style->flex_grow / total_grow;
            }
          }
        } else if (free < 0) {
          float total_shrink = 0;
          for (const FlexItemData* it : line.items) {
            total_shrink += it->style->flex_shrink * it->base_main;
          }
          if (total_shrink > 0) {
            for (FlexItemData* it : line.items) {
              const float weight = it->style->flex_shrink * it->base_main;
              float final = it->base_main - (-free) * weight / total_shrink;
              if (it->min_main > 0 && final < it->min_main) {
                final = it->min_main;
              }
              if (final < 0) {
                final = 0;
              }
              it->content_main = final;
            }
          }
        }
        // Apply the resolved min/max constraints on the final main size.
        for (FlexItemData* it : line.items) {
          if (it->max_main_clamp > 0 && it->content_main > it->max_main_clamp) {
            it->content_main = it->max_main_clamp;
          }
          if (it->min_main > 0 && it->content_main < it->min_main) {
            it->content_main = it->min_main;
          }
        }
      }
    }

    // Lays out |element|'s children as flex items (CSS Flexbox 1 §9), into
    // |box| (whose border-box width is already set).  Supports
    // flex-direction row/column (and reverses), flex-wrap, flex-grow/shrink/
    // basis, justify-content, align-items, align-content (with a definite
    // container cross size) and row/column gap.  Returns the container's
    // content height.
    float LayoutFlexContent(LayoutBox& box,
                            dom::Element& element,
                            float avail_width,
                            float cb_x,
                            float cb_y,
                            float cb_w,
                            float cb_h,
                            std::vector<AbsoluteChild>& absolute_children,
                            float definite_content_height = 0)
    {
      const style::ComputedStyle& cs = box.style;
      const bool row = cs.flex_direction == style::FlexDirection::kRow ||
                       cs.flex_direction == style::FlexDirection::kRowReverse;
      const bool reverse_main = cs.flex_direction == style::FlexDirection::kRowReverse ||
                                cs.flex_direction == style::FlexDirection::kColumnReverse;
      const bool reverse_cross = cs.flex_wrap == style::FlexWrap::kWrapReverse;
      const float main_gap = row ? cs.column_gap : cs.row_gap;
      const float cross_gap = row ? cs.row_gap : cs.column_gap;

      // Container main size along the item-flow axis.  For a row this is the
      // (definite) content width; for a column it is the specified content
      // height, else auto (content-determined).  Percent heights are
      // indefinite (they depend on the containing block's resolved height,
      // which the engine does not track) and fall back to auto; treating them
      // as a definite 0 collapses the container.
      const float container_bp_h =
          box.border_top + box.border_bottom + box.padding_top + box.padding_bottom;
      float container_main = avail_width;
      bool main_definite = row;
      if (!row) {
        if (definite_content_height > 0) {
          // Caller has already resolved the used height (an absolutely
          // positioned container filling its containing block).
          container_main = definite_content_height;
          main_definite = true;
        } else if (cs.height.has_value() && !cs.height.value().percent) {
          container_main =
              SpecToContent(cs.height.value(), avail_width, container_bp_h, cs.box_sizing);
          main_definite = true;
        }
      }
      // Container cross size: for a column it is the (definite) content
      // width; for a row it is the specified content height, else auto.  A
      // percentage height resolves against the containing block height when
      // it is definite (cb_h > 0) -- e.g. an absolutely positioned inline-flex
      // picture sized 100% x 100% of its banner; with an indefinite containing
      // block the percentage is auto.  |definite_content_height| (nonzero)
      // supplies the already-resolved used height of a container that fills
      // its containing block but has no height declaration of its own.
      float container_cross = avail_width;
      bool cross_definite = !row;
      if (row) {
        if (definite_content_height > 0) {
          container_cross = definite_content_height;
          cross_definite = true;
        } else if (cs.height.has_value()) {
          const style::SizeSpec& cross_height = cs.height.value();
          if (!cross_height.percent) {
            container_cross =
                SpecToContent(cross_height, avail_width, container_bp_h, cs.box_sizing);
            cross_definite = true;
          } else if (cb_h > 0) {
            container_cross = SpecToContent(cross_height, cb_h, container_bp_h, cs.box_sizing);
            cross_definite = true;
          }
        }
      }

      // ---- Collect and measure flex items (§9.2) ----
      std::vector<FlexItemData> items;
      CollectFlexItems(element,
                       cs,
                       avail_width,
                       row,
                       cs.align_items,
                       cb_x,
                       cb_y,
                       cb_w,
                       cb_h,
                       absolute_children,
                       items);

      // ---- Ordering (CSS Flexbox 1 §5.4) ----
      // Items are laid out in ascending |order|; a stable sort keeps document
      // order for equal values (this affects packing, wrapping and the
      // painting order of the finished boxes).
      std::stable_sort(
          items.begin(), items.end(), [](const FlexItemData& a, const FlexItemData& b) {
            return a.style->order < b.style->order;
          });

      // ---- Collect items into lines (§9.3) ----
      std::vector<FlexLineData> lines;
      PackFlexLines(cs, main_definite, container_main, main_gap, items, lines);
      if (lines.empty()) {
        return 0.0f;
      }

      // ---- Resolve flexible lengths (§9.7) ----
      ResolveFlexLengths(main_definite, row, container_main, lines);

      // ---- Build row item boxes at their final main size; fix column
      // heights to their final main size ----
      for (FlexItemData& item : items) {
        if (row) {
          // cb_h for the item's percentage heights: the flex container's
          // content height when definite (cross_definite carries that), else
          // 0 (indefinite -> percentage heights are auto).
          const float item_cb_h = cross_definite ? container_cross : 0.0f;
          item.box = BuildFlexItemBox(
              item, item.content_main, std::nullopt, avail_width, cb_x, cb_y, cb_w, item_cb_h);
          item.content_cross = item.box->content_height();
        } else {
          item.box->height =
              item.content_main + item.border_padding_main; // main == height for column
        }
      }

      // ---- Cross sizes of lines and items (§9.6) ----
      std::vector<float> line_cross(lines.size(), 0.0f);
      for (std::size_t li = 0; li < lines.size(); ++li) {
        for (const FlexItemData* it : lines[li].items) {
          const float outer_cross = it->content_cross + it->border_padding_cross + it->margin_cross;
          line_cross[li] = std::max(line_cross[li], outer_cross);
        }
      }
      // A single line with a definite container cross size and the default
      // align-content: stretch grows to fill the container, so align-items
      // (center / flex-end / stretch) positions items within the full cross
      // size rather than the natural content height.
      if (cross_definite && lines.size() == 1 &&
          cs.align_content == style::AlignContent::kStretch) {
        line_cross[0] = container_cross;
      }

      float total_lines_cross = 0;
      for (std::size_t li = 0; li < lines.size(); ++li) {
        total_lines_cross += line_cross[li];
        if (li + 1 < lines.size()) {
          total_lines_cross += cross_gap;
        }
      }

      // Line cross positions.  Lines always stack along the cross axis; a
      // definite container cross size with multiple lines lets align-content
      // distribute the extra space (or stretch the lines).
      std::vector<float> line_offsets(lines.size(), 0.0f);
      float leading = 0;
      float gap = cross_gap;
      // With a definite container cross size and multiple lines,
      // align-content distributes the positive extra space.  With no (or
      // negative) extra space every alignment packs toward cross-start
      // (CSS Flexbox 1 §8.4), so the switch is skipped entirely.
      const float align_extra = cross_definite ? container_cross - total_lines_cross : 0.0f;
      if (align_extra > 0) {
        switch (cs.align_content) {
        case style::AlignContent::kStretch:
          if (align_extra > 0) {
            const float share = align_extra / static_cast<float>(lines.size());
            for (float& lc : line_cross) {
              lc += share;
            }
          }
          break;
        case style::AlignContent::kFlexEnd:
          leading = align_extra;
          break;
        case style::AlignContent::kCenter:
          leading = align_extra / 2.0f;
          break;
        case style::AlignContent::kSpaceBetween:
          // Single-line containers fall back to flex-start (CSS Flexbox 1
          // §8.4); dividing by (lines-1) with fewer than two lines would
          // yield infinity / underflow.
          if (lines.size() >= 2) {
            gap = cross_gap + align_extra / static_cast<float>(lines.size() - 1);
          }
          break;
        case style::AlignContent::kSpaceAround:
          gap = cross_gap + align_extra / static_cast<float>(lines.size());
          leading = (align_extra / static_cast<float>(lines.size())) / 2.0f;
          break;
        case style::AlignContent::kFlexStart:
          break;
        }
      }
      if (reverse_cross) {
        // Wrap-reverse stacks the lines from the cross end.
        float cursor = cross_definite ? container_cross : total_lines_cross;
        for (std::size_t li = 0; li < lines.size(); ++li) {
          cursor -= line_cross[li];
          line_offsets[li] = cursor;
          cursor -= gap;
        }
      } else {
        float cursor = leading;
        for (std::size_t li = 0; li < lines.size(); ++li) {
          line_offsets[li] = cursor;
          cursor += line_cross[li] + gap;
        }
      }

      // Baseline of a laid-out box relative to its border-box top.
      auto baseline_of = [](const LayoutBox& b) -> float {
        if (b.lines.empty()) {
          return b.height;
        }
        return b.lines.back().baseline - b.y;
      };

      // ---- Place items (§9.4 / §9.5) ----
      for (std::size_t li = 0; li < lines.size(); ++li) {
        const FlexLineData& line = lines[li];
        const int n = static_cast<int>(line.items.size());
        // Final free space along the main axis for this line.
        float content_sum = 0;
        for (const FlexItemData* it : line.items) {
          content_sum += it->content_main + it->border_padding_main + it->margin_main;
        }
        const float gaps = main_gap * static_cast<float>(std::max(0, n - 1));
        // Placement basis along the main axis.  A row and a definite-height
        // column use the container's content main size; a column with an
        // AUTO height has none yet, so the line's own extent is the basis.
        // Using the available WIDTH there (the old fallback) put every
        // reverse-direction child hundreds of pixels down the page and
        // distributed phantom free space (codeberg's column-reverse header
        // row: children landed at y=728 instead of y=72).
        const float line_main_extent = content_sum + gaps;
        const float used_main = main_definite ? container_main : line_main_extent;
        const float free = used_main - line_main_extent;
        float main_cursor = 0;
        float extra_gap = 0;
        // Auto main margins absorb the positive free space and take
        // precedence over justify-content (CSS Flexbox 1 §8.1); each auto
        // margin on the line receives an equal share.
        int auto_main_total = 0;
        for (const FlexItemData* it : line.items) {
          auto_main_total += it->auto_main_margins;
        }
        const float auto_main_share =
            (free > 0 && auto_main_total > 0) ? free / static_cast<float>(auto_main_total) : 0.0f;
        // With negative free space the line overflows the container; every
        // justify-content value then packs items toward main-start with
        // overflow at main-end (CSS Flexbox 1 §8.2), so the distribution is
        // only applied when there is space to distribute (and no auto
        // margins are consuming it).
        if (free > 0 && auto_main_total == 0) {
          switch (cs.justify_content) {
          case style::JustifyContent::kFlexEnd:
            main_cursor = free;
            break;
          case style::JustifyContent::kCenter:
            main_cursor = free / 2.0f;
            break;
          case style::JustifyContent::kSpaceBetween:
            if (n > 1) {
              extra_gap = free / static_cast<float>(n - 1);
            }
            break;
          case style::JustifyContent::kSpaceAround:
            extra_gap = free / static_cast<float>(n);
            main_cursor = extra_gap / 2.0f;
            break;
          case style::JustifyContent::kSpaceEvenly:
            extra_gap = free / static_cast<float>(n + 1);
            main_cursor = extra_gap;
            break;
          case style::JustifyContent::kFlexStart:
            break;
          }
        }

        float line_baseline = 0;
        for (const FlexItemData* it : line.items) {
          if (it->style->align_self.value_or(cs.align_items) == style::AlignItems::kBaseline) {
            line_baseline = std::max(line_baseline, baseline_of(*it->box));
          }
        }

        for (FlexItemData* it : line.items) {
          // Effective cross alignment: align-self overrides align-items.
          const style::AlignItems eff_align = it->style->align_self.value_or(cs.align_items);
          // The item's effective main margin includes its share of the free
          // space distributed to auto margins.
          const float eff_margin_main =
              it->margin_main + auto_main_share * static_cast<float>(it->auto_main_margins);
          // Cross size: stretch items fill the line exactly (auto cross
          // margins override stretch, CSS Flexbox 1 §8.1).  The stretched
          // size REPLACES the natural cross size -- taking the max left a
          // replaced item (an <img> with an intrinsic aspect ratio) taller
          // than its stretch target, so bilibili's header art image was not
          // sized to its 100%-height banner.  Content taller than the filled
          // line overflows, which is the spec behaviour.
          if (it->auto_cross_margins == 0 && eff_align == style::AlignItems::kStretch &&
              it->cross_auto) {
            const float target = line_cross[li] - it->margin_cross;
            if (row) {
              it->box->height = target;
              it->content_cross = it->box->content_height();
            } else {
              it->box->width = target;
              it->content_cross = it->box->content_width();
            }
          }

          const float outer_cross = it->content_cross + it->border_padding_cross + it->margin_cross;
          const float outer_main = it->content_main + it->border_padding_main + eff_margin_main;

          // Cross offset within the line.  Auto cross margins absorb the
          // line's free cross space and override align-self.
          float cross_pos = 0;
          if (it->auto_cross_margins > 0) {
            const float free_cross = line_cross[li] - outer_cross;
            if (free_cross > 0) {
              const float share = free_cross / static_cast<float>(it->auto_cross_margins);
              if (it->auto_cross_start) {
                cross_pos += share;
              }
            }
          } else {
            switch (eff_align) {
            case style::AlignItems::kFlexEnd:
              cross_pos = line_cross[li] - outer_cross;
              break;
            case style::AlignItems::kCenter:
              cross_pos = (line_cross[li] - outer_cross) / 2.0f;
              break;
            case style::AlignItems::kBaseline:
              cross_pos = line_baseline - baseline_of(*it->box);
              break;
            case style::AlignItems::kStretch:
            case style::AlignItems::kFlexStart:
              break;
            }
          }

          // Margin-box origin in container content coordinates.  An auto
          // margin on the item's main-start side absorbs one share of the
          // free space, pushing the item along the main axis.
          if (it->auto_main_start) {
            main_cursor += auto_main_share;
          }
          float main_pos = main_cursor;
          if (reverse_main) {
            main_pos = used_main - main_cursor - outer_main;
          }
          float cross_pos_abs = line_offsets[li] + cross_pos;

          float dx = main_pos;
          float dy = cross_pos_abs;
          if (!row) {
            std::swap(dx, dy);
          }
          TranslateBox(*it->box, box.content_x() + dx, box.content_y() + dy);
          box.children.push_back(std::move(it->box));

          main_cursor += outer_main + main_gap + extra_gap;
        }
      }

      // Container content height: for a row it is the stacked cross sizes of
      // the lines; for a column it is the tallest line's main extent.
      float content_height = 0;
      if (row) {
        content_height = total_lines_cross;
        if (cross_definite) {
          content_height = std::max(content_height, container_cross);
        }
      } else {
        for (const FlexLineData& line : lines) {
          float extent = 0;
          for (const FlexItemData* it : line.items) {
            extent += it->content_main + it->border_padding_main + it->margin_main;
          }
          extent +=
              main_gap * static_cast<float>(std::max(0, static_cast<int>(line.items.size()) - 1));
          content_height = std::max(content_height, extent);
        }
      }
      return content_height;
    }

