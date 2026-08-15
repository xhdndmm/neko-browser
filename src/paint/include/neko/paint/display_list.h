#pragma once

#include "neko/css/color.h"
#include "neko/style/computed_style.h"

#include <string>
#include <vector>

namespace neko::image {
struct Image;
}

namespace neko::paint {

// Retained drawing commands.  Coordinates are in the page's coordinate space
// (top-left origin, y down), matching layout output.
enum class CommandType
{
  kFillRect,
  kFillRoundRect,
  kBorderRect,
  kDrawText,
  kDrawImage,
  kPushClip, // clip subsequent commands to the rect until kPopClip
  kPopClip
};

struct DrawCommand
{
  CommandType type = CommandType::kFillRect;

  // Rectangle (border box for kFillRect / kFillRoundRect / kBorderRect).
  float x = 0;
  float y = 0;
  float width = 0;
  float height = 0;
  css::Color color{0, 0, 0, 255};

  // Corner radius (px) for kFillRoundRect.
  float radius = 0;

  // Border widths (top, right, bottom, left) for kBorderRect.
  float border_top = 0;
  float border_right = 0;
  float border_bottom = 0;
  float border_left = 0;

  // Text for kDrawText.
  std::string text;
  std::string font_family = "sans-serif"; // CSS font-family for glyph selection
  int font_weight = 400;
  bool font_italic = false;
  float font_size = 16;
  css::Color text_color{0, 0, 0, 255};
  bool underline = false;

  // Image for kDrawImage: drawn into (x, y, width, height) per |object_fit|.
  const image::Image* image = nullptr;
  style::ObjectFit object_fit = style::ObjectFit::kFill;
};

// A sequence of retained paint commands, built from the layout tree.
class DisplayList
{
public:
  void FillRect(float x, float y, float width, float height, css::Color color);
  void FillRoundRect(float x, float y, float width, float height, float radius, css::Color color);
  void BorderRect(float x,
                  float y,
                  float width,
                  float height,
                  float top,
                  float right,
                  float bottom,
                  float left,
                  css::Color color);
  void DrawText(float x,
                float y,
                std::string text,
                float font_size,
                css::Color color,
                bool underline = false,
                std::string font_family = "sans-serif",
                int font_weight = 400,
                bool font_italic = false);
  void DrawImage(float x,
                 float y,
                 float width,
                 float height,
                 const image::Image& image,
                 style::ObjectFit object_fit);
  // Pushes a clip rectangle (padding box) that restricts every following
  // command until the matching PopClip.  Clips nest; each is intersected
  // with the enclosing clip.
  void PushClip(float x, float y, float width, float height);
  void PopClip();

  const std::vector<DrawCommand>& commands() const
  {
    return commands_;
  }
  std::size_t size() const
  {
    return commands_.size();
  }

private:
  std::vector<DrawCommand> commands_;
};

} // namespace neko::paint
