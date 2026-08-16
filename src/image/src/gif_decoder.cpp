// GIF (87a/89a) decoder, implemented in-house.
//
// Supports: header, logical screen descriptor, global/local color tables,
// LZW decompression (variable code width, sub-block data), interlace, graphic
// control extension (transparency + disposal), comment/application/plain-text
// extensions (skipped).  The first frame is composited onto the logical
// screen and returned as a single RGBA image; animation is not yet supported.
//
// All parsing is bounds-checked: untrusted input must not be able to trigger
// out-of-bounds reads or unbounded allocation.

#include "neko/base/status.h"
#include "neko/image/image.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace neko::image {
namespace {

struct ColorTable
{
  std::array<uint8_t, 256 * 3> rgb{}; // 256 entries max (3 bytes each)
  int size = 0;                       // number of valid entries
};

// A bounds-checked reader over the whole GIF buffer.
class Reader
{
public:
  explicit Reader(std::string_view data) : data_(data) {}

  bool BytesLeft(std::size_t n) const
  {
    return pos_ + n <= data_.size();
  }

  // Reads a single byte; advances on success.
  bool ReadU8(uint8_t& out)
  {
    if (pos_ >= data_.size()) {
      return false;
    }
    out = static_cast<uint8_t>(data_[pos_++]);
    return true;
  }

  // Reads a little-endian 16-bit value.
  bool ReadU16LE(uint16_t& out)
  {
    if (!BytesLeft(2)) {
      return false;
    }
    out = static_cast<uint16_t>(static_cast<uint8_t>(data_[pos_])) |
          static_cast<uint16_t>(static_cast<uint8_t>(data_[pos_ + 1]) << 8);
    pos_ += 2;
    return true;
  }

  // Skips |n| bytes.
  bool Skip(std::size_t n)
  {
    if (!BytesLeft(n)) {
      return false;
    }
    pos_ += n;
    return true;
  }

  std::size_t pos() const
  {
    return pos_;
  }

private:
  std::string_view data_;
  std::size_t pos_ = 0;
};

// Reads a color table of |size| entries from the reader.
bool ReadColorTable(Reader& r, int size, ColorTable& table)
{
  if (size < 0 || size > 256) {
    return false;
  }
  table.size = size;
  if (!r.BytesLeft(static_cast<std::size_t>(size) * 3)) {
    return false;
  }
  for (int i = 0; i < size; ++i) {
    uint8_t red = 0, green = 0, blue = 0;
    if (!r.ReadU8(red) || !r.ReadU8(green) || !r.ReadU8(blue)) {
      return false;
    }
    const std::size_t base = static_cast<std::size_t>(i) * 3;
    table.rgb[base + 0] = red;
    table.rgb[base + 1] = green;
    table.rgb[base + 2] = blue;
  }
  return true;
}

// Number of color table entries from the packed field's size bits.
int TableSizeFromBits(uint8_t packed)
{
  return 2 << (packed & 0x07);
}

// Reads one sub-block of data; returns its contents.  A zero-length block
// ends the sub-block stream.
bool ReadSubBlock(Reader& r, std::vector<uint8_t>& out)
{
  uint8_t size = 0;
  if (!r.ReadU8(size)) {
    return false;
  }
  if (size == 0) {
    out.clear();
    return true; // terminator
  }
  if (!r.BytesLeft(size)) {
    return false;
  }
  out.resize(size);
  for (uint8_t& b : out) {
    if (!r.ReadU8(b)) {
      return false;
    }
  }
  return true;
}

// Reads and concatenates all sub-blocks following an image descriptor.
bool ReadAllSubBlocks(Reader& r, std::vector<uint8_t>& out)
{
  out.clear();
  for (;;) {
    std::vector<uint8_t> block;
    if (!ReadSubBlock(r, block)) {
      return false;
    }
    if (block.empty()) {
      return true; // terminator reached
    }
    out.insert(out.end(), block.begin(), block.end());
    if (out.size() > 64u * 1024u * 1024u) {
      return false; // unbounded sub-block data
    }
  }
}

// A bit reader over LZW code data (GIF packs codes least-significant-bit
// first).
class BitReader
{
public:
  explicit BitReader(const std::vector<uint8_t>& data) : data_(data) {}

  // Reads a |width|-bit code; returns -1 on end of data.
  int ReadCode(int width)
  {
    if (width <= 0 || width > 12 || bit_pos_ + static_cast<std::size_t>(width) > data_.size() * 8) {
      return -1;
    }
    int code = 0;
    for (int i = 0; i < width; ++i) {
      const std::size_t bit = bit_pos_ + static_cast<std::size_t>(i);
      const uint8_t byte = data_[bit / 8];
      const int value = (byte >> (bit % 8)) & 1;
      code |= value << i;
    }
    bit_pos_ += static_cast<std::size_t>(width);
    return code;
  }

private:
  const std::vector<uint8_t>& data_;
  std::size_t bit_pos_ = 0;
};

// Decompresses a GIF LZW stream (|min_code_size| byte already read).
// Stops with false once |max_output| bytes would be emitted, so a crafted
// stream cannot balloon memory before the caller validates the length.
// Returns false on any malformed input.
bool LzwDecompress(const std::vector<uint8_t>& data,
                   int min_code_size,
                   std::size_t max_output,
                   std::vector<uint8_t>& out)
{
  if (min_code_size < 2 || min_code_size > 8) {
    return false;
  }
  const int clear_code = 1 << min_code_size;
  const int end_code = clear_code + 1;
  int code_size = min_code_size + 1;
  int next_code = end_code + 1;

  // Dictionary of strings (as index sequences).  Codes up to 12 bits (4096).
  std::vector<std::vector<uint8_t>> dict;
  dict.reserve(4096u);
  for (int i = 0; i < clear_code; ++i) {
    dict.push_back({static_cast<uint8_t>(i)});
  }
  dict.push_back({}); // clear_code (unused as an entry)
  dict.push_back({}); // end_code (unused as an entry)

  BitReader bits(data);
  int prev = -1;
  bool seen_data = false;
  for (;;) {
    const int code = bits.ReadCode(code_size);
    if (code < 0) {
      // Truncated stream: only valid if we already saw the end code.
      return false;
    }
    if (code == clear_code) {
      // Reset the dictionary: a clear code starts a fresh LZW dictionary.
      // Keeping the old entries would make subsequent codes hit stale
      // strings from the previous generation and corrupt the output (and
      // allow unbounded dictionary growth from clear-code bombing).
      dict.clear();
      dict.reserve(4096u);
      for (int i = 0; i < clear_code; ++i) {
        dict.push_back({static_cast<uint8_t>(i)});
      }
      dict.push_back({}); // clear_code (unused as an entry)
      dict.push_back({}); // end_code (unused as an entry)
      code_size = min_code_size + 1;
      next_code = end_code + 1;
      prev = -1;
      continue;
    }
    if (code == end_code) {
      return seen_data; // normal termination
    }
    if (prev < 0) {
      // First data code after (re)set: must be a root color.
      if (code >= clear_code) {
        return false;
      }
      if (out.size() >= max_output) {
        return false;
      }
      out.push_back(static_cast<uint8_t>(code));
      prev = code;
      seen_data = true;
      continue;
    }
    // Build the entry for |code|.
    std::vector<uint8_t> entry;
    if (code < static_cast<int>(dict.size())) {
      entry = dict[static_cast<std::size_t>(code)];
    } else if (code == next_code) {
      // KwKwK case: the entry is prev + first(prev).
      entry = dict[static_cast<std::size_t>(prev)];
      entry.push_back(entry.front());
    } else {
      return false; // code out of range
    }
    if (entry.empty()) {
      return false;
    }
    if (entry.size() > max_output - out.size()) {
      return false; // would exceed the output bound
    }
    out.insert(out.end(), entry.begin(), entry.end());
    // Add prev + first(entry) to the dictionary.
    if (next_code < 4096) {
      std::vector<uint8_t> new_entry = dict[static_cast<std::size_t>(prev)];
      new_entry.push_back(entry.front());
      dict.push_back(std::move(new_entry));
      ++next_code;
      if (next_code == (1 << code_size) && code_size < 12) {
        ++code_size;
      }
    }
    prev = code;
    seen_data = true;
  }
}

// Maps a pixel index through a color table into RGBA.
void IndexToRgba(const ColorTable& table,
                 uint8_t index,
                 bool transparent,
                 uint8_t transparent_index,
                 uint8_t* rgba)
{
  if (transparent && index == transparent_index) {
    rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0; // fully transparent
    return;
  }
  const int i = static_cast<int>(index) * 3;
  rgba[0] = table.rgb[static_cast<std::size_t>(i) + 0];
  rgba[1] = table.rgb[static_cast<std::size_t>(i) + 1];
  rgba[2] = table.rgb[static_cast<std::size_t>(i) + 2];
  rgba[3] = 255;
}

// Maps an interlace row order index to the actual scanline.
int DeinterlaceRow(int row, int height)
{
  if (row < 0 || height <= 0) {
    return row;
  }
  // Pass 1: rows 0, 8, 16, ...; Pass 2: 4, 12, ...; Pass 3: 2, 6, ...;
  // Pass 4: 1, 3, 5, ...
  int p1 = (height + 7) / 8;
  if (row < p1)
    return row * 8;
  int p2 = (height + 3) / 8;
  if (row < p1 + p2)
    return 4 + (row - p1) * 8;
  int p3 = (height + 1) / 4;
  if (row < p1 + p2 + p3)
    return 2 + (row - p1 - p2) * 4;
  return 1 + (row - p1 - p2 - p3) * 2;
}

} // namespace

bool IsGif(std::string_view data)
{
  return data.size() >= 6 && (data.substr(0, 6) == "GIF87a" || data.substr(0, 6) == "GIF89a");
}

base::Result<Image> DecodeGif(std::string_view data)
{
  if (!IsGif(data)) {
    return base::Err(base::Error::Parse("gif: bad magic"));
  }
  Reader r(data);
  r.Skip(6); // "GIF87a"/"GIF89a"

  uint16_t screen_width = 0, screen_height = 0;
  uint8_t packed = 0, background_index = 0, aspect = 0;
  if (!r.ReadU16LE(screen_width) || !r.ReadU16LE(screen_height) || !r.ReadU8(packed) ||
      !r.ReadU8(background_index) || !r.ReadU8(aspect)) {
    return base::Err(base::Error::Parse("gif: truncated logical screen descriptor"));
  }
  (void)aspect; // pixel aspect ratio is not used
  if (screen_width == 0 || screen_height == 0) {
    return base::Err(base::Error::Parse("gif: invalid logical screen size"));
  }

  ColorTable global_table;
  const bool has_global = (packed & 0x80) != 0;
  if (has_global) {
    if (!ReadColorTable(r, TableSizeFromBits(packed), global_table)) {
      return base::Err(base::Error::Parse("gif: truncated global color table"));
    }
  }

  // The canvas is initialized to the background color (or transparent when
  // the GIF has no usable background).
  Image out;
  out.width = screen_width;
  out.height = screen_height;
  out.rgba.assign(static_cast<std::size_t>(screen_width) * screen_height * 4, 0);
  if (has_global && background_index < global_table.size) {
    const int i = static_cast<int>(background_index) * 3;
    for (std::size_t p = 0; p < out.rgba.size(); p += 4) {
      out.rgba[p + 0] = global_table.rgb[static_cast<std::size_t>(i) + 0];
      out.rgba[p + 1] = global_table.rgb[static_cast<std::size_t>(i) + 1];
      out.rgba[p + 2] = global_table.rgb[static_cast<std::size_t>(i) + 2];
      out.rgba[p + 3] = 255;
    }
  }

  // Current graphic control extension state (applies to the next frame).
  bool transparent = false;
  uint8_t transparent_index = 0;
  uint8_t disposal = 0;
  bool saw_frame = false;

  for (;;) {
    uint8_t block = 0;
    if (!r.ReadU8(block)) {
      if (saw_frame) {
        break; // EOF after a complete frame: accept.
      }
      return base::Err(base::Error::Parse("gif: truncated file"));
    }
    if (block == 0x3B) {
      break; // trailer
    }
    if (block == 0x21) {
      // Extension.
      uint8_t label = 0;
      if (!r.ReadU8(label)) {
        return base::Err(base::Error::Parse("gif: truncated extension"));
      }
      if (label == 0xF9) {
        // Graphic control extension.
        uint8_t size = 0;
        uint8_t g_packed = 0;
        uint16_t delay = 0;
        uint8_t t_index = 0;
        uint8_t terminator = 0;
        if (!r.ReadU8(size) || size != 4 || !r.ReadU8(g_packed) || !r.ReadU16LE(delay) ||
            !r.ReadU8(t_index) || !r.ReadU8(terminator)) {
          return base::Err(base::Error::Parse("gif: malformed graphic control extension"));
        }
        disposal = static_cast<uint8_t>((g_packed >> 2) & 0x07);
        transparent = (g_packed & 0x01) != 0;
        transparent_index = t_index;
        (void)delay; // animation timing is not supported yet
      } else {
        // Comment / plain-text / application extension: skip sub-blocks.
        std::vector<uint8_t> ignored;
        if (!ReadAllSubBlocks(r, ignored)) {
          return base::Err(base::Error::Parse("gif: malformed extension data"));
        }
      }
      continue;
    }
    if (block != 0x2C) {
      return base::Err(base::Error::Parse("gif: unknown block"));
    }

    // Image descriptor.
    uint16_t left = 0, top = 0, width = 0, height = 0;
    uint8_t i_packed = 0;
    if (!r.ReadU16LE(left) || !r.ReadU16LE(top) || !r.ReadU16LE(width) || !r.ReadU16LE(height) ||
        !r.ReadU8(i_packed)) {
      return base::Err(base::Error::Parse("gif: truncated image descriptor"));
    }
    if (width == 0 || height == 0) {
      return base::Err(base::Error::Parse("gif: zero-size image"));
    }
    const int right = static_cast<int>(left) + static_cast<int>(width);
    const int bottom = static_cast<int>(top) + static_cast<int>(height);
    if (right > screen_width || bottom > screen_height) {
      return base::Err(base::Error::Parse("gif: image exceeds logical screen"));
    }

    // Local color table overrides the global one for this frame.
    ColorTable table = global_table;
    if ((i_packed & 0x80) != 0) {
      if (!ReadColorTable(r, TableSizeFromBits(i_packed), table)) {
        return base::Err(base::Error::Parse("gif: truncated local color table"));
      }
    }
    const bool interlace = (i_packed & 0x40) != 0;

    // LZW min code size + sub-block data.
    uint8_t min_code_size = 0;
    if (!r.ReadU8(min_code_size)) {
      return base::Err(base::Error::Parse("gif: missing LZW code size"));
    }
    std::vector<uint8_t> lzw_data;
    if (!ReadAllSubBlocks(r, lzw_data)) {
      return base::Err(base::Error::Parse("gif: malformed image data"));
    }
    std::vector<uint8_t> pixels;
    const std::size_t expected = static_cast<std::size_t>(width) * height;
    // Cap decompression at the expected size plus a small slack so a crafted
    // stream cannot allocate unbounded memory before the length check below.
    const std::size_t max_output = expected > (1u << 20) ? expected * 2 : expected + (1u << 20);
    if (!LzwDecompress(lzw_data, min_code_size, max_output, pixels)) {
      return base::Err(base::Error::Parse("gif: invalid LZW stream"));
    }
    if (pixels.size() < expected) {
      return base::Err(base::Error::Parse("gif: LZW output too short"));
    }

    // Composite this frame onto the canvas.
    for (int row = 0; row < height; ++row) {
      const int dst_row = interlace ? DeinterlaceRow(row, height) : row;
      if (dst_row < 0 || dst_row >= screen_height) {
        return base::Err(base::Error::Parse("gif: bad interlace row"));
      }
      const std::size_t dst_base =
          (static_cast<std::size_t>(top) + static_cast<std::size_t>(dst_row)) *
              static_cast<std::size_t>(screen_width) * 4 +
          static_cast<std::size_t>(left) * 4;
      const std::size_t src_base = static_cast<std::size_t>(row) * width;
      for (int col = 0; col < width; ++col) {
        const uint8_t index = pixels[src_base + static_cast<std::size_t>(col)];
        if (transparent && index == transparent_index) {
          continue; // leave canvas as-is
        }
        if (static_cast<int>(index) >= table.size) {
          return base::Err(base::Error::Parse("gif: pixel index out of color table"));
        }
        IndexToRgba(table,
                    index,
                    /*transparent=*/false,
                    0,
                    &out.rgba[dst_base + static_cast<std::size_t>(col) * 4]);
      }
    }
    saw_frame = true;
    // Only the first frame is rendered (animation is not supported); a
    // disposal of "restore to previous" (3) for later frames is irrelevant.
    (void)disposal;
    break;
  }

  if (!saw_frame) {
    return base::Err(base::Error::Parse("gif: no image data"));
  }
  return out;
}

} // namespace neko::image
