    // Resolves the box-model edges (margins, borders, padding) of |box|.
    void ResolveBoxEdges(LayoutBox& box, float containing_width)
    {
      box.margin_top = ResolveSize(box.style.margin_top, containing_width);
      box.margin_right = ResolveSize(box.style.margin_right, containing_width);
      box.margin_bottom = ResolveSize(box.style.margin_bottom, containing_width);
      box.margin_left = ResolveSize(box.style.margin_left, containing_width);
      // border-style: none makes the used border width 0 (CSS Backgrounds 3
      // §4.3), even though the computed width keeps its value.
      const bool border_visible = box.style.border_style != style::BorderStyle::kNone;
      box.border_top = border_visible ? ResolveSize(box.style.border_top, containing_width) : 0.0f;
      box.border_right =
          border_visible ? ResolveSize(box.style.border_right, containing_width) : 0.0f;
      box.border_bottom =
          border_visible ? ResolveSize(box.style.border_bottom, containing_width) : 0.0f;
      box.border_left =
          border_visible ? ResolveSize(box.style.border_left, containing_width) : 0.0f;
      box.padding_top = ResolveSize(box.style.padding_top, containing_width);
      box.padding_right = ResolveSize(box.style.padding_right, containing_width);
      box.padding_bottom = ResolveSize(box.style.padding_bottom, containing_width);
      box.padding_left = ResolveSize(box.style.padding_left, containing_width);
    }

    // Applies CSS 2.1 §10.3.3 auto horizontal margins: a block-level box with
    // a definite width and auto left/right margins centers itself within its
    // containing block by splitting the leftover space between the margins
    // (the common `margin: 0 auto` pattern).  Must run after the box's width
    // is final and before its x is derived from margin_left.
    void CenterWithAutoMargins(LayoutBox& box, float containing_width)
    {
      const bool left_auto = box.style.margin_left_auto;
      const bool right_auto = box.style.margin_right_auto;
      if (!left_auto && !right_auto) {
        return;
      }
      const float used = box.width + box.margin_left + box.margin_right;
      const float leftover = containing_width - used;
      if (leftover <= 0) {
        return;
      }
      if (left_auto && right_auto) {
        box.margin_left = box.margin_right = leftover / 2.0f;
      } else if (left_auto) {
        box.margin_left = leftover;
      } else {
        box.margin_right = leftover;
      }
    }

    // Lays out an inline-block (display:inline-block, CSS2.2 9.2.2.1) as an
    // atomic block-level box at a local origin.  Width is the explicit value,
    // else shrink-to-fit min(max(min-content, available), preferred)
    // (CSS2.2 10.3.9), with the containing block width used as "available".
    // The box is positioned at its margin edge (border-box top-left at x=y=0
    // plus margins), so its four borders stay on-screen.
    std::unique_ptr<LayoutBox>
    BuildInlineBlock(dom::Element& element, float containing_width, float percent_base_height = 0)
    {
      auto box = std::make_unique<LayoutBox>();
      box->element = &element;
      box->style = styles.StyleFor(element);
      // <button> is an inline-block per the UA sheet and reaches this builder
      // instead of BuildFormControl, so it is marked here.
      SetFormControlState(*box, ClassifyFormControl(element));
      ResolveBoxEdges(*box, containing_width);

      const float border_padding_w =
          box->border_left + box->border_right + box->padding_left + box->padding_right;
      float content_width;
      if (box->style.width.has_value()) {
        content_width = SpecToContent(
            box->style.width.value(), containing_width, border_padding_w, box->style.box_sizing);
      } else {
        const float extras = box->margin_left + box->margin_right + border_padding_w;
        const float available = std::max(0.0f, containing_width - extras);
        const IntrinsicWidths w = MeasureContent(element, styles, registry);
        content_width = std::min(std::max(w.min, available), w.max);
      }
      box->width = content_width + border_padding_w;

      // Border-box origin at the margin edge; content is placed at
      // content_x()/content_y() = +border+padding.
      box->x = box->margin_left;
      box->y = box->margin_top;

      const float avail_width = box->width - box->border_left - box->border_right -
                                box->padding_left - box->padding_right;
      const float border_padding_h =
          box->border_top + box->border_bottom + box->padding_top + box->padding_bottom;
      // Definite content height, known before content layout: a specified
      // height, or a percentage against the block container's definite
      // height (the containing block for an atomic inline; CSS 2.2 §10.5).
      // Resolving it up front lets the content itself see it -- an
      // inline-flex picture with height:100% resolves its own flex items'
      // cross sizes instead of falling back to the image's intrinsic height.
      float definite_height = 0;
      if (box->style.height.has_value()) {
        const style::SizeSpec& height = box->style.height.value();
        if (!height.percent) {
          definite_height =
              SpecToContent(height, containing_width, border_padding_h, box->style.box_sizing);
        } else if (percent_base_height > 0) {
          definite_height =
              SpecToContent(height, percent_base_height, border_padding_h, box->style.box_sizing);
        }
      }
      std::vector<AbsoluteChild> absolute_children;
      float content_height = LayoutBlockContent(*box,
                                                element,
                                                avail_width,
                                                0,
                                                0,
                                                avail_width,
                                                0,
                                                kNoFloats,
                                                absolute_children,
                                                /*percent_base_h=*/definite_height,
                                                /*definite_content_height=*/definite_height);
      if (box->style.height.has_value() && !box->style.height.value().percent) {
        content_height = definite_height; // specified height wins (10.6.2)
      } else if (box->style.height.has_value() && definite_height > 0) {
        content_height = definite_height; // definite percentage: exact
      } else if (box->style.aspect_ratio.has_value() && box->style.width.has_value()) {
        // aspect-ratio (CSS Box Sizing 4): definite width, auto height.
        content_height = content_width / box->style.aspect_ratio.value();
      }
      box->height = content_height + border_padding_h;
      const float child_cb_h = box->height - box->border_top - box->border_bottom;
      for (const AbsoluteChild& child : absolute_children) {
        box->positioned_children.push_back(BuildAbsolute(*child.element,
                                                         0,
                                                         0,
                                                         avail_width,
                                                         child_cb_h,
                                                         child.static_x,
                                                         child.static_y,
                                                         kNoFloats));
      }
      return box;
    }

    // Lays out a float (float:left/right, CSS2.2 5.5) as an out-of-flow box at
    // the given vertical position |float_y| within the containing block |
    // |containing_width| wide.  Width is the explicit value, else shrink-to-fit
    // (CSS2.2 10.3.5).  A left float hugs the left edge; a right float the
    // right edge — beside earlier floats of the same side (CSS2.2 9.5.1:
    // "the outer left edge may not be to the left of the outer left edge of
    // any earlier left float"), dropping below them when there is no room.
    // |style.clear| pushes the float below matching earlier floats.
    std::unique_ptr<LayoutBox> BuildFloat(dom::Element& element,
                                          float containing_width,
                                          float left_edge,
                                          float float_y,
                                          const std::vector<const LayoutBox*>& earlier_floats = {})
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
        ComputeReplacedSize(box->style,
                            element,
                            replaced_image,
                            containing_width,
                            /*containing_height=*/0,
                            replaced_w,
                            replaced_h);
      }

      const float border_padding_w =
          box->border_left + box->border_right + box->padding_left + box->padding_right;
      float content_width;
      if (box->style.width.has_value()) {
        content_width = SpecToContent(
            box->style.width.value(), containing_width, border_padding_w, box->style.box_sizing);
      } else if (replaced_media) {
        // A floated replaced box shrink-to-fits to its replaced size.
        content_width = replaced_w;
      } else {
        const float extras = box->margin_left + box->margin_right + border_padding_w;
        const float available = std::max(0.0f, containing_width - extras);
        const IntrinsicWidths w = MeasureContent(element, styles, registry);
        content_width = std::min(std::max(w.min, available), w.max);
      }
      box->width = content_width + border_padding_w;

      const bool left_side = box->style.floating == style::Float::kLeft;

      // True when |f|'s vertical band overlaps [top, bottom).
      const auto overlaps_band = [](const LayoutBox* f, float top, float bottom) {
        return f->y < bottom && f->y + f->height > top;
      };

      // clear (CSS2.2 9.5.2): start below the bottom edge of every earlier
      // float on the cleared side(s).
      float place_y = float_y;
      if (box->style.clear != style::Clear::kNone) {
        for (const LayoutBox* f : earlier_floats) {
          const bool relevant = box->style.clear == style::Clear::kBoth ||
                                (box->style.clear == style::Clear::kLeft &&
                                 f->style.floating == style::Float::kLeft) ||
                                (box->style.clear == style::Clear::kRight &&
                                 f->style.floating == style::Float::kRight);
          if (relevant) {
            place_y = std::max(place_y, f->y + f->height);
          }
        }
      }

      // Provisional placement at the classic position (edge-hugging): content
      // is laid out here first because the avoidance pass below needs the
      // real box height for its band tests.
      const float provisional_x =
          left_side ? left_edge + box->margin_left
                    : left_edge + containing_width - box->margin_right - box->width;
      box->x = provisional_x;
      box->y = place_y + box->margin_top;

      const float avail_width = box->width - box->border_left - box->border_right -
                                box->padding_left - box->padding_right;
      std::vector<AbsoluteChild> absolute_children;
      float content_height = 0;
      if (replaced_media) {
        // Replaced content has no in-flow children: its content height is the
        // replaced size (the decoded pixels ride on the box for the painter).
        box->image = replaced_image;
        content_height = replaced_h;
      } else {
        content_height = LayoutBlockContent(*box,
                                            element,
                                            avail_width,
                                            box->border_left,
                                            box->border_top,
                                            avail_width,
                                            0,
                                            kNoFloats,
                                            absolute_children);
      }
      const float border_padding_h =
          box->border_top + box->border_bottom + box->padding_top + box->padding_bottom;
      if (box->style.height.has_value() && !box->style.height.value().percent) {
        content_height = SpecToContent(
            box->style.height.value(), containing_width, border_padding_h, box->style.box_sizing);
      } else if (box->style.aspect_ratio.has_value() && box->style.width.has_value()) {
        content_height = content_width / box->style.aspect_ratio.value();
      }
      box->height = content_height + border_padding_h;

      // Avoidance pass (CSS2.2 §9.5.1), now with the real height: sit as high
      // and far toward the floated side as possible; when the band beside the
      // earlier floats is full, drop below the blockers and retry.
      // Terminates: each non-fitting pass advances |place_y| past at least
      // one earlier float's bottom edge.
      float final_x = provisional_x;
      for (;;) {
        float candidate_x = left_side
                                ? left_edge + box->margin_left
                                : left_edge + containing_width - box->margin_right - box->width;
        for (const LayoutBox* f : earlier_floats) {
          if (!overlaps_band(f, place_y, place_y + box->height)) {
            continue;
          }
          if (left_side && f->style.floating == style::Float::kLeft) {
            candidate_x = std::max(candidate_x, f->x + f->width + box->margin_left);
          } else if (!left_side && f->style.floating == style::Float::kRight) {
            candidate_x = std::min(candidate_x, f->x - box->margin_right - box->width);
          }
        }
        const bool fits = left_side
                              ? candidate_x + box->width <= left_edge + containing_width + 0.5f
                              : candidate_x >= left_edge - 0.5f;
        final_x = candidate_x;
        if (fits) {
          break;
        }
        float next_y = place_y;
        for (const LayoutBox* f : earlier_floats) {
          if (overlaps_band(f, place_y, place_y + box->height)) {
            next_y = std::max(next_y, f->y + f->height);
          }
        }
        // Defensive: nothing advanced (float wider than its container) —
        // keep the computed position rather than looping forever.
        if (next_y <= place_y) {
          break;
        }
        place_y = next_y;
      }

      // Shift the laid-out subtree to the final position (children keep their
      // relative offsets).
      const float dx = final_x - box->x;
      const float dy = place_y + box->margin_top - box->y;
      if (dx != 0 || dy != 0) {
        TranslateBox(*box, dx, dy);
      }
      return box;
    }

    // Collects inline content under |node| into |items|: text, <br>, atomic
    // replaced <img> boxes, inline-blocks (atomic block boxes), and inline
    // element recursion.  |style|/|element| apply to |node|'s text children.
    // |containing_width| is the block container's content width;
    // |percent_base_height| is its definite content height (0 = indefinite),
    // the basis for an atomic inline box's percentage height.
    void CollectInline(dom::Node& node,
                       const style::ComputedStyle& style,
                       const dom::Element* element,
                       float containing_width,
                       std::vector<InlineItem>& items,
                       float percent_base_height = 0)
    {
      if (node.node_type() == dom::NodeType::kText) {
        CollectText(static_cast<const dom::Text&>(node).data(), style, element, items);
        return;
      }
      if (node.node_type() != dom::NodeType::kElement) {
        return;
      }
      dom::Element& child_element = static_cast<dom::Element&>(node);
      const style::ComputedStyle& child_style = styles.StyleFor(child_element);
      // display:none removes the element AND its whole subtree from layout
      // (CSS2.2 §9.2.4).  Without this check an inline descendant of a hidden
      // element still leaked into the line boxes (e.g. a hover-only dropdown
      // menu showed before hover).
      if (child_style.display == style::Display::kNone) {
        return;
      }
      if (child_element.tag_name() == "br") {
        items.push_back(InlineItem{{}, &child_style, &child_element, /*line_break=*/true});
        return;
      }
      if (IsReplacedElement(child_element.tag_name())) {
        const image::Image* img = images != nullptr ? images->Find(child_element) : nullptr;
        float w = 0;
        float h = 0;
        ComputeReplacedSize(
            child_style, child_element, img, containing_width, /*containing_height=*/0, w, h);
        // A replaced image/media/canvas/svg element's baseline is its bottom
        // edge.
        items.push_back(InlineItem{{},
                                   &child_style,
                                   &child_element,
                                   /*line_break=*/false,
                                   /*atomic=*/true,
                                   img,
                                   w,
                                   h,
                                   /*baseline_offset=*/h});
        return;
      }
      // Form controls render as atomic inline boxes with value/placeholder.
      if (child_element.tag_name() == "input" || child_element.tag_name() == "textarea" ||
          child_element.tag_name() == "select") {
        auto block_box = BuildFormControl(child_element, containing_width);
        if (block_box == nullptr) {
          // input[type=hidden] generates no box (§15.3.10) and no inline item.
          return;
        }
        InlineItem item;
        item.style = &child_style;
        item.element = &child_element;
        item.atomic = true;
        item.width = block_box->margin_left + block_box->width + block_box->margin_right;
        item.height = block_box->margin_top + block_box->height + block_box->margin_bottom;
        item.baseline_offset = block_box->height;
        item.block_box = std::move(block_box);
        items.push_back(std::move(item));
        return;
      }
      if (child_style.display == style::Display::kInlineBlock &&
          (child_style.position == style::Position::kStatic ||
           child_style.position == style::Position::kRelative)) {
        auto block_box = BuildInlineBlock(child_element, containing_width, percent_base_height);
        InlineItem item;
        item.style = &child_style;
        item.element = &child_element;
        item.atomic = true;
        item.width = block_box->margin_left + block_box->width + block_box->margin_right;
        item.height = block_box->margin_top + block_box->height + block_box->margin_bottom;
        item.baseline_offset = block_box->lines.empty() ? block_box->margin_top + block_box->height
                                                        : block_box->lines.back().baseline;
        item.block_box = std::move(block_box);
        items.push_back(std::move(item));
        return;
      }
      if (child_style.display == style::Display::kInlineFlex &&
          (child_style.position == style::Position::kStatic ||
           child_style.position == style::Position::kRelative)) {
        // Inline-level flex container: an atomic inline box whose content is
        // a flex container (handled by LayoutFlexContent inside
        // BuildInlineBlock's LayoutBlockContent dispatch).
        auto block_box = BuildInlineBlock(child_element, containing_width, percent_base_height);
        InlineItem item;
        item.style = &child_style;
        item.element = &child_element;
        item.atomic = true;
        item.width = block_box->margin_left + block_box->width + block_box->margin_right;
        item.height = block_box->margin_top + block_box->height + block_box->margin_bottom;
        item.baseline_offset = block_box->lines.empty() ? block_box->margin_top + block_box->height
                                                        : block_box->lines.back().baseline;
        item.block_box = std::move(block_box);
        items.push_back(std::move(item));
        return;
      }
      if (child_style.display == style::Display::kInlineGrid &&
          (child_style.position == style::Position::kStatic ||
           child_style.position == style::Position::kRelative)) {
        // Inline-level grid container: an atomic inline box whose content is
        // a grid container (handled by LayoutGridContent inside
        // BuildInlineBlock's LayoutBlockContent dispatch).
        auto block_box = BuildInlineBlock(child_element, containing_width, percent_base_height);
        InlineItem item;
        item.style = &child_style;
        item.element = &child_element;
        item.atomic = true;
        item.width = block_box->margin_left + block_box->width + block_box->margin_right;
        item.height = block_box->margin_top + block_box->height + block_box->margin_bottom;
        item.baseline_offset = block_box->lines.empty() ? block_box->margin_top + block_box->height
                                                        : block_box->lines.back().baseline;
        item.block_box = std::move(block_box);
        items.push_back(std::move(item));
        return;
      }
      for (dom::Node* child : node.ChildNodes()) {
        // A closed <details> renders only its disclosure control even when an
        // author turns it into an inline box (the block/flex/grid paths apply
        // the same rule).
        if (!RendersChild(child_element, *child)) {
          continue;
        }
        CollectInline(
            *child, child_style, &child_element, containing_width, items, percent_base_height);
      }
    }

    // Lays out the block-level children and inline content of |element| into
    // |box| (which already has its width and content origin set).  Fills
    // box.children and box.lines; returns the total content height.
    // |cb_*| is the containing block (padding box) of |element|; absolutely
    // positioned children are collected into |absolute_children| (not laid out
    // here) because their bottom/right offsets need the finished box height.
    // Lays out the block-level children and inline content of |element| into
    // |box|.  |parent_floats| are floats from the enclosing block formatting
    // context; they keep affecting this block's line boxes (and those of its
    // block children) until their vertical extent ends (CSS2.2 9.5).
    float LayoutBlockContent(LayoutBox& box,
                             dom::Element& element,
                             float avail_width,
                             float cb_x,
                             float cb_y,
                             float cb_w,
                             float cb_h,
                             const std::vector<const LayoutBox*>& parent_floats,
                             std::vector<AbsoluteChild>& absolute_children,
                             float percent_base_h = 0,
                             float definite_content_height = 0)
    {
      // A flex container's children are flex items, not normal-flow content.
      if (box.style.display == style::Display::kFlex ||
          box.style.display == style::Display::kInlineFlex) {
        return LayoutFlexContent(box,
                                 element,
                                 avail_width,
                                 cb_x,
                                 cb_y,
                                 cb_w,
                                 cb_h,
                                 absolute_children,
                                 definite_content_height);
      }
      // A grid container's children are grid items, not normal-flow content.
      if (box.style.display == style::Display::kGrid ||
          box.style.display == style::Display::kInlineGrid) {
        return LayoutGridContent(
            box, element, avail_width, cb_x, cb_y, cb_w, cb_h, absolute_children);
      }
      float cursor_y = 0;
      // Children are processed in DOM order.  Consecutive inline-level
      // children form an anonymous block (an "inline run") between the
      // block-level siblings, as CSS 2.1 §9.2.1.1 requires: a block followed
      // by text and then another block must render in exactly that order, so
      // runs and blocks are interleaved rather than grouped ("all inline
      // first, all blocks after" hoisted text above preceding blocks).
      struct InlineRun
      {
        std::vector<InlineItem> items;
      };
      struct BlockChild
      {
        dom::Element* element;
        style::ComputedStyle style;
        bool table = false;
        bool form_control = false;
      };
      struct FlowEntry
      {
        bool inline_run = false; // else a block-level child
        std::size_t index = 0;   // into inline_runs or block_children
      };
      std::vector<InlineRun> inline_runs;
      std::vector<BlockChild> block_children;
      std::vector<FlowEntry> flow;
      const auto current_inline_items = [&]() -> std::vector<InlineItem>& {
        if (flow.empty() || !flow.back().inline_run) {
          flow.push_back(FlowEntry{true, inline_runs.size()});
          inline_runs.emplace_back();
        }
        return inline_runs.back().items;
      };
      const auto start_block = [&](BlockChild bc) {
        flow.push_back(FlowEntry{false, block_children.size()});
        block_children.push_back(bc);
      };
      for (dom::Node* child : element.ChildNodes()) {
        // A closed <details> renders only its first <summary> child; the rest
        // of the subtree (text included) contributes no box, no line and no
        // hit-test target.
        if (!RendersChild(element, *child)) {
          continue;
        }
        if (child->node_type() == dom::NodeType::kText) {
          CollectText(
              static_cast<dom::Text*>(child)->data(), box.style, &element, current_inline_items());
          continue;
        }
        if (child->node_type() != dom::NodeType::kElement) {
          continue;
        }
        dom::Element& child_element = *static_cast<dom::Element*>(child);
        const style::ComputedStyle& child_style = styles.StyleFor(child_element);
        if (child_style.display == style::Display::kNone) {
          continue;
        }
        if (child_style.position == style::Position::kAbsolute ||
            child_style.position == style::Position::kFixed) {
          // Out of flow: positioned relative to the containing block.  Record
          // the static (hypothetical in-flow) position for the auto insets;
          // its horizontal part is final now, the vertical part is filled in
          // below once the preceding flow content has been laid out.
          AbsoluteChild absolute;
          absolute.element = &child_element;
          absolute.static_x = box.content_x() - cb_x;
          absolute.preceding_blocks = block_children.size();
          absolute_children.push_back(absolute);
          continue;
        }
        if (child_style.floating != style::Float::kNone &&
            child_style.position == style::Position::kStatic) {
          // float: left/right -- out of flow, placed at the block's side at
          // its source vertical position; line boxes wrap around it.
          float f_y = box.content_y() + cursor_y;
          std::vector<const LayoutBox*> placed;
          placed.reserve(box.floats.size());
          for (const auto& f : box.floats) {
            placed.push_back(f.get());
          }
          box.floats.push_back(
              BuildFloat(child_element, avail_width, box.content_x(), f_y, placed));
          continue;
        }
        if ((child_element.tag_name() == "input" || child_element.tag_name() == "textarea" ||
             child_element.tag_name() == "select") &&
            (child_style.display == style::Display::kBlock ||
             child_style.display == style::Display::kFlex ||
             child_style.display == style::Display::kGrid)) {
          // A block-level form control still paints as a replaced widget.
          start_block(
              BlockChild{&child_element, child_style, /*table=*/false, /*form_control=*/true});
        } else if (child_style.display == style::Display::kBlock ||
                   child_style.display == style::Display::kListItem ||
                   child_style.display == style::Display::kFlex ||
                   child_style.display == style::Display::kGrid) {
          start_block(BlockChild{&child_element, child_style, /*table=*/false});
        } else if (child_style.display == style::Display::kTable) {
          start_block(BlockChild{&child_element, child_style, /*table=*/true});
        } else {
          // Inline element: its text (and atomic <img>/inline-block boxes)
          // flows into the current inline run (this box's lines).
          CollectInline(child_element,
                        child_style,
                        &child_element,
                        avail_width,
                        current_inline_items(),
                        /*percent_base_height=*/percent_base_h);
        }
      }

      // Active floats for this block's line boxes: the block's own floats plus
      // those inherited from the enclosing BFC.
      std::vector<const LayoutBox*> all_floats = parent_floats;
      for (const auto& f : box.floats) {
        all_floats.push_back(f.get());
      }
      // Absolute children take their static vertical position from the flow
      // cursor at their document position: after the block-level siblings
      // that precede them.  Fill those in as the flow advances (the value is
      // only known once those siblings have been laid out).
      std::size_t next_absolute = 0;
      const auto flush_absolute_static_y = [&](std::size_t preceding_blocks) {
        while (next_absolute < absolute_children.size() &&
               absolute_children[next_absolute].preceding_blocks <= preceding_blocks) {
          absolute_children[next_absolute].static_y = box.content_y() + cursor_y - cb_y;
          ++next_absolute;
        }
      };
      // Bottom margin of the last in-flow block child, held back so it can be
      // collapsed against the next sibling's top margin (CSS 2.1 8.3.1).  Zero
      // means "nothing to collapse with", which is also the case for the first
      // child and for any block preceded by line boxes -- inline content
      // separates the margins, so they must not collapse.
      float pending_margin_bottom = 0.0f;
      bool pending_margin_valid = false;
      // The parent collapses its own top margin with its first in-flow child's
      // when it has no border/padding to separate them, so the child's
      // collapsed margin is partly applied outside the parent's content box.
      for (const FlowEntry& entry : flow) {
        if (entry.inline_run) {
          // An anonymous block of inline content between block-level siblings:
          // laid out at the current flow position, advancing the cursor.
          float lines_height = 0;
          LayoutLines(inline_runs[entry.index].items,
                      avail_width,
                      box.content_x(),
                      box.content_y() + cursor_y,
                      registry,
                      box.style,
                      all_floats,
                      box.lines,
                      lines_height);
          cursor_y += lines_height;
          // Line boxes separate adjoining block margins (CSS 2.1 8.3.1).
          pending_margin_valid = false;
          continue;
        }
        flush_absolute_static_y(entry.index);
        const BlockChild& bc = block_children[entry.index];
        // Sibling margin collapsing applies between in-flow block-level boxes
        // in the same BFC.  Tables and form controls participate in the flow
        // but the engine lays them out through their own paths, so they are
        // left out of the collapse rather than mis-positioned by it.
        const bool collapsible = !bc.table && !bc.form_control;
        // How far the child's top margin shifts the flow cursor.  The child is
        // placed with place_y + its own margin_top, so the adjustment is the
        // difference between the collapsed margin and the raw one.  Resolve the
        // margin the same way ResolveBoxEdges will, against the same containing
        // width, so the two cannot disagree.
        float top_adjust = 0.0f;
        // |pending_margin_valid| is only true directly after a block child:
        // an inline run in between resets it, because its line boxes
        // separate the adjoining margins.
        if (collapsible && pending_margin_valid) {
          const float child_margin_top = ResolveSize(bc.style.margin_top, avail_width);
          const float collapsed = CollapseAdjoiningMargins(pending_margin_bottom, child_margin_top);
          top_adjust = collapsed - child_margin_top;
        }
        pending_margin_valid = false;
        std::vector<const LayoutBox*> cur = parent_floats;
        for (const auto& f : box.floats) {
          cur.push_back(f.get());
        }
        // clear (CSS2.2 9.5.2): a block with clear:left/right/both starts
        // below the bottom edge of the matching earlier floats.
        if (bc.style.clear != style::Clear::kNone) {
          for (const LayoutBox* f : cur) {
            const bool relevant = bc.style.clear == style::Clear::kBoth ||
                                  (bc.style.clear == style::Clear::kLeft &&
                                   f->style.floating == style::Float::kLeft) ||
                                  (bc.style.clear == style::Clear::kRight &&
                                   f->style.floating == style::Float::kRight);
            if (relevant) {
              const float bottom_in_block =
                  (f->y + f->height + f->margin_bottom) - (box.content_y() + cursor_y);
              if (bottom_in_block > 0) {
                cursor_y += bottom_in_block;
              }
            }
          }
        }
        std::unique_ptr<LayoutBox> child_box;
        if (bc.form_control) {
          child_box = BuildFormControl(*bc.element, avail_width);
          if (child_box == nullptr) {
            // input[type=hidden] generates no box (§15.3.10): it contributes
            // nothing to the flow, neither a box nor vertical space.
            continue;
          }
          child_box->x = box.content_x() + child_box->margin_left;
          child_box->y = box.content_y() + cursor_y + child_box->margin_top;
          if (!child_box->lines.empty() && !child_box->lines.front().runs.empty()) {
            TextRun& run = child_box->lines.front().runs.front();
            run.x = child_box->x + child_box->border_left + child_box->padding_left;
            run.y = child_box->y + child_box->border_top + child_box->padding_top;
          }
        } else if (bc.table) {
          child_box = BuildTable(*bc.element,
                                 avail_width,
                                 box.content_x(),
                                 box.content_y() + cursor_y,
                                 cb_x,
                                 cb_y,
                                 cb_w,
                                 cb_h);
        } else {
          child_box = BuildBlock(*bc.element,
                                 avail_width,
                                 box.content_x(),
                                 box.content_y() + cursor_y + top_adjust,
                                 cb_x,
                                 cb_y,
                                 cb_w,
                                 cb_h,
                                 cur,
                                 /*percent_base_h=*/percent_base_h);
        }
        if (collapsible) {
          // The child's top margin has been consumed by the collapse, so the
          // cursor advances by the collapsed amount plus the border box; only
          // the bottom margin is held back for the next sibling.
          cursor_y += child_box->margin_top + top_adjust + child_box->height;
          pending_margin_bottom = child_box->margin_bottom;
          pending_margin_valid = true;
        } else {
          cursor_y += child_box->margin_top + child_box->height + child_box->margin_bottom;
        }
        box.children.push_back(std::move(child_box));
      }
      // NOT IMPLEMENTED: the parent/last-child and parent/first-child halves of
      // margin collapsing (CSS 2.1 8.3.1).  Adjoining *sibling* collapsing,
      // implemented above, is the case that dominates real pages (the UA
      // sheet's `p { margin: 1em 0 }` means every pair of paragraphs is an
      // adjoining pair).  The parent/child halves interact with how the root
      // box's own margins contribute to the document content height, and
      // getting that wrong silently changes the scroll height of every page,
      // so they are left as a follow-up rather than half-applied here.
      if (pending_margin_valid) {
        cursor_y += pending_margin_bottom;
      }
      // Absolute children placed after every in-flow block use the final
      // cursor (after all preceding content).
      flush_absolute_static_y(block_children.size());
      // Floats expand the containing block: its height reaches at least the
      // bottom of every float it holds (the float is out of flow, so its
      // height is not otherwise counted here).
      float bottom = cursor_y;
      for (const auto& f : box.floats) {
        bottom = std::max(bottom, (f->y - box.content_y()) + f->height);
      }

      // A list item paints its marker (bullet / ordinal) in the reserved
      // gutter at the start of its first line.  The marker is a text run in
      // the gutter between the list's padding and the content.
      if (box.style.display == style::Display::kListItem &&
          box.style.list_style_type != style::ListStyleType::kNone) {
        const std::string marker =
            ListMarkerText(box.style.list_style_type, ListItemOrdinal(element));
        if (!marker.empty()) {
          TextRun run;
          run.text = marker;
          run.font_family = box.style.font_family;
          run.font_weight = box.style.font_weight;
          run.font_italic = box.style.font_italic;
          run.font_size = box.style.font_size;
          run.color = box.style.color.value_or(css::Color{0, 0, 0, 255});
          run.visible = box.style.visibility != style::Visibility::kHidden;
          run.element = &element;
          run.width = MeasureTextWidth(
              registry, run.font_family, run.font_weight, run.font_italic, run.text, run.font_size);
          // Align the marker's top with the first line of content.
          float marker_y = box.content_y() + box.style.line_height * 0.15f;
          for (const Line& line : box.lines) {
            if (!line.runs.empty()) {
              marker_y = line.runs.front().y;
              break;
            }
          }
          run.x = box.content_x() - kListMarkerGap;
          run.y = marker_y;
          if (box.lines.empty()) {
            Line empty;
            empty.height = box.style.line_height;
            box.lines.push_back(std::move(empty));
          }
          box.lines.front().runs.insert(box.lines.front().runs.begin(), std::move(run));
        }
      }
      return bottom;
    }

    std::unique_ptr<LayoutBox> BuildBlock(dom::Element& element,
                                          float containing_width,
                                          float origin_x,
                                          float origin_y,
                                          float cb_x,
                                          float cb_y,
                                          float cb_w,
                                          float cb_h,
                                          const std::vector<const LayoutBox*>& parent_floats,
                                          float percent_base_h = 0)
    {
      auto box = std::make_unique<LayoutBox>();
      box->element = &element;
      box->style = styles.StyleFor(element);
      // A block-level <button> (display:block from author CSS) is still a
      // button widget, so it is marked here.
      SetFormControlState(*box, ClassifyFormControl(element));
      // CSS background-image: carry the URL and resolve the decoded image
      // through the ImageProvider (the browser fetches it keyed by element,
      // the same mechanism as <img>).
      if (box->style.background_image.has_value()) {
        box->background_image_url = box->style.background_image.value();
        if (images != nullptr) {
          box->background_image = images->Find(element);
        }
      }
      ResolveBoxEdges(*box, containing_width);

      // A <summary> that is the disclosure control of a <details> reserves a
      // gutter for the disclosure triangle, which the paint layer draws from
      // the box's details flags (§4.11.1).  The real UA rule makes the summary
      // a list item with a disclosure-* marker; this engine keeps it a block
      // box and models the marker as a fixed left gutter, like a list marker.
      if (const dom::Element* details = DetailsOfSummaryControl(element); details != nullptr) {
        box->details_summary = true;
        box->details_open = details->HasAttribute("open");
        box->padding_left += kDisclosureMarkerGap;
      }

      // A list item reserves space for its marker on the left: the marker box
      // is placed in the gap between the list's padding and the item content.
      if (box->style.display == style::Display::kListItem &&
          box->style.list_style_type != style::ListStyleType::kNone) {
        box->padding_left += kListMarkerGap;
      }

      // Replaced content in the block flow (img/video/canvas with display:block,
      // including table cells and list items, which share this builder).  Its
      // content box is the replaced size and the decoded pixels ride on the box
      // for the painter; without this a block-level <img> laid out as an empty
      // box of the right size and never painted its pixels (gray covers on real
      // sites).
      const bool replaced_media = IsReplacedElement(element.tag_name());
      const image::Image* replaced_image = nullptr;
      float replaced_w = 0;
      float replaced_h = 0;
      if (replaced_media) {
        replaced_image = images != nullptr ? images->Find(element) : nullptr;
        ComputeReplacedSize(box->style,
                            element,
                            replaced_image,
                            containing_width,
                            percent_base_h,
                            replaced_w,
                            replaced_h);
      }

      // Width: explicit (px or %) or fill the containing block.
      const float border_padding_w =
          box->border_left + box->border_right + box->padding_left + box->padding_right;
      float content_width;
      if (box->style.width.has_value()) {
        // box-sizing: with border-box the specified width covers border+padding.
        content_width = SpecToContent(
            box->style.width.value(), containing_width, border_padding_w, box->style.box_sizing);
      } else if (replaced_media) {
        // CSS width > presentational width attribute > intrinsic size (with
        // aspect-ratio preservation for a single axis).
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
      box->width = content_width + border_padding_w;

      // Auto horizontal margins center the box within its containing block.
      CenterWithAutoMargins(*box, containing_width);

      // Border-box position, including the relative offset.  |origin_x| is the
      // content-box origin of the parent; the border box sits at this box's
      // margin edge, so border/padding extend right/down from there (they are
      // not subtracted — doing so pushed padded/bordered boxes off-canvas).
      float rel_x = 0;
      float rel_y = 0;
      if (box->style.position == style::Position::kRelative) {
        if (!box->style.left_auto) {
          rel_x = ResolveSize(box->style.left, containing_width);
        } else if (!box->style.right_auto) {
          rel_x = -ResolveSize(box->style.right, containing_width);
        }
        if (!box->style.top_auto) {
          rel_y = ResolveSize(box->style.top, percent_base_h);
        } else if (!box->style.bottom_auto) {
          rel_y = -ResolveSize(box->style.bottom, percent_base_h);
        }
      }
      box->x = origin_x + box->margin_left + rel_x;
      box->y = origin_y + box->margin_top + rel_y;

      const float avail_width = box->width - box->border_left - box->border_right -
                                box->padding_left - box->padding_right;

      // Containing block for descendants: this box's padding box when it is a
      // positioning ancestor, else the inherited containing block.  x/y/w are
      // known now; h is only needed for absolute descendants' bottom offset
      // and is computed after the content height is known.
      float child_cb_x = cb_x;
      float child_cb_y = cb_y;
      float child_cb_w = cb_w;
      if (box->style.position != style::Position::kStatic) {
        child_cb_x = box->x + box->border_left;
        child_cb_y = box->y + box->border_top;
        child_cb_w = box->width - box->border_left - box->border_right;
      }

      const float border_padding_h =
          box->border_top + box->border_bottom + box->padding_top + box->padding_bottom;

      // Definite content height of this box, when it has an explicit height
      // that can resolve now.  It becomes the percentage-height basis for the
      // box's block children (CSS 2.2 §10.5: a percentage height resolves
      // against the containing block only when that height is definite).
      float definite_content_height = 0;
      if (box->style.height.has_value()) {
        if (box->style.height.value().percent) {
          if (percent_base_h > 0) {
            definite_content_height = std::max(0.0f,
                                               SpecToContent(box->style.height.value(),
                                                             percent_base_h,
                                                             border_padding_h,
                                                             box->style.box_sizing));
          }
        } else {
          definite_content_height = std::max(0.0f,
                                             SpecToContent(box->style.height.value(),
                                                           containing_width,
                                                           border_padding_h,
                                                           box->style.box_sizing));
        }
      }

      std::vector<AbsoluteChild> absolute_children;
      float content_height = 0;
      if (replaced_media) {
        // Replaced content has no in-flow children: its content height is the
        // replaced size (the decoded pixels were attached to the box above).
        box->image = replaced_image;
        content_height = replaced_h;
      } else {
        content_height = LayoutBlockContent(*box,
                                            element,
                                            avail_width,
                                            child_cb_x,
                                            child_cb_y,
                                            child_cb_w,
                                            /*cb_h*/ 0.0f,
                                            parent_floats,
                                            absolute_children,
                                            /*percent_base_h=*/definite_content_height,
                                            /*definite_content_height=*/definite_content_height);
      }

      if (replaced_media) {
        // ComputeReplacedSize already resolved the CSS height / attribute /
        // intrinsic ratio; re-applying them here would double-count.
      } else if (box->style.height.has_value()) {
        if (box->style.height.value().percent) {
          if (percent_base_h > 0) {
            // A percentage height against a definite containing block height
            // is exact: content taller than the box overflows (CSS 2.2
            // §10.5; the carousel's overflow:hidden relies on it).
            content_height = definite_content_height;
          }
          // else: a percentage height against an auto containing block is auto.
        } else {
          // A definite height wins over the content: the used height is the
          // specified height and taller content overflows (CSS 2.2 §10.6.3).
          // Growing the box to its content instead made `height:60px;
          // overflow:hidden` reserve its content height, so baidu.com's
          // adaptive top nav kept the space of its wrapped (clipped) rows.
          content_height = SpecToContent(
              box->style.height.value(), containing_width, border_padding_h, box->style.box_sizing);
        }
      } else if (box->style.aspect_ratio.has_value() && box->style.width.has_value()) {
        content_height = content_width / box->style.aspect_ratio.value();
      }
      auto clamp_height = [&](const style::SizeSpec& spec, bool as_min) {
        if (spec.percent && percent_base_h <= 0) {
          return; // percentage min/max-height against auto CB is none.
        }
        const float base = spec.percent ? percent_base_h : containing_width;
        const float resolved = SpecToContent(spec, base, border_padding_h, box->style.box_sizing);
        content_height =
            as_min ? std::max(content_height, resolved) : std::min(content_height, resolved);
      };
      if (box->style.min_height.has_value()) {
        clamp_height(box->style.min_height.value(), /*as_min=*/true);
      }
      if (box->style.max_height.has_value()) {
        clamp_height(box->style.max_height.value(), /*as_min=*/false);
      }
      box->height = content_height + border_padding_h;

      const float child_cb_h = box->style.position != style::Position::kStatic
                                   ? box->height - box->border_top - box->border_bottom
                                   : cb_h;
      for (const AbsoluteChild& child : absolute_children) {
        box->positioned_children.push_back(BuildAbsolute(*child.element,
                                                         child_cb_x,
                                                         child_cb_y,
                                                         child_cb_w,
                                                         child_cb_h,
                                                         child.static_x,
                                                         child.static_y,
                                                         kNoFloats));
      }
      return box;
    }

    // Lays out an absolutely positioned element (out of flow) against its
    // containing block |cb_*|.  Content is laid out at a local origin and the
    // finished box is translated into place once its height (needed for
    // bottom/right offsets) is known.  Width follows CSS2.2 §10.3.7: the
    // shrink-to-fit min(max(min-content, available), preferred) when auto, or
    // the left/right constraint equation when both insets are given.
    std::unique_ptr<LayoutBox> BuildAbsolute(dom::Element& element,
                                             float cb_x,
                                             float cb_y,
                                             float cb_w,
                                             float cb_h,
                                             float static_x,
                                             float static_y,
                                             const std::vector<const LayoutBox*>& parent_floats)
    {
      auto box = std::make_unique<LayoutBox>();
      box->element = &element;
      box->style = styles.StyleFor(element);
      // position: fixed anchors to the initial containing block (the
      // viewport), not to the positioned ancestor (CSS 2.2 §10.1): insets,
      // percentage resolutions and the auto-inset pin below all resolve
      // against it.  Layout still places the box at its scroll-0 document
      // position; the painter pins the subtree out of the scroll shift.
      if (box->style.position == style::Position::kFixed) {
        cb_x = 0;
        cb_y = 0;
        cb_w = viewport_width;
        cb_h = viewport_height;
      }
      ResolveBoxEdges(*box, cb_w);

      const float border_padding_w =
          box->border_left + box->border_right + box->padding_left + box->padding_right;
      const float extras = box->margin_left + box->margin_right + border_padding_w;
      // Available width for shrink-to-fit is found by solving the constraint
      // equation with the unspecified inset set to 0, i.e. it excludes any
      // specified left/right inset (CSS2.2 §10.3.7).
      const float left_inset = box->style.left_auto ? 0.0f : ResolveSize(box->style.left, cb_w);
      const float right_inset = box->style.right_auto ? 0.0f : ResolveSize(box->style.right, cb_w);
      const float top_inset = box->style.top_auto ? 0.0f : ResolveSize(box->style.top, cb_h);
      const float bottom_inset =
          box->style.bottom_auto ? 0.0f : ResolveSize(box->style.bottom, cb_h);
      float inset_x = 0;
      if (!box->style.left_auto) {
        inset_x += left_inset;
      }
      if (!box->style.right_auto) {
        inset_x += right_inset;
      }
      const float available = std::max(0.0f, cb_w - inset_x - extras);

      float content_width;
      if (box->style.width.has_value()) {
        content_width =
            SpecToContent(box->style.width.value(), cb_w, border_padding_w, box->style.box_sizing);
      } else if (IsReplacedElement(element.tag_name())) {
        // Replaced absolute boxes honor CSS size, else the presentational
        // width/height attributes, else the decoded intrinsic size.
        const image::Image* img = images != nullptr ? images->Find(element) : nullptr;
        float replaced_w = 0;
        float replaced_h = 0;
        ComputeReplacedSize(box->style, element, img, cb_w, cb_h, replaced_w, replaced_h);
        content_width = replaced_w;
        box->image = img;
      } else if (!box->style.left_auto && !box->style.right_auto) {
        // Constraint equation: left + width + right = containing block width.
        content_width = cb_w - left_inset - right_inset - extras;
        if (content_width < 0) {
          content_width = 0;
        }
      } else {
        // shrink-to-fit.
        const IntrinsicWidths w = MeasureContent(element, styles, registry);
        content_width = std::min(std::max(w.min, available), w.max);
      }
      box->width = content_width + border_padding_w;
      const float avail_width = box->width - box->border_left - box->border_right -
                                box->padding_left - box->padding_right;

      const float border_padding_h =
          box->border_top + box->border_bottom + box->padding_top + box->padding_bottom;
      std::vector<AbsoluteChild> absolute_children;
      float content_height = 0;
      if (IsReplacedElement(element.tag_name())) {
        const image::Image* img = images != nullptr ? images->Find(element) : nullptr;
        float replaced_w = 0;
        float replaced_h = 0;
        ComputeReplacedSize(box->style, element, img, cb_w, cb_h, replaced_w, replaced_h);
        content_height = replaced_h;
        box->image = img;
      } else {
        // The vertical constraint equation (CSS 2.2 §10.6.4): top +
        // margin-top + border/padding + height + margin-bottom + bottom =
        // containing block height.  With height:auto and both insets
        // definite the used height is known before content layout -- an
        // absolute top:0;bottom:0 panel fills the remaining space, and the
        // hint lets a flex container inside resolve its definite height (a
        // 100%-height child chain; the carousel activity banner relied on
        // it, otherwise the slide image fell back to its intrinsic height).
        float definite_fill_height = 0;
        const bool fills_vertically = !box->style.height.has_value() && cb_h > 0 &&
                                      !box->style.top_auto && !box->style.bottom_auto;
        if (fills_vertically) {
          const float vertical_extras = box->margin_top + box->margin_bottom + border_padding_h;
          definite_fill_height = std::max(0.0f, cb_h - top_inset - bottom_inset - vertical_extras);
        }
        content_height = LayoutBlockContent(*box,
                                            element,
                                            avail_width,
                                            box->border_left,
                                            box->border_top,
                                            avail_width,
                                            /*cb_h*/ cb_h,
                                            parent_floats,
                                            absolute_children,
                                            /*percent_base_h*/ 0,
                                            /*definite_content_height*/ definite_fill_height);
        if (box->style.height.has_value()) {
          const style::SizeSpec& height = box->style.height.value();
          if (!height.percent) {
            content_height =
                std::max(content_height,
                         SpecToContent(height, cb_w, border_padding_h, box->style.box_sizing));
          } else if (cb_h > 0) {
            // An absolutely positioned box resolves a percentage height
            // against the containing block's height (CSS 2.2 §10.6.4).  The
            // caller passes the nearest positioned ancestor's content height;
            // 0 means the height is not definite, in which case the
            // percentage is treated as auto.  The resolved height is exact
            // (content taller than a specified height overflows).
            content_height = SpecToContent(height, cb_h, border_padding_h, box->style.box_sizing);
          }
        } else if (fills_vertically) {
          content_height = definite_fill_height;
        }
      }
      box->height = content_height + border_padding_h;

      // Absolutely positioned descendants use this box's padding box (local).
      const float child_cb_h = box->height - box->border_top - box->border_bottom;
      for (const AbsoluteChild& child : absolute_children) {
        box->positioned_children.push_back(BuildAbsolute(*child.element,
                                                         box->border_left,
                                                         box->border_top,
                                                         avail_width,
                                                         child_cb_h,
                                                         child.static_x,
                                                         child.static_y,
                                                         parent_floats));
      }

      // Insets position the margin edge (CSS2.2 §10.3.7 / §10.6.4); the
      // border-box origin is then inset by the used margin.  With both insets
      // auto the box stays at its static position instead of the containing
      // block origin; fixed boxes remain pinned to the containing block.
      const bool fixed = box->style.position == style::Position::kFixed;
      float x = cb_x + box->margin_left;
      if (!box->style.left_auto) {
        x = cb_x + left_inset + box->margin_left;
      } else if (!box->style.right_auto) {
        x = cb_x + cb_w - right_inset - box->margin_right - box->width;
      } else if (!fixed) {
        x = cb_x + static_x + box->margin_left;
      }
      float y = cb_y + box->margin_top;
      if (!box->style.top_auto) {
        y = cb_y + top_inset + box->margin_top;
      } else if (!box->style.bottom_auto) {
        y = cb_y + cb_h - bottom_inset - box->margin_bottom - box->height;
      } else if (!fixed) {
        y = cb_y + static_y + box->margin_top;
      }
      TranslateBox(*box, x, y); // content was laid out at the local (0,0)
      return box;
    }

