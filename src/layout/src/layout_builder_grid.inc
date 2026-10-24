    // Lays out |element|'s children as grid items (CSS Grid Layout 1 §7–10)
    // into |box| (whose border-box width is already set).  Supports explicit
    // grid-template-columns/rows (px/%/fr/auto/min-content/max-content,
    // repeat(), minmax(min,max)), named grid lines ([name] ...), template
    // areas (grid-template-areas + grid-area, with their implicit
    // <name>-start/<name>-end lines), line/span/named placement (numeric
    // lines may be negative), grid-auto-flow (row/column, sparse/dense) and
    // row/column gaps.  Tracks beyond the explicit template (implicit) are
    // auto-sized.  Returns the container's content height.
    float LayoutGridContent(LayoutBox& box,
                            dom::Element& element,
                            float avail_width,
                            float cb_x,
                            float cb_y,
                            float cb_w,
                            float cb_h,
                            std::vector<AbsoluteChild>& absolute_children)
    {
      const style::ComputedStyle& cs = box.style;
      const float column_gap = cs.column_gap;
      const float row_gap = cs.row_gap;
      const float container_width = avail_width;
      // The container content height is definite only for an explicit
      // non-percentage height (auto/percent rows then resolve like auto).
      const float container_bp_h =
          box.border_top + box.border_bottom + box.padding_top + box.padding_bottom;
      const bool height_definite = cs.height.has_value() && !cs.height.value().percent;
      const float container_height =
          height_definite
              ? SpecToContent(cs.height.value(), avail_width, container_bp_h, cs.box_sizing)
              : 0.0f;

      // ---- Grid definition: explicit tracks, named lines, areas ----
      const std::vector<std::vector<std::string>>& areas = cs.grid_template_areas;
      const int area_cols = areas.empty() ? 0 : static_cast<int>(areas[0].size());
      const int area_rows = static_cast<int>(areas.size());
      const int template_cols = static_cast<int>(cs.grid_template_columns.size());
      const int template_rows = static_cast<int>(cs.grid_template_rows.size());
      const int explicit_cols = std::max(template_cols, area_cols);
      const int explicit_rows = std::max(template_rows, area_rows);

      // Area rectangles (name → inclusive row/column span) from the
      // contiguous cells carrying that name.
      struct AreaRect
      {
        int r0 = 0, c0 = 0, r1 = 0, c1 = 0;
      };
      std::map<std::string, AreaRect> area_rects;
      for (int r = 0; r < area_rows; ++r) {
        for (int c = 0; c < area_cols; ++c) {
          const std::string& name = areas[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)];
          if (name.empty() || name == ".") {
            continue;
          }
          const auto it = area_rects.find(name);
          if (it == area_rects.end()) {
            area_rects.emplace(name, AreaRect{r, c, r, c});
          } else {
            it->second.r0 = std::min(it->second.r0, r);
            it->second.c0 = std::min(it->second.c0, c);
            it->second.r1 = std::max(it->second.r1, r);
            it->second.c1 = std::max(it->second.c1, c);
          }
        }
      }

      // Named lines per axis: the explicit [name] lines plus the implicit
      // <name>-start / <name>-end lines the template areas create.
      const auto build_line_names = [](const std::vector<std::vector<std::string>>& explicit_lines,
                                       int track_count,
                                       bool horizontal,
                                       const std::map<std::string, AreaRect>& rects) {
        std::vector<std::vector<std::string>> lines(static_cast<std::size_t>(track_count) + 1);
        for (std::size_t i = 0; i < explicit_lines.size() && i < lines.size(); ++i) {
          lines[i] = explicit_lines[i];
        }
        for (const auto& [name, rect] : rects) {
          const int start = horizontal ? rect.c0 : rect.r0;
          const int end = horizontal ? rect.c1 + 1 : rect.r1 + 1;
          if (start >= 0 && start < static_cast<int>(lines.size())) {
            lines[static_cast<std::size_t>(start)].push_back(name + "-start");
          }
          if (end >= 0 && end < static_cast<int>(lines.size())) {
            lines[static_cast<std::size_t>(end)].push_back(name + "-end");
          }
        }
        return lines;
      };
      const std::vector<std::vector<std::string>> col_lines =
          build_line_names(cs.grid_column_lines, explicit_cols, /*horizontal=*/true, area_rects);
      const std::vector<std::vector<std::string>> row_lines =
          build_line_names(cs.grid_row_lines, explicit_rows, /*horizontal=*/false, area_rects);

      // Resolves one placement to a 0-based line index, or -1 when it does
      // not resolve (treated as auto).  Per §8.3 an ident matches a template
      // area first (its edge on this axis), then a named line.
      const auto resolve_line = [&](const style::GridPlacement& p,
                                    const std::vector<std::vector<std::string>>& lines,
                                    bool horizontal,
                                    int track_count) -> int {
        if (p.kind == style::GridPlacement::Kind::kLine) {
          if (!p.name.empty()) {
            const auto area_it = area_rects.find(p.name);
            if (area_it != area_rects.end()) {
              return horizontal ? area_it->second.c0 : area_it->second.r0;
            }
            for (std::size_t i = 0; i < lines.size(); ++i) {
              for (const std::string& name : lines[i]) {
                if (name == p.name) {
                  return static_cast<int>(i);
                }
              }
            }
            return -1;
          }
          if (p.line < 0) {
            // Negative lines count from the end of the explicit grid
            // (line -1 = the last explicit line).
            return track_count + 1 + p.line;
          }
          return p.line - 1;
        }
        return -1;
      };
      // Resolves a named span ("span <name>"): the first line with |name|
      // after |start_line|, or -1.
      const auto resolve_span_named = [](const style::GridPlacement& p,
                                         const std::vector<std::vector<std::string>>& lines,
                                         int start_line) -> int {
        if (p.kind != style::GridPlacement::Kind::kSpan || p.name.empty()) {
          return -1;
        }
        for (std::size_t i = static_cast<std::size_t>(start_line) + 1; i < lines.size(); ++i) {
          for (const std::string& name : lines[i]) {
            if (name == p.name) {
              return static_cast<int>(i);
            }
          }
        }
        return -1;
      };

      // One in-flow grid item with its resolved placement.
      struct GridItem
      {
        dom::Element* element = nullptr;
        const style::ComputedStyle* style = nullptr;
        bool col_definite = false; // start line explicitly resolved
        bool row_definite = false;
        int col_start = 0; // 0-based (when definite)
        int row_start = 0;
        int col_span = 1;
        int row_span = 1;
        std::unique_ptr<LayoutBox> layout;
      };
      std::vector<GridItem> items;

      for (dom::Node* child : element.ChildNodes()) {
        if (!RendersChild(element, *child)) {
          continue;
        }
        if (child->node_type() != dom::NodeType::kElement) {
          continue;
        }
        auto* el = static_cast<dom::Element*>(child);
        const style::ComputedStyle& s = styles.StyleFor(*el);
        if (s.display == style::Display::kNone) {
          continue;
        }
        if (s.position == style::Position::kAbsolute || s.position == style::Position::kFixed) {
          AbsoluteChild absolute;
          absolute.element = el;
          absolute_children.push_back(absolute);
          continue;
        }
        GridItem item;
        item.element = el;
        item.style = &s;
        // Column start.
        if (s.grid_column_start.kind == style::GridPlacement::Kind::kLine) {
          const int idx = resolve_line(s.grid_column_start, col_lines, true, explicit_cols);
          if (idx >= 0) {
            item.col_definite = true;
            item.col_start = idx;
          }
        } else if (s.grid_column_start.kind == style::GridPlacement::Kind::kSpan) {
          item.col_span = std::max(1, s.grid_column_start.span);
        }
        // Column end: an explicit end line makes the start definite when it
        // was auto ("auto / 4" → start = end − span), per §8.3.
        if (s.grid_column_end.kind == style::GridPlacement::Kind::kSpan) {
          if (!s.grid_column_end.name.empty() && item.col_definite) {
            const int end = resolve_span_named(s.grid_column_end, col_lines, item.col_start);
            if (end > item.col_start) {
              item.col_span = end - item.col_start;
            }
          } else {
            item.col_span = std::max(1, s.grid_column_end.span);
          }
        } else if (s.grid_column_end.kind == style::GridPlacement::Kind::kLine) {
          const int idx = resolve_line(s.grid_column_end, col_lines, true, explicit_cols);
          if (idx >= 0) {
            if (item.col_definite) {
              item.col_span = std::max(1, idx - item.col_start);
            } else {
              item.col_start = std::max(0, idx - item.col_span);
              item.col_definite = true;
              item.col_span = std::max(1, idx - item.col_start);
            }
          }
        }
        // Row start / end (symmetric).
        if (s.grid_row_start.kind == style::GridPlacement::Kind::kLine) {
          const int idx = resolve_line(s.grid_row_start, row_lines, false, explicit_rows);
          if (idx >= 0) {
            item.row_definite = true;
            item.row_start = idx;
          }
        } else if (s.grid_row_start.kind == style::GridPlacement::Kind::kSpan) {
          item.row_span = std::max(1, s.grid_row_start.span);
        }
        if (s.grid_row_end.kind == style::GridPlacement::Kind::kSpan) {
          if (!s.grid_row_end.name.empty() && item.row_definite) {
            const int end = resolve_span_named(s.grid_row_end, row_lines, item.row_start);
            if (end > item.row_start) {
              item.row_span = end - item.row_start;
            }
          } else {
            item.row_span = std::max(1, s.grid_row_end.span);
          }
        } else if (s.grid_row_end.kind == style::GridPlacement::Kind::kLine) {
          const int idx = resolve_line(s.grid_row_end, row_lines, false, explicit_rows);
          if (idx >= 0) {
            if (item.row_definite) {
              item.row_span = std::max(1, idx - item.row_start);
            } else {
              item.row_start = std::max(0, idx - item.row_span);
              item.row_definite = true;
              item.row_span = std::max(1, idx - item.row_start);
            }
          }
        }
        items.push_back(std::move(item));
      }
      if (items.empty()) {
        return 0.0f;
      }

      // ---- Auto placement (grid-auto-flow: row|column, sparse|dense) ----
      // occupied[r][c] marks a claimed cell; the grid grows in both axes as
      // items extend past the explicit template (implicit auto-sized tracks).
      std::vector<std::vector<bool>> occupied;
      auto ensure_cells = [&](int rows, int cols) {
        if (static_cast<int>(occupied.size()) < rows) {
          occupied.resize(static_cast<std::size_t>(rows));
        }
        for (auto& row : occupied) {
          if (static_cast<int>(row.size()) < cols) {
            row.resize(static_cast<std::size_t>(cols), false);
          }
        }
      };
      const auto cells_free = [&](int row, int col, int col_span, int row_span) {
        for (int r2 = row; r2 < row + row_span; ++r2) {
          for (int c = col; c < col + col_span; ++c) {
            if (occupied[static_cast<std::size_t>(r2)][static_cast<std::size_t>(c)]) {
              return false;
            }
          }
        }
        return true;
      };
      const auto claim_cells = [&](int row, int col, int col_span, int row_span) {
        ensure_cells(row + row_span, col + col_span);
        for (int r2 = row; r2 < row + row_span; ++r2) {
          for (int c = col; c < col + col_span; ++c) {
            occupied[static_cast<std::size_t>(r2)][static_cast<std::size_t>(c)] = true;
          }
        }
      };
      const auto place_item = [&](GridItem& item, int row, int col) {
        claim_cells(row, col, item.col_span, item.row_span);
        item.row_start = row;
        item.col_start = col;
      };

      const auto flow = cs.grid_auto_flow;
      const bool flow_column =
          flow == style::GridAutoFlow::kColumn || flow == style::GridAutoFlow::kColumnDense;
      const bool dense =
          flow == style::GridAutoFlow::kRowDense || flow == style::GridAutoFlow::kColumnDense;
      int grid_rows = 0;
      int grid_cols = std::max(1, explicit_cols);
      // Sparse-flow cursors: the next candidate cell for auto placement.
      int cursor_row = 0;
      int cursor_col = 0;
      for (GridItem& item : items) {
        const int col_span = std::max(1, item.col_span);
        const int row_span = std::max(1, item.row_span);
        bool placed = false;
        if (item.col_definite && item.row_definite) {
          place_item(item, item.row_start, item.col_start);
          placed = true;
        } else if (item.col_definite) {
          // Definite column: scan rows top-down at that column.  In row flow
          // the sparse cursor only moves forward (§8.5 step 2): moving to an
          // earlier column advances the row cursor past the previous
          // placement there.  (Column flow scans from the top as an
          // approximation of the per-column cursor.)
          const int col = item.col_start;
          const int row_from = (!flow_column && !dense && col < cursor_col) ? cursor_row + 1 : 0;
          for (int row = row_from; row <= grid_rows; ++row) {
            ensure_cells(row + row_span, col + col_span);
            if (cells_free(row, col, col_span, row_span)) {
              place_item(item, row, col);
              placed = true;
              break;
            }
          }
          if (!placed) {
            ensure_cells(grid_rows + row_span, col + col_span);
            place_item(item, grid_rows, col);
            placed = true;
          }
          cursor_col = col;
          cursor_row = item.row_start + row_span;
        } else if (item.row_definite) {
          // Definite row: scan columns left-to-right at that row.  In column
          // flow the sparse cursor only moves forward (same rule, axes
          // swapped); row flow scans from the start as an approximation of
          // the per-row cursor.
          const int row = item.row_start;
          const int col_from = (flow_column && !dense && row < cursor_row) ? cursor_col + 1 : 0;
          for (int col = col_from; col <= grid_cols; ++col) {
            ensure_cells(row + row_span, col + col_span);
            if (cells_free(row, col, col_span, row_span)) {
              place_item(item, row, col);
              placed = true;
              break;
            }
          }
          if (!placed) {
            ensure_cells(row + row_span, grid_cols + col_span);
            place_item(item, row, grid_cols);
            placed = true;
          }
          cursor_row = row;
          cursor_col = item.col_start + col_span;
        } else if (flow_column) {
          // Column flow: iterate columns left-to-right; inside each column
          // scan rows from the cursor (sparse) or the top (dense).  Rows are
          // bounded by the implicit grid's row count (explicit rows, grown
          // by placement and spans, §8.5 step 3 with axes swapped); columns
          // only extend the grid via the fallback below.
          for (int col = dense ? 0 : cursor_col; col <= grid_cols && !placed; ++col) {
            const int rows_bound = std::max({explicit_rows, grid_rows, row_span});
            const int row_from = dense ? 0 : (col == cursor_col ? cursor_row : 0);
            for (int row = row_from; row < rows_bound; ++row) {
              ensure_cells(row + row_span, col + col_span);
              if (cells_free(row, col, col_span, row_span)) {
                place_item(item, row, col);
                cursor_col = col;
                cursor_row = row + row_span;
                placed = true;
                break;
              }
            }
          }
          if (!placed) {
            // No existing column fits: open a new column below the grid's
            // right edge, taking its first row.
            ensure_cells(std::max(row_span, 1), grid_cols + col_span);
            place_item(item, 0, grid_cols);
            placed = true;
          }
        } else {
          // Row flow (initial): iterate rows top-down; inside each row scan
          // columns from the cursor (sparse) or the left edge (dense).  Rows
          // only extend the grid via the fallback below.
          for (int row = dense ? 0 : cursor_row; row <= grid_rows && !placed; ++row) {
            const int col_from = dense ? 0 : (row == cursor_row ? cursor_col : 0);
            for (int col = col_from; col + col_span <= grid_cols; ++col) {
              ensure_cells(row + row_span, col + col_span);
              if (cells_free(row, col, col_span, row_span)) {
                place_item(item, row, col);
                cursor_row = row;
                cursor_col = col + col_span;
                placed = true;
                break;
              }
            }
          }
          if (!placed) {
            // No existing row fits: open a new row at the grid's bottom
            // edge, starting at its first column.
            ensure_cells(grid_rows + row_span, std::max(grid_cols, col_span));
            place_item(item, grid_rows, 0);
            placed = true;
          }
        }
        grid_rows = std::max(grid_rows, item.row_start + row_span);
        grid_cols = std::max(grid_cols, item.col_start + col_span);
      }

      // ---- Column track sizing ----
      std::vector<float> col_sizes(static_cast<std::size_t>(grid_cols), 0.0f);
      // Phase 1: fixed tracks.
      for (int c = 0; c < grid_cols; ++c) {
        const style::GridTrack* t =
            c < template_cols ? &cs.grid_template_columns[static_cast<std::size_t>(c)] : nullptr;
        if (t != nullptr && t->kind == style::GridTrack::Kind::kFixed) {
          col_sizes[static_cast<std::size_t>(c)] =
              t->percent > 0 ? container_width * t->percent / 100.0f : t->length;
        }
      }
      // Phase 2: content tracks (auto/min-content/max-content and implicit
      // tracks) sized to the items starting in each column (a spanning item
      // contributes to its start column only — documented simplification).
      for (int c = 0; c < grid_cols; ++c) {
        const style::GridTrack* t =
            c < template_cols ? &cs.grid_template_columns[static_cast<std::size_t>(c)] : nullptr;
        style::GridTrack::Kind kind = t != nullptr ? t->kind : style::GridTrack::Kind::kAuto;
        if (kind == style::GridTrack::Kind::kFixed || kind == style::GridTrack::Kind::kFr) {
          continue;
        }
        for (const GridItem& item : items) {
          if (item.col_start != c) {
            continue;
          }
          const IntrinsicWidths w = MeasureContent(*item.element, styles, registry);
          col_sizes[static_cast<std::size_t>(c)] =
              std::max(col_sizes[static_cast<std::size_t>(c)],
                       kind == style::GridTrack::Kind::kMinContent ? w.min : w.max);
        }
      }
      // Phase 3: fr tracks share the leftover space.
      {
        float used = 0;
        for (int c = 0; c < grid_cols; ++c) {
          used += col_sizes[static_cast<std::size_t>(c)];
        }
        used += column_gap * static_cast<float>(std::max(0, grid_cols - 1));
        float total_fr = 0;
        for (int c = 0; c < grid_cols; ++c) {
          const style::GridTrack* t =
              c < template_cols ? &cs.grid_template_columns[static_cast<std::size_t>(c)] : nullptr;
          if (t != nullptr && t->kind == style::GridTrack::Kind::kFr) {
            total_fr += t->fr;
          }
        }
        if (total_fr > 0) {
          const float leftover = std::max(0.0f, container_width - used);
          for (int c = 0; c < grid_cols; ++c) {
            const style::GridTrack* t = c < template_cols
                                            ? &cs.grid_template_columns[static_cast<std::size_t>(c)]
                                            : nullptr;
            if (t != nullptr && t->kind == style::GridTrack::Kind::kFr) {
              col_sizes[static_cast<std::size_t>(c)] = leftover * t->fr / total_fr;
            }
          }
        }
      }

      // Phase 4: minmax() bounds clamp the resolved track size (CSS Grid
      // Layout 1 §5.1).  Content keywords inside minmax() clamp against the
      // min-content / max-content of the items starting in the track.
      const auto column_content_extent = [&](int c, bool max_measure) {
        float extent = 0;
        for (const GridItem& item : items) {
          if (item.col_start != c) {
            continue;
          }
          const IntrinsicWidths w = MeasureContent(*item.element, styles, registry);
          extent = std::max(extent, max_measure ? w.max : w.min);
        }
        return extent;
      };
      for (int c = 0; c < grid_cols; ++c) {
        const style::GridTrack* t =
            c < template_cols ? &cs.grid_template_columns[static_cast<std::size_t>(c)] : nullptr;
        if (t == nullptr) {
          continue;
        }
        float& size = col_sizes[static_cast<std::size_t>(c)];
        if (t->kind != style::GridTrack::Kind::kFr) {
          if (t->max_size.has_value()) {
            size = std::min(size, ResolveSize(t->max_size.value(), container_width));
          }
          if (t->max_is_max_content) {
            size = std::min(size, column_content_extent(c, /*max_measure=*/true));
          }
        }
        // The minimum always wins over the maximum (minmax(A,B) with A > B
        // resolves to A, §5.1).
        if (t->min_size.has_value()) {
          size = std::max(size, ResolveSize(t->min_size.value(), container_width));
        }
        if (t->min_is_min_content) {
          size = std::max(size, column_content_extent(c, /*max_measure=*/false));
        }
        if (t->min_is_max_content) {
          size = std::max(size, column_content_extent(c, /*max_measure=*/true));
        }
      }

      // The width of an item's grid area (its spanned columns plus gaps).
      const auto cell_width = [&](const GridItem& item) -> float {
        float w = 0;
        for (int c = item.col_start; c < item.col_start + item.col_span; ++c) {
          if (c > item.col_start) {
            w += column_gap;
          }
          w += col_sizes[static_cast<std::size_t>(c)];
        }
        return w;
      };

      // Build each item's box at the grid-area width (local origin; the box
      // is translated into place after the rows are sized).
      for (GridItem& item : items) {
        item.layout =
            BuildBlock(*item.element, cell_width(item), 0, 0, cb_x, cb_y, cb_w, cb_h, kNoFloats);
      }

      // ---- Row track sizing ----
      std::vector<float> row_sizes(static_cast<std::size_t>(grid_rows), 0.0f);
      for (int r = 0; r < grid_rows; ++r) {
        const style::GridTrack* t =
            r < template_rows ? &cs.grid_template_rows[static_cast<std::size_t>(r)] : nullptr;
        if (t != nullptr && t->kind == style::GridTrack::Kind::kFixed) {
          row_sizes[static_cast<std::size_t>(r)] = t->percent > 0 && height_definite
                                                       ? container_height * t->percent / 100.0f
                                                       : t->length;
        }
      }
      for (int r = 0; r < grid_rows; ++r) {
        const style::GridTrack* t =
            r < template_rows ? &cs.grid_template_rows[static_cast<std::size_t>(r)] : nullptr;
        style::GridTrack::Kind kind = t != nullptr ? t->kind : style::GridTrack::Kind::kAuto;
        if (kind == style::GridTrack::Kind::kFixed || kind == style::GridTrack::Kind::kFr) {
          continue;
        }
        for (const GridItem& item : items) {
          if (item.row_start != r) {
            continue;
          }
          const float outer =
              item.layout->margin_top + item.layout->height + item.layout->margin_bottom;
          row_sizes[static_cast<std::size_t>(r)] =
              std::max(row_sizes[static_cast<std::size_t>(r)], outer);
        }
      }
      if (height_definite) {
        float used = 0;
        for (int r = 0; r < grid_rows; ++r) {
          used += row_sizes[static_cast<std::size_t>(r)];
        }
        used += row_gap * static_cast<float>(std::max(0, grid_rows - 1));
        float total_fr = 0;
        for (int r = 0; r < grid_rows; ++r) {
          const style::GridTrack* t =
              r < template_rows ? &cs.grid_template_rows[static_cast<std::size_t>(r)] : nullptr;
          if (t != nullptr && t->kind == style::GridTrack::Kind::kFr) {
            total_fr += t->fr;
          }
        }
        if (total_fr > 0) {
          const float leftover = std::max(0.0f, container_height - used);
          for (int r = 0; r < grid_rows; ++r) {
            const style::GridTrack* t =
                r < template_rows ? &cs.grid_template_rows[static_cast<std::size_t>(r)] : nullptr;
            if (t != nullptr && t->kind == style::GridTrack::Kind::kFr) {
              row_sizes[static_cast<std::size_t>(r)] = leftover * t->fr / total_fr;
            }
          }
        }
      }

      // Row minmax() bounds (percent bounds resolve against the definite
      // container height; 0 otherwise, making them no-ops).
      const auto row_content_extent = [&](int r) {
        float extent = 0;
        for (const GridItem& item : items) {
          if (item.row_start != r) {
            continue;
          }
          extent = std::max(
              extent, item.layout->margin_top + item.layout->height + item.layout->margin_bottom);
        }
        return extent;
      };
      for (int r = 0; r < grid_rows; ++r) {
        const style::GridTrack* t =
            r < template_rows ? &cs.grid_template_rows[static_cast<std::size_t>(r)] : nullptr;
        if (t == nullptr) {
          continue;
        }
        float& size = row_sizes[static_cast<std::size_t>(r)];
        if (t->kind != style::GridTrack::Kind::kFr) {
          if (t->max_size.has_value()) {
            size = std::min(size, ResolveSize(t->max_size.value(), container_height));
          }
          if (t->max_is_max_content) {
            size = std::min(size, row_content_extent(r));
          }
        }
        // The minimum always wins (see the column clamp phase).
        if (t->min_size.has_value()) {
          size = std::max(size, ResolveSize(t->min_size.value(), container_height));
        }
        if (t->min_is_min_content || t->min_is_max_content) {
          size = std::max(size, row_content_extent(r));
        }
      }

      // ---- Track offsets and item placement ----
      std::vector<float> col_offsets(static_cast<std::size_t>(grid_cols), 0.0f);
      {
        float x = 0;
        for (int c = 0; c < grid_cols; ++c) {
          col_offsets[static_cast<std::size_t>(c)] = x;
          x += col_sizes[static_cast<std::size_t>(c)] + column_gap;
        }
      }
      std::vector<float> row_offsets(static_cast<std::size_t>(grid_rows), 0.0f);
      {
        float y = 0;
        for (int r = 0; r < grid_rows; ++r) {
          row_offsets[static_cast<std::size_t>(r)] = y;
          y += row_sizes[static_cast<std::size_t>(r)] + row_gap;
        }
      }
      for (GridItem& item : items) {
        const float cell_x = col_offsets[static_cast<std::size_t>(item.col_start)];
        const float cell_y = row_offsets[static_cast<std::size_t>(item.row_start)];
        // Default align-items is stretch (CSS Grid 1 §8.3.1 / Box Alignment 3):
        // an item with an auto block size fills its grid area's row span.  The
        // stretched border-box size REPLACES the natural size (the flex cross
        // stretch rule this engine already follows); content taller than the
        // area overflows, which is spec behaviour.  An explicit block size or
        // auto block margins disable stretch (auto margins absorb the space).
        if (!item.style->height.has_value() && !item.style->margin_top_auto &&
            !item.style->margin_bottom_auto) {
          float area_height = 0;
          for (int r = item.row_start; r < item.row_start + item.row_span; ++r) {
            if (r > item.row_start) {
              area_height += row_gap;
            }
            area_height += row_sizes[static_cast<std::size_t>(r)];
          }
          item.layout->height =
              std::max(0.0f, area_height - item.layout->margin_top - item.layout->margin_bottom);
        }
        TranslateBox(*item.layout, box.content_x() + cell_x, box.content_y() + cell_y);
        box.children.push_back(std::move(item.layout));
      }

      // Container content height: the stacked row tracks plus gaps.
      float content_height = 0;
      for (int r = 0; r < grid_rows; ++r) {
        content_height += row_sizes[static_cast<std::size_t>(r)];
        if (r + 1 < grid_rows) {
          content_height += row_gap;
        }
      }
      return content_height;
    }

