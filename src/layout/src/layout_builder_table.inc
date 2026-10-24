    // Lays out a table cell's content into a box of the given content width.
    // The box is positioned at (0,0); the caller translates it to its grid
    // slot.  Margins are ignored (table cells have no margin in CSS).
    std::unique_ptr<LayoutBox> LayoutCell(
        dom::Element& element, float content_width, float cb_x, float cb_y, float cb_w, float cb_h)
    {
      auto box = std::make_unique<LayoutBox>();
      box->element = &element;
      box->style = styles.StyleFor(element);
      ResolveBoxEdges(*box, content_width);
      box->width = content_width + box->border_left + box->border_right + box->padding_left +
                   box->padding_right;
      const float avail_width = box->width - box->border_left - box->border_right -
                                box->padding_left - box->padding_right;
      std::vector<AbsoluteChild> absolute_children;
      const float content_height = LayoutBlockContent(
          *box, element, avail_width, cb_x, cb_y, cb_w, cb_h, kNoFloats, absolute_children);
      box->height = content_height + box->border_top + box->border_bottom + box->padding_top +
                    box->padding_bottom;
      for (const AbsoluteChild& child : absolute_children) {
        box->positioned_children.push_back(BuildAbsolute(
            *child.element, cb_x, cb_y, cb_w, cb_h, child.static_x, child.static_y, kNoFloats));
      }
      return box;
    }

    std::unique_ptr<LayoutBox> BuildTable(dom::Element& element,
                                          float containing_width,
                                          float origin_x,
                                          float origin_y,
                                          float cb_x,
                                          float cb_y,
                                          float cb_w,
                                          float cb_h)
    {
      auto table = std::make_unique<LayoutBox>();
      table->element = &element;
      table->style = styles.StyleFor(element);
      ResolveBoxEdges(*table, containing_width);

      // Table width.  An explicit width is honored directly; with width:auto
      // the table shrink-wraps its content (CSS2.1 17.5.2): the width is
      // max(min-content, min(max-content, available)).  The final value for
      // auto tables is computed below after the columns are measured.
      const float border_padding_w =
          table->border_left + table->border_right + table->padding_left + table->padding_right;
      const float available_content =
          containing_width - table->margin_left - table->margin_right - border_padding_w;
      float content_width;
      if (table->style.width.has_value()) {
        content_width = SpecToContent(table->style.width.value(),
                                      containing_width,
                                      border_padding_w,
                                      table->style.box_sizing);
      } else {
        content_width = std::max(0.0f, available_content);
      }
      table->width = content_width + border_padding_w;
      table->x = origin_x + table->margin_left - table->border_left - table->padding_left;
      table->y = origin_y + table->margin_top - table->border_top - table->padding_top;
      float table_x = table->content_x();
      float table_y = table->content_y();

      // Collect rows (flattening row groups) and cells, then place cells into a
      // grid honoring colspan/rowspan.  Row-group boundaries are kept so a
      // rowspan="0" cell spans to the end of its own group (thead/tbody/tfoot,
      // or the implicit group of consecutive anonymous <tr>).
      struct RowInfo
      {
        dom::Element* element = nullptr;
        style::ComputedStyle style;
      };
      struct RowGroup
      {
        int start = 0; // inclusive first row index
        int end = 0;   // exclusive last row index
      };
      std::vector<RowInfo> rows;
      std::vector<RowGroup> row_groups;
      std::vector<CellInfo> cells;
      std::vector<std::vector<int>> grid; // grid[r][c] = cell index, or -1
      int implicit_group_start_ = -1;     // first row of the current implicit group

      auto flush_implicit_group = [&](int next_row) {
        if (implicit_group_start_ >= 0 && implicit_group_start_ < next_row) {
          row_groups.push_back(RowGroup{implicit_group_start_, next_row});
        }
        implicit_group_start_ = -1;
      };

      auto collect_rows = [&](const dom::Element& container) {
        for (dom::Node* child : container.ChildNodes()) {
          if (child->node_type() != dom::NodeType::kElement) {
            continue;
          }
          dom::Element& el = static_cast<dom::Element&>(*child);
          const style::ComputedStyle& cs = styles.StyleFor(el);
          if (cs.display == style::Display::kTableRowGroup) {
            flush_implicit_group(static_cast<int>(rows.size()));
            const int group_start = static_cast<int>(rows.size());
            for (dom::Node* gchild : el.ChildNodes()) {
              if (gchild->node_type() == dom::NodeType::kElement &&
                  styles.StyleFor(static_cast<dom::Element&>(*gchild)).display ==
                      style::Display::kTableRow) {
                dom::Element& rel = static_cast<dom::Element&>(*gchild);
                rows.push_back(RowInfo{&rel, styles.StyleFor(rel)});
              }
            }
            row_groups.push_back(RowGroup{group_start, static_cast<int>(rows.size())});
          } else if (cs.display == style::Display::kTableRow) {
            // Consecutive anonymous <tr> children form one implicit row group.
            if (implicit_group_start_ < 0) {
              implicit_group_start_ = static_cast<int>(rows.size());
            }
            rows.push_back(RowInfo{&el, cs});
          }
          // caption / colgroup / col / whitespace are not part of the row grid.
        }
      };
      collect_rows(element);
      flush_implicit_group(static_cast<int>(rows.size()));
      const int nrows = static_cast<int>(rows.size());

      // Collect every cell with its (unresolved) span; rowspan="0" is resolved
      // below once the row-group boundaries are known.
      for (int r = 0; r < nrows; ++r) {
        dom::Element& tr = *rows[static_cast<std::size_t>(r)].element;
        for (dom::Node* child : tr.ChildNodes()) {
          if (child->node_type() != dom::NodeType::kElement) {
            continue;
          }
          dom::Element& cel = static_cast<dom::Element&>(*child);
          const style::ComputedStyle& cs = styles.StyleFor(cel);
          if (cs.display != style::Display::kTableCell) {
            continue;
          }
          CellInfo info;
          info.element = &cel;
          info.style = cs;
          info.row = r;
          info.colspan = ResolveColspan(cel);
          info.rowspan = ResolveRowspan(cel, info.grows_downward);
          cells.push_back(std::move(info));
        }
      }
      // rowspan="0": the cell spans the remaining rows of its own row group
      // (WHATWG tables.html: "span all the remaining rows in the row group").
      for (CellInfo& cell : cells) {
        if (!cell.grows_downward) {
          continue;
        }
        int group_end = nrows;
        for (const RowGroup& group : row_groups) {
          if (cell.row >= group.start && cell.row < group.end) {
            group_end = group.end;
            break;
          }
        }
        cell.rowspan = std::max(1, group_end - cell.row);
      }

      // Place cells into the grid: find each cell's first free column, honoring
      // the (now resolved) colspan/rowspan occupancy.
      for (std::size_t i = 0; i < cells.size(); ++i) {
        CellInfo& info = cells[i];
        const std::size_t ri = static_cast<std::size_t>(info.row);
        if (grid.size() <= ri) {
          grid.resize(ri + 1);
        }
        int col = 0;
        while (true) {
          bool free = true;
          for (int cc = col; cc < col + info.colspan; ++cc) {
            const std::size_t cci = static_cast<std::size_t>(cc);
            if (cci < grid[ri].size() && grid[ri][cci] != -1) {
              free = false;
              break;
            }
          }
          if (free) {
            break;
          }
          ++col;
        }
        info.col = col;
        for (int rr = info.row; rr < info.row + info.rowspan; ++rr) {
          const std::size_t rri = static_cast<std::size_t>(rr);
          if (grid.size() <= rri) {
            grid.resize(rri + 1);
          }
          const std::size_t cols = static_cast<std::size_t>(col + info.colspan);
          if (grid[rri].size() < cols) {
            grid[rri].resize(cols, -1);
          }
          for (int cc = col; cc < col + info.colspan; ++cc) {
            grid[rri][static_cast<std::size_t>(cc)] = static_cast<int>(i);
          }
        }
      }

      const int ncols =
          grid.empty()
              ? 0
              : static_cast<int>(
                    std::max_element(grid.begin(), grid.end(), [](const auto& a, const auto& b) {
                      return a.size() < b.size();
                    })->size());

      // Intrinsic widths for auto column sizing.
      for (CellInfo& cell : cells) {
        const IntrinsicWidths w = MeasureContent(*cell.element, styles, registry);
        // A column must be wide enough for the cell's content *plus* the cell's own
        // padding and border (CSS 2.1 §17.5.2.2); leaving them out made every padded
        // cell too narrow.
        const float cell_extras = HorizontalBoxExtras(cell.style);
        cell.min_width = w.min + cell_extras;
        cell.max_width = w.max + cell_extras;
      }

      // width:auto tables shrink to fit their content (CSS2.1 17.5.2):
      // table width = max(min-content, min(max-content, available)).  This
      // keeps a small table from being stretched across the full containing
      // block, which would spread its columns with large gaps.
      if (!table->style.width.has_value()) {
        float min_content = 0; // widest single column content per column
        float max_content = 0; // natural (unwrapped) content width per column
        // Sum the per-column extrema of the non-spanning cells.
        std::vector<float> col_min(static_cast<std::size_t>(ncols), 0.0f);
        std::vector<float> col_max(static_cast<std::size_t>(ncols), 0.0f);
        for (const CellInfo& cell : cells) {
          if (cell.colspan != 1) {
            continue;
          }
          const std::size_t c = static_cast<std::size_t>(cell.col);
          if (c < col_min.size()) {
            col_min[c] = std::max(col_min[c], cell.min_width);
            col_max[c] = std::max(col_max[c], cell.max_width);
          }
        }
        for (const float w : col_min) {
          min_content += w;
        }
        for (const float w : col_max) {
          max_content += w;
        }
        // A caption's natural width also contributes to the table's
        // max-content width (the table must be wide enough to hold it).
        for (dom::Node* child : element.ChildNodes()) {
          if (child->node_type() != dom::NodeType::kElement) {
            continue;
          }
          const dom::Element& child_el = static_cast<const dom::Element&>(*child);
          if (styles.StyleFor(child_el).display == style::Display::kTableCaption) {
            max_content = std::max(max_content, MeasureContent(child_el, styles, registry).max);
            break;
          }
        }
        const float fit = std::min(std::max(min_content, available_content), max_content);
        content_width = std::max(0.0f, fit);
        table->width = content_width + border_padding_w;
      }

      // max-width clamp (CSS 2.2 §10.4) on the final width.
      if (table->style.max_width.has_value()) {
        const float max_w = SpecToContent(table->style.max_width.value(),
                                          containing_width,
                                          border_padding_w,
                                          table->style.box_sizing);
        content_width = std::min(content_width, std::max(0.0f, max_w));
        table->width = content_width + border_padding_w;
      }

      // Auto horizontal margins center the table within its containing block
      // (the width is final now; recompute x and the content origin used for
      // cells/caption from the resolved margin).
      CenterWithAutoMargins(*table, containing_width);
      table->x = origin_x + table->margin_left - table->border_left - table->padding_left;
      table_x = table->content_x();
      table_y = table->content_y();

      // The table's caption (display: table-caption), if any, is laid out as a
      // block above the rows at the table's final content width and becomes
      // the table box's first child.
      std::unique_ptr<LayoutBox> caption_box;
      float caption_height = 0;
      for (dom::Node* child : element.ChildNodes()) {
        if (child->node_type() != dom::NodeType::kElement) {
          continue;
        }
        dom::Element& child_el = static_cast<dom::Element&>(*child);
        if (styles.StyleFor(child_el).display != style::Display::kTableCaption) {
          continue;
        }
        caption_box = std::make_unique<LayoutBox>();
        caption_box->element = &child_el;
        caption_box->style = styles.StyleFor(child_el);
        ResolveBoxEdges(*caption_box, content_width);
        caption_box->width = content_width + caption_box->border_left + caption_box->border_right +
                             caption_box->padding_left + caption_box->padding_right;
        const float caption_avail = caption_box->width - caption_box->border_left -
                                    caption_box->border_right - caption_box->padding_left -
                                    caption_box->padding_right;
        std::vector<AbsoluteChild> caption_absolute;
        caption_height = LayoutBlockContent(*caption_box,
                                            child_el,
                                            caption_avail,
                                            table_x,
                                            table_y,
                                            cb_w,
                                            cb_h,
                                            kNoFloats,
                                            caption_absolute);
        caption_box->height = caption_height + caption_box->border_top +
                              caption_box->border_bottom + caption_box->padding_top +
                              caption_box->padding_bottom;
        // Lay out at a local origin (0,0), then translate the whole box --
        // including the text runs laid out by LayoutBlockContent -- so that
        // its content box starts at the table's content origin (the same
        // pattern LayoutCell uses).
        TranslateBox(*caption_box,
                     table_x - caption_box->border_left - caption_box->padding_left,
                     table_y - caption_box->border_top - caption_box->padding_top);
        for (const AbsoluteChild& abs_child : caption_absolute) {
          caption_box->positioned_children.push_back(BuildAbsolute(*abs_child.element,
                                                                   table_x,
                                                                   table_y,
                                                                   cb_w,
                                                                   cb_h,
                                                                   abs_child.static_x,
                                                                   abs_child.static_y,
                                                                   kNoFloats));
        }
        break; // only the first caption participates in the table model
      }
      const std::vector<float> col_widths = ComputeColumnWidths(cells, ncols, content_width);

      // Lay out each cell (at a local origin) and derive row heights.
      for (CellInfo& cell : cells) {
        cell.box = LayoutCell(
            *cell.element, SumColumns(col_widths, cell.col, cell.colspan), cb_x, cb_y, cb_w, cb_h);
      }

      std::vector<float> row_heights(rows.size(), 0.0f);
      for (const CellInfo& cell : cells) {
        if (cell.rowspan == 1) {
          const std::size_t ri = static_cast<std::size_t>(cell.row);
          row_heights[ri] = std::max(row_heights[ri], cell.box->height);
        }
      }
      // Row-spanning cells: make sure the spanned rows are tall enough, giving
      // any overflow to the last spanned row (simplified CSS2.1 distribution).
      for (const CellInfo& cell : cells) {
        if (cell.rowspan <= 1) {
          continue;
        }
        float spanned = 0;
        for (int r = cell.row;
             r < cell.row + cell.rowspan && r < static_cast<int>(row_heights.size());
             ++r) {
          spanned += row_heights[static_cast<std::size_t>(r)];
        }
        if (cell.box->height > spanned) {
          const int last =
              std::min(cell.row + cell.rowspan - 1, static_cast<int>(row_heights.size()) - 1);
          row_heights[static_cast<std::size_t>(last)] += cell.box->height - spanned;
        }
      }

      // Build the box tree (table -> [caption] -> rows -> cells) with grid
      // positions.  The caption sits above the rows.
      float y = 0;
      if (caption_box != nullptr) {
        const float caption_outer = caption_height + caption_box->border_top +
                                    caption_box->border_bottom + caption_box->padding_top +
                                    caption_box->padding_bottom;
        table->children.push_back(std::move(caption_box));
        y += caption_outer;
      }
      for (std::size_t r = 0; r < rows.size(); ++r) {
        auto row_box = std::make_unique<LayoutBox>();
        row_box->element = rows[r].element;
        row_box->style = rows[r].style;
        row_box->x = table_x;
        row_box->y = table_y + y;
        row_box->width = content_width;
        row_box->height = row_heights[r];
        for (CellInfo& cell : cells) {
          if (cell.row != static_cast<int>(r)) {
            continue;
          }
          const float cx = table_x + SumColumns(col_widths, 0, cell.col);
          const float cy = table_y + y;
          TranslateBox(*cell.box, cx, cy);
          if (cell.rowspan <= 1) {
            cell.box->height = row_heights[r];
          } else {
            float spanned = 0;
            for (int rr = cell.row;
                 rr < cell.row + cell.rowspan && rr < static_cast<int>(row_heights.size());
                 ++rr) {
              spanned += row_heights[static_cast<std::size_t>(rr)];
            }
            cell.box->height = spanned;
          }
          row_box->children.push_back(std::move(cell.box));
        }
        table->children.push_back(std::move(row_box));
        y += row_heights[r];
      }

      table->height =
          y + table->border_top + table->border_bottom + table->padding_top + table->padding_bottom;
      return table;
    }
