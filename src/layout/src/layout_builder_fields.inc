    const style::StyleEngine& styles;
    const graphics::FontRegistry* registry; // may be null (monospace fallback)
    const ImageProvider* images;            // may be null (no decoded images)
    // The initial containing block (the viewport), which position: fixed
    // boxes anchor to (CSS 2.2 §10.1).
    float viewport_width = 0;
    float viewport_height = 0;

    // Records on |box| which form control it is and its `checked` / `disabled`
    // state.  BuildFormControl does this for the controls whose geometry it
    // derives itself; the generic builders call it because <button> reaches them
    // instead (the UA sheet makes it an inline-block).
