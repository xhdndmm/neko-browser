  // Coordinates are absolute (viewport space).  BuildBlock lays out |element|
  // inside a containing block whose content box starts at |origin_x|/|origin_y|.
  struct Builder
  {
#include "layout_builder_fields.inc"
#include "layout_builder_form.inc"
#include "layout_builder_block.inc"
#include "layout_builder_flex.inc"
#include "layout_builder_grid.inc"
#include "layout_builder_table.inc"
  };
