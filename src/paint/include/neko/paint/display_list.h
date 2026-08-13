#pragma once

#include <string>
#include <vector>

#include "neko/css/color.h"

namespace neko::paint {

// Retained drawing commands.  Coordinates are in the page's coordinate space
// (top-left origin, y down), matching layout output.
enum class CommandType { kFillRect, kBorderRect, kDrawText };

struct DrawCommand {
  CommandType type = CommandType::kFillRect;

  // Rectangle (border box for kFillRect / kBorderRect).
  float x = 0;
  float y = 0;
  float width = 0;
  float height = 0;
  css::Color color{0, 0, 0, 255};

  // Border widths (top, right, bottom, left) for kBorderRect.
  float border_top = 0;
  float border_right = 0;
  float border_bottom = 0;
  float border_left = 0;

  // Text for kDrawText.
  std::string text;
  float font_size = 16;
  css::Color text_color{0, 0, 0, 255};
};

// A sequence of retained paint commands, built from the layout tree.
class DisplayList {
 public:
  void FillRect(float x, float y, float width, float height, css::Color color);
  void BorderRect(float x, float y, float width, float height, float top, float right,
                  float bottom, float left, css::Color color);
  void DrawText(float x, float y, std::string text, float font_size, css::Color color);

  const std::vector<DrawCommand>& commands() const { return commands_; }
  std::size_t size() const { return commands_.size(); }

 private:
  std::vector<DrawCommand> commands_;
};

}  // namespace neko::paint
