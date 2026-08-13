#include "neko/paint/display_list.h"

#include <string>
#include <utility>

#include "neko/image/image.h"

namespace neko::paint {

void DisplayList::FillRect(float x, float y, float width, float height, css::Color color) {
  DrawCommand command;
  command.type = CommandType::kFillRect;
  command.x = x;
  command.y = y;
  command.width = width;
  command.height = height;
  command.color = color;
  commands_.push_back(std::move(command));
}

void DisplayList::BorderRect(float x, float y, float width, float height, float top, float right,
                             float bottom, float left, css::Color color) {
  DrawCommand command;
  command.type = CommandType::kBorderRect;
  command.x = x;
  command.y = y;
  command.width = width;
  command.height = height;
  command.border_top = top;
  command.border_right = right;
  command.border_bottom = bottom;
  command.border_left = left;
  command.color = color;
  commands_.push_back(std::move(command));
}

void DisplayList::DrawText(float x, float y, std::string text, float font_size,
                           css::Color color, bool underline, std::string font_family,
                           int font_weight, bool font_italic) {
  DrawCommand command;
  command.type = CommandType::kDrawText;
  command.x = x;
  command.y = y;
  command.text = std::move(text);
  command.font_family = std::move(font_family);
  command.font_weight = font_weight;
  command.font_italic = font_italic;
  command.font_size = font_size;
  command.text_color = color;
  command.underline = underline;
  commands_.push_back(std::move(command));
}

void DisplayList::DrawImage(float x, float y, float width, float height,
                            const image::Image& image, style::ObjectFit object_fit) {
  DrawCommand command;
  command.type = CommandType::kDrawImage;
  command.x = x;
  command.y = y;
  command.width = width;
  command.height = height;
  command.image = &image;
  command.object_fit = object_fit;
  commands_.push_back(std::move(command));
}

}  // namespace neko::paint
