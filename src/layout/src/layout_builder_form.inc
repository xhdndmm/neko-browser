    void SetFormControlState(LayoutBox& box, FormControlKind kind)
    {
      if (kind == FormControlKind::kNone) {
        return;
      }
      box.form_control = kind;
      box.form_control_checked = box.element->HasAttribute("checked");
      box.form_control_disabled = box.element->HasAttribute("disabled");
    }

    // Average and maximum character width of |style|'s primary font, in CSS
    // pixels -- the two numbers of §15.5.6's converting-a-character-width-to-
    // pixels algorithm.  The standard leaves the character set behind them to
    // the UA and browsers disagree widely (Firefox 156 measures a 13px average
    // for a 13.33px field font, ~1em); this engine measures the printable ASCII
    // range.  Without a font registry the monospace fallback model applies, so
    // both values are the font size.
    void
    AverageAndMaxCharWidth(const style::ComputedStyle& style, float& average, float& widest) const
    {
      if (registry == nullptr) {
        average = style.font_size;
        widest = style.font_size;
        return;
      }
      float total = 0;
      float widest_char = 0;
      int count = 0;
      for (int code = 0x20; code <= 0x7E; ++code) {
        const char c = static_cast<char>(code);
        const float width = MeasureTextWidth(registry,
                                             style.font_family,
                                             style.font_weight,
                                             style.font_italic,
                                             std::string_view(&c, 1),
                                             style.font_size);
        total += width;
        widest_char = std::max(widest_char, width);
        ++count;
      }
      average = total / static_cast<float>(count);
      widest = widest_char;
    }

    // Lays out a form control as an atomic widget box.  <input>, <textarea> and
    // <select> reach this path; ClassifyFormControl says which widget it is.
    // Geometry follows WHATWG HTML §15.5 Widgets: a text-entry control takes its
    // inline size from `size` with the converting-a-character-width-to-pixels
    // algorithm (§15.5.6), <textarea> from `cols`/`rows` (§15.5.17), <select>
    // from its option labels and display size (§15.5.16), and the remaining
    // widgets from the native metrics in form_control.h.  Returns nullptr for a
    // control that generates no box at all (input[type=hidden]).
    //
    // PARTIALLY IMPLEMENTED (Wave 2): the widget's content -- checkbox tick,
    // radio dot, slider track and thumb, colour swatch, file selector button,
    // drop-down arrow, and a textarea value containing line breaks -- is drawn
    // by the paint layer, which dispatches on LayoutBox::form_control.  A
    // textarea value is flattened to one line here because the engine treats
    // `white-space: pre-wrap` as `normal` (see computed_style.h).
    std::unique_ptr<LayoutBox> BuildFormControl(dom::Element& element, float containing_width)
    {
      const FormControlKind kind = ClassifyFormControl(element);
      // §15.3.10 gives input[type=hidden] appearance:none and the UA sheet
      // display:none, so it generates no box.  Classified in code as well,
      // because the `type` attribute is ASCII case-insensitive (the UA sheet's
      // selector is not): an upper-case TYPE=HIDDEN must not become a widget.
      if (kind == FormControlKind::kHidden || kind == FormControlKind::kNone) {
        return nullptr;
      }

      auto box = std::make_unique<LayoutBox>();
      box->element = &element;
      box->style = styles.StyleFor(element);
      ResolveBoxEdges(*box, containing_width);
      SetFormControlState(*box, kind);

      // Default widget chrome (border, background, padding) comes from the UA
      // stylesheet, so author declarations override it through the cascade.
      // It must not be re-applied here: `border: none` is a valid author
      // choice (baidu.com's search textarea), and a hardcoded default cannot
      // distinguish it from "not specified".

      const float border_padding_w =
          box->border_left + box->border_right + box->padding_left + box->padding_right;
      const float border_padding_h =
          box->border_top + box->border_bottom + box->padding_top + box->padding_bottom;
      // "One line high" for the one-line widgets (§15.5.6 requires a text
      // control's used line-height to be no smaller than `normal`), following
      // the engine's line box convention of 1.2 * font-size.
      const float line_h = std::max(1.0f, box->style.font_size * 1.2f);

      float content_width = 0;
      float content_height = line_h;
      // The text the control shows.  A widget that is drawn rather than written
      // in (checkbox, radio, file control) shows no text run: its content
      // belongs to the paint layer.  Range and colour well have no drawn
      // innards in this engine yet, so they show the sanitized value as a text
      // run (the slider/colour well chrome itself is still NOT IMPLEMENTED).
      std::string text;
      switch (kind) {
      case FormControlKind::kCheckbox:
      case FormControlKind::kRadio:
        // §15.5.10: a native box of its own size, with the tick / dot painted
        // inside it.  Stated as a border-box size (see form_control.h).
        content_width = std::max(0.0f, kCheckboxBoxSize - border_padding_w);
        content_height = std::max(0.0f, kCheckboxBoxSize - border_padding_h);
        break;
      case FormControlKind::kRange:
        // §15.5.8: an inline-block slider; the track and thumb are painted.
        content_width = std::max(0.0f, kRangeWidthEm * box->style.font_size - border_padding_w);
        content_height = std::max(0.0f, kRangeHeightEm * box->style.font_size - border_padding_h);
        // The value is clamped and step-aligned by the value sanitization
        // algorithm, so e.g. min=10 max=20 value=99 shows "20".
        text = html::InputValue(element);
        break;
      case FormControlKind::kColor:
        // §15.5.9: a colour well, painted from the element's value.
        content_width = std::max(0.0f, kColorWellWidth - border_padding_w);
        content_height = std::max(0.0f, kColorWellHeight - border_padding_h);
        // The sanitized simple colour ("#rrggbb", lowercase) is the visible
        // value until the colour well is painted.
        text = html::InputValue(element);
        break;
      case FormControlKind::kText: {
        float average = 0;
        float widest = 0;
        AverageAndMaxCharWidth(box->style, average, widest);
        // §15.5.6: (size - 1) * avg + max, with `size` defaulting to 20.
        const int characters = FieldSizeAttribute(element, "size").value_or(kDefaultFieldSizeChars);
        content_width = static_cast<float>(characters - 1) * average + widest;
        // The current (sanitized) value: an <input type=number value=abc>
        // reads as empty, a URL is trimmed, and so on.
        text = html::InputValue(element);
        // §15.5.6: a password state shows bullets instead of the value.
        if (base::AsciiEqualsIgnoreCase(std::string(element.GetAttribute("type").value_or("")),
                                        "password")) {
          text = MaskPasswordValue(text);
        }
        break;
      }
      case FormControlKind::kTextArea: {
        float average = 0;
        float widest = 0;
        AverageAndMaxCharWidth(box->style, average, widest);
        // §15.5.17: effective width = character width (cols) * avg + scrollbar
        // width; effective height = the block size of the `rows` lines.  The
        // engine renders no scrollbars, so the scrollbar term is zero.
        const int cols = FieldSizeAttribute(element, "cols").value_or(kDefaultTextAreaCols);
        const int rows = FieldSizeAttribute(element, "rows").value_or(kDefaultTextAreaRows);
        content_width = static_cast<float>(cols) * average;
        content_height = static_cast<float>(rows) * box->style.line_height;
        text = CollapseWhitespace(element.TextContent());
        break;
      }
      case FormControlKind::kSelect: {
        std::vector<const dom::Element*> options;
        CollectOptions(element, options);
        // §15.5.16: the inline size is the width of the select's labels.  The
        // drop-down arrow's area is reserved here and painted by the paint
        // layer -- the standard does not fix its size.
        float labels = 0;
        for (const dom::Element* option : options) {
          labels = std::max(labels,
                            MeasureTextWidth(registry,
                                             box->style.font_family,
                                             box->style.font_weight,
                                             box->style.font_italic,
                                             OptionLabel(*option),
                                             box->style.font_size));
        }
        content_width = labels + kSelectArrowWidthEm * box->style.font_size;
        const std::optional<int> display_size = FieldSizeAttribute(element, "size");
        // A list box shows `size` rows, or four when the attribute is absent or
        // invalid; a drop-down box is one row (the popup itself is NOT
        // IMPLEMENTED).  A `multiple` select is a list box, and this engine has
        // no multi-select popup, so it is one here even at a display size of 1.
        const int declared_size = display_size.value_or(kDefaultSelectDisplaySize);
        const bool list_box = declared_size > 1 || element.HasAttribute("multiple");
        const int rows =
            list_box ? display_size.value_or(kDefaultListBoxRows) : kDefaultSelectDisplaySize;
        content_height = static_cast<float>(rows) * box->style.line_height;
        if (const dom::Element* shown = SelectedOption(options); shown != nullptr) {
          text = OptionLabel(*shown);
        }
        break;
      }
      case FormControlKind::kButton:
        // §15.5.3 button layout: the box shrink-wraps its label.
        text = ButtonLabel(element);
        content_width = MeasureTextWidth(registry,
                                         box->style.font_family,
                                         box->style.font_weight,
                                         box->style.font_italic,
                                         text,
                                         box->style.font_size);
        break;
      case FormControlKind::kFile:
        // §15.5.11: the selected filename(s) followed by the file selector
        // button.  Nothing is selected without a file dialog, and the button
        // (its implementation-defined "Choose file" text) is painted, so no text
        // run is produced here.
        content_width = std::max(0.0f, kFileWidthEm * box->style.font_size - border_padding_w);
        break;
      case FormControlKind::kNone:
      case FormControlKind::kHidden:
        return nullptr; // unreachable: handled above
      }

      if (box->style.width.has_value()) {
        content_width = SpecToContent(
            box->style.width.value(), containing_width, border_padding_w, box->style.box_sizing);
      }
      box->width = std::max(0.0f, content_width) + border_padding_w;

      if (box->style.height.has_value() && !box->style.height.value().percent) {
        content_height = std::max(content_height,
                                  SpecToContent(box->style.height.value(),
                                                containing_width,
                                                border_padding_h,
                                                box->style.box_sizing));
      }
      if (box->style.min_height.has_value()) {
        const style::SizeSpec& spec = box->style.min_height.value();
        if (!spec.percent) {
          content_height = std::max(
              content_height,
              SpecToContent(spec, containing_width, border_padding_h, box->style.box_sizing));
        }
      }
      box->height = std::max(0.0f, content_height) + border_padding_h;
      box->x = box->margin_left;
      box->y = box->margin_top;

      // The hint text a text control shows while empty.
      bool is_placeholder = false;
      if ((kind == FormControlKind::kText || kind == FormControlKind::kTextArea) && text.empty()) {
        text = PlaceholderFor(element);
        is_placeholder = !text.empty();
      }
      // Text and textarea controls always get a run -- an empty value has a
      // zero-width run -- so the focused caret geometry (and therefore the
      // input method) exists before the first character is typed.  Without
      // the empty run an empty field had no caret and Qt kept the IME
      // disabled, making Chinese/Japanese/Korean input impossible.
      const bool caret_control =
          kind == FormControlKind::kText || kind == FormControlKind::kTextArea;
      if (!text.empty() || caret_control) {
        TextRun run;
        run.text = text;
        run.font_family = box->style.font_family;
        run.font_weight = box->style.font_weight;
        run.font_italic = box->style.font_italic;
        run.font_size = box->style.font_size;
        // The box origin carries its margins; content starts inside the
        // border+padding.  Run coordinates must be global (the box origin
        // offset is included), matching how the painter draws runs.
        run.x = box->x + box->border_left + box->padding_left;
        run.y = box->y + box->border_top + box->padding_top;
        run.color = is_placeholder ? css::Color{160, 160, 160, 255}
                                   : box->style.color.value_or(css::Color{0, 0, 0, 255});
        run.visible = box->style.visibility != style::Visibility::kHidden;
        run.width = MeasureTextWidth(
            registry, run.font_family, run.font_weight, run.font_italic, run.text, run.font_size);
        run.element = &element;
        Line line;
        line.height = line_h;
        line.runs.push_back(std::move(run));
        box->lines.push_back(std::move(line));
      }
      return box;
    }

