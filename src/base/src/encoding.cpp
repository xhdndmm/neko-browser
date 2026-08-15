#include "neko/base/encoding.h"

#include "encoding_data.inc"
#include "encoding_labels.inc"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neko::base::encoding {
namespace {

constexpr std::uint32_t kReplacementCp = 0xFFFD;

// ---------------------------------------------------------------------------
// UTF-8 output helpers
// ---------------------------------------------------------------------------

void AppendUtf8(std::string& out, std::uint32_t cp)
{
  if (cp < 0x80U) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800U) {
    out.push_back(static_cast<char>(0xC0U | (cp >> 6)));
    out.push_back(static_cast<char>(0x80U | (cp & 0x3F)));
  } else if (cp < 0x10000U) {
    out.push_back(static_cast<char>(0xE0U | (cp >> 12)));
    out.push_back(static_cast<char>(0x80U | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80U | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0U | (cp >> 18)));
    out.push_back(static_cast<char>(0x80U | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80U | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80U | (cp & 0x3F)));
  }
}

// Scalar value from a valid surrogate pair.
std::uint32_t FromSurrogates(std::uint16_t lead, std::uint16_t trail)
{
  return 0x10000U + ((static_cast<std::uint32_t>(lead) - 0xD800U) << 10) +
         (static_cast<std::uint32_t>(trail) - 0xDC00U);
}

bool IsAsciiByte(int b)
{
  return b >= 0 && b <= 0x7F;
}

bool IsAsciiWhitespace(char c)
{
  return c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ';
}

bool IsAsciiAlpha(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

char AsciiLower(char c)
{
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 0x20) : c;
}

// A byte stream reader with a small LIFO of "restored" (unread) bytes, which
// implements the I/O queue "restore" operation used by the WHATWG decoders.
// Next() returns -1 at end-of-queue.
class ByteReader
{
public:
  explicit ByteReader(std::string_view data) : data_(data) {}

  int Next()
  {
    if (!restored_.empty()) {
      const int b = restored_.back();
      restored_.pop_back();
      return b;
    }
    if (pos_ < data_.size()) {
      return static_cast<unsigned char>(data_[pos_++]);
    }
    return -1;
  }

  // Re-inserts bytes so they are read again in the given order.
  void Restore(std::initializer_list<std::uint8_t> bytes)
  {
    for (auto it = bytes.end(); it != bytes.begin();) {
      --it;
      restored_.push_back(*it);
    }
  }

private:
  std::string_view data_;
  std::size_t pos_ = 0;
  std::vector<std::uint8_t> restored_;
};

// ---------------------------------------------------------------------------
// WHATWG UTF-8 decoder (replacement error mode).
// ---------------------------------------------------------------------------
void DecodeUtf8(std::string_view bytes, std::string& out)
{
  std::size_t i = 0;
  while (i < bytes.size()) {
    const unsigned b0 = static_cast<unsigned char>(bytes[i]);
    if (b0 < 0x80) {
      out.push_back(static_cast<char>(b0));
      ++i;
      continue;
    }
    unsigned needed = 0;
    std::uint32_t cp = 0;
    unsigned lower = 0x80;
    unsigned upper = 0xBF;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
      needed = 1;
      cp = b0 & 0x1F;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
      if (b0 == 0xE0) {
        lower = 0xA0;
      }
      if (b0 == 0xED) {
        upper = 0x9F;
      }
      needed = 2;
      cp = b0 & 0x0F;
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
      if (b0 == 0xF0) {
        lower = 0x90;
      }
      if (b0 == 0xF4) {
        upper = 0x8F;
      }
      needed = 3;
      cp = b0 & 0x07;
    } else {
      AppendUtf8(out, kReplacementCp);
      ++i;
      continue;
    }

    // Consume continuation bytes (WHATWG UTF-8 decoder).
    std::size_t pos = i + 1;
    bool invalid = false;
    unsigned seen = 0;
    while (pos < bytes.size() && seen < needed) {
      const unsigned bc = static_cast<unsigned char>(bytes[pos]);
      if (seen == 0 && (bc < lower || bc > upper)) {
        invalid = true;
        break;
      }
      if (seen > 0 && (bc < 0x80 || bc > 0xBF)) {
        invalid = true;
        break;
      }
      cp = (cp << 6) | (bc & 0x3F);
      ++seen;
      ++pos;
    }
    if (seen < needed && !invalid) {
      // Truncated at end of stream: a single error; consume the remainder.
      AppendUtf8(out, kReplacementCp);
      break;
    }
    if (invalid) {
      // Error with the offending byte restored (reprocessed as a new lead).
      AppendUtf8(out, kReplacementCp);
      ++i;
      continue;
    }
    AppendUtf8(out, cp);
    i = pos;
  }
}

// ---------------------------------------------------------------------------
// WHATWG shared UTF-16 decoder.
// ---------------------------------------------------------------------------
void DecodeUtf16(std::string_view bytes, std::string& out, bool big_endian)
{
  ByteReader reader(bytes);
  std::optional<std::uint8_t> leading_byte;
  std::optional<std::uint16_t> leading_surrogate;
  for (int b = reader.Next(); b != -1; b = reader.Next()) {
    if (!leading_byte.has_value()) {
      leading_byte = static_cast<std::uint8_t>(b);
      continue;
    }
    const std::uint8_t first = *leading_byte;
    leading_byte.reset();
    const std::uint16_t unit = big_endian ? static_cast<std::uint16_t>((first << 8) | b)
                                          : static_cast<std::uint16_t>((b << 8) | first);
    if (leading_surrogate.has_value()) {
      const std::uint16_t lead = *leading_surrogate;
      leading_surrogate.reset();
      if (unit >= 0xDC00 && unit <= 0xDFFF) {
        AppendUtf8(out, FromSurrogates(lead, unit));
        continue;
      }
      // Not a trailing surrogate: restore the two bytes and error.
      AppendUtf8(out, kReplacementCp);
      if (big_endian) {
        reader.Restore(
            {static_cast<std::uint8_t>(unit >> 8), static_cast<std::uint8_t>(unit & 0xFF)});
      } else {
        reader.Restore(
            {static_cast<std::uint8_t>(unit & 0xFF), static_cast<std::uint8_t>(unit >> 8)});
      }
      continue;
    }
    if (unit >= 0xD800 && unit <= 0xDBFF) {
      leading_surrogate = unit;
      continue;
    }
    if (unit >= 0xDC00 && unit <= 0xDFFF) {
      AppendUtf8(out, kReplacementCp);
      continue;
    }
    AppendUtf8(out, unit);
  }
  if (leading_byte.has_value() || leading_surrogate.has_value()) {
    AppendUtf8(out, kReplacementCp);
  }
}

// ---------------------------------------------------------------------------
// WHATWG single-byte decoder (index single-byte).
// ---------------------------------------------------------------------------
void DecodeSingleByte(std::string_view bytes, std::string& out, const std::uint16_t* table)
{
  for (const char c : bytes) {
    const unsigned b = static_cast<unsigned char>(c);
    if (b < 0x80) {
      out.push_back(static_cast<char>(b));
    } else {
      const std::uint16_t cp = table[b - 0x80];
      if (cp == 0) {
        AppendUtf8(out, kReplacementCp);
      } else {
        AppendUtf8(out, cp);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// WHATWG gb18030 decoder (also used for GBK).
// ---------------------------------------------------------------------------
// Index gb18030 ranges code point (WHATWG Encoding 5): the ranges file maps
// the four-byte pointer space in 207 contiguous ranges.
std::optional<std::uint32_t> Gb18030RangesCodePoint(std::uint32_t pointer)
{
  if ((pointer > 39419 && pointer < 189000) || pointer > 1237575) {
    return std::nullopt;
  }
  if (pointer == 7457) {
    return 0xE7C7;
  }
  // Last range entry with entry.pointer <= pointer.
  const auto& ranges = detail::kGb18030Ranges;
  auto it =
      std::upper_bound(ranges,
                       ranges + detail::kGb18030RangesCount,
                       pointer,
                       [](std::uint32_t p, const detail::RangeEntry& e) { return p < e.pointer; });
  if (it == ranges) {
    return std::nullopt;
  }
  --it;
  return static_cast<std::uint32_t>(it->code_point) + pointer - it->pointer;
}

void DecodeGb18030(std::string_view bytes, std::string& out)
{
  ByteReader reader(bytes);
  std::uint8_t first = 0;
  std::uint8_t second = 0;
  std::uint8_t third = 0;
  for (int b = reader.Next(); b != -1; b = reader.Next()) {
    if (third != 0) {
      if (b >= 0x30 && b <= 0x39) {
        const std::uint32_t pointer =
            (static_cast<std::uint32_t>(first - 0x81) * (10U * 126U * 10U)) +
            (static_cast<std::uint32_t>(second - 0x30) * (10U * 126U)) +
            (static_cast<std::uint32_t>(third - 0x81) * 10U) + static_cast<std::uint32_t>(b) - 0x30;
        first = second = third = 0;
        const auto cp = Gb18030RangesCodePoint(pointer);
        if (cp.has_value()) {
          AppendUtf8(out, *cp);
        } else {
          AppendUtf8(out, kReplacementCp);
        }
      } else {
        AppendUtf8(out, kReplacementCp);
        reader.Restore({second, third, static_cast<std::uint8_t>(b)});
        first = second = third = 0;
      }
      continue;
    }
    if (second != 0) {
      if (b >= 0x81 && b <= 0xFE) {
        third = static_cast<std::uint8_t>(b);
        continue;
      }
      AppendUtf8(out, kReplacementCp);
      reader.Restore({second, static_cast<std::uint8_t>(b)});
      first = second = 0;
      continue;
    }
    if (first != 0) {
      if (b >= 0x30 && b <= 0x39) {
        second = static_cast<std::uint8_t>(b);
        continue;
      }
      const std::uint8_t leading = first;
      first = 0;
      const std::uint8_t offset = b < 0x7F ? 0x40 : 0x41;
      std::uint16_t cp = 0;
      if ((b >= 0x40 && b <= 0x7E) || (b >= 0x80 && b <= 0xFE)) {
        const std::size_t pointer = static_cast<std::size_t>(leading - 0x81) * 190U +
                                    static_cast<std::uint32_t>(b) - offset;
        cp = detail::kGb18030[pointer];
      }
      if (cp != 0) {
        AppendUtf8(out, cp);
      } else {
        if (IsAsciiByte(b)) {
          reader.Restore({static_cast<std::uint8_t>(b)});
        }
        AppendUtf8(out, kReplacementCp);
      }
      continue;
    }
    if (IsAsciiByte(b)) {
      out.push_back(static_cast<char>(b));
    } else if (b == 0x80) {
      AppendUtf8(out, 0x20AC); // EURO SIGN
    } else if (b >= 0x81 && b <= 0xFE) {
      first = static_cast<std::uint8_t>(b);
    } else {
      AppendUtf8(out, kReplacementCp);
    }
  }
  if (first != 0 || second != 0 || third != 0) {
    AppendUtf8(out, kReplacementCp);
  }
}

// ---------------------------------------------------------------------------
// WHATWG Big5 decoder.
// ---------------------------------------------------------------------------
void DecodeBig5(std::string_view bytes, std::string& out)
{
  ByteReader reader(bytes);
  std::uint8_t leading = 0;
  for (int b = reader.Next(); b != -1; b = reader.Next()) {
    if (leading != 0) {
      const std::uint8_t lead = leading;
      leading = 0;
      const std::uint8_t offset = b < 0x7F ? 0x40 : 0x62;
      std::uint32_t cp = 0;
      if ((b >= 0x40 && b <= 0x7E) || (b >= 0xA1 && b <= 0xFE)) {
        const std::size_t pointer =
            static_cast<std::size_t>(lead - 0x81) * 157U + static_cast<std::uint32_t>(b) - offset;
        // Two code points for four legacy pointers (WHATWG Big5 decoder).
        switch (pointer) {
        case 1133:
          AppendUtf8(out, 0x00CA);
          AppendUtf8(out, 0x0304);
          continue;
        case 1135:
          AppendUtf8(out, 0x00CA);
          AppendUtf8(out, 0x030C);
          continue;
        case 1164:
          AppendUtf8(out, 0x00EA);
          AppendUtf8(out, 0x0304);
          continue;
        case 1166:
          AppendUtf8(out, 0x00EA);
          AppendUtf8(out, 0x030C);
          continue;
        default:
          cp = detail::kBig5[pointer];
          break;
        }
      }
      if (cp != 0) {
        AppendUtf8(out, cp);
      } else {
        if (IsAsciiByte(b)) {
          reader.Restore({static_cast<std::uint8_t>(b)});
        }
        AppendUtf8(out, kReplacementCp);
      }
      continue;
    }
    if (IsAsciiByte(b)) {
      out.push_back(static_cast<char>(b));
    } else if (b >= 0x81 && b <= 0xFE) {
      leading = static_cast<std::uint8_t>(b);
    } else {
      AppendUtf8(out, kReplacementCp);
    }
  }
  if (leading != 0) {
    AppendUtf8(out, kReplacementCp);
  }
}

// ---------------------------------------------------------------------------
// WHATWG Shift_JIS decoder.
// ---------------------------------------------------------------------------
void DecodeShiftJis(std::string_view bytes, std::string& out)
{
  ByteReader reader(bytes);
  std::uint8_t leading = 0;
  for (int b = reader.Next(); b != -1; b = reader.Next()) {
    if (leading != 0) {
      const std::uint8_t lead = leading;
      leading = 0;
      const std::uint8_t leading_offset = lead < 0xA0 ? 0x81 : 0xC1;
      const std::uint8_t offset = b < 0x7F ? 0x40 : 0x41;
      std::uint16_t cp = 0;
      bool in_eudc = false;
      if ((b >= 0x40 && b <= 0x7E) || (b >= 0x80 && b <= 0xFC)) {
        const std::size_t pointer = static_cast<std::size_t>(lead - leading_offset) * 188U +
                                    static_cast<std::uint32_t>(b) - offset;
        if (pointer >= 8836 && pointer <= 10715) {
          // Windows EUDC interop range.
          AppendUtf8(out, 0xE000U - 8836U + static_cast<std::uint32_t>(pointer));
          in_eudc = true;
        } else {
          cp = detail::kJis0208[pointer];
        }
      }
      if (in_eudc) {
        continue;
      }
      if (cp != 0) {
        AppendUtf8(out, cp);
      } else {
        if (IsAsciiByte(b)) {
          reader.Restore({static_cast<std::uint8_t>(b)});
        }
        AppendUtf8(out, kReplacementCp);
      }
      continue;
    }
    if (IsAsciiByte(b) || b == 0x80) {
      out.push_back(static_cast<char>(b));
    } else if (b >= 0xA1 && b <= 0xDF) {
      // Halfwidth katakana.
      AppendUtf8(out, 0xFF61U - 0xA1U + static_cast<std::uint32_t>(b));
    } else if ((b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC)) {
      leading = static_cast<std::uint8_t>(b);
    } else {
      AppendUtf8(out, kReplacementCp);
    }
  }
  if (leading != 0) {
    AppendUtf8(out, kReplacementCp);
  }
}

// ---------------------------------------------------------------------------
// WHATWG EUC-JP decoder.
// ---------------------------------------------------------------------------
void DecodeEucJp(std::string_view bytes, std::string& out)
{
  ByteReader reader(bytes);
  std::uint8_t leading = 0;
  bool jis0212 = false;
  for (int b = reader.Next(); b != -1; b = reader.Next()) {
    if (leading == 0x8E && b >= 0xA1 && b <= 0xDF) {
      leading = 0;
      AppendUtf8(out, 0xFF61U - 0xA1U + static_cast<std::uint32_t>(b));
      continue;
    }
    if (leading == 0x8F && b >= 0xA1 && b <= 0xFE) {
      jis0212 = true;
      leading = static_cast<std::uint8_t>(b);
      continue;
    }
    if (leading != 0) {
      const std::uint8_t lead = leading;
      leading = 0;
      std::uint16_t cp = 0;
      if (lead >= 0xA1 && lead <= 0xFE && b >= 0xA1 && b <= 0xFE) {
        const std::size_t pointer =
            static_cast<std::size_t>(lead - 0xA1) * 94U + static_cast<std::uint32_t>(b) - 0xA1;
        cp = jis0212 ? detail::kJis0212[pointer] : detail::kJis0208[pointer];
      }
      jis0212 = false;
      if (cp != 0) {
        AppendUtf8(out, cp);
      } else {
        if (IsAsciiByte(b)) {
          reader.Restore({static_cast<std::uint8_t>(b)});
        }
        AppendUtf8(out, kReplacementCp);
      }
      continue;
    }
    if (IsAsciiByte(b)) {
      out.push_back(static_cast<char>(b));
    } else if (b == 0x8E || b == 0x8F || (b >= 0xA1 && b <= 0xFE)) {
      leading = static_cast<std::uint8_t>(b);
    } else {
      AppendUtf8(out, kReplacementCp);
    }
  }
  if (leading != 0) {
    AppendUtf8(out, kReplacementCp);
  }
}

// ---------------------------------------------------------------------------
// WHATWG EUC-KR decoder.
// ---------------------------------------------------------------------------
void DecodeEucKr(std::string_view bytes, std::string& out)
{
  ByteReader reader(bytes);
  std::uint8_t leading = 0;
  for (int b = reader.Next(); b != -1; b = reader.Next()) {
    if (leading != 0) {
      const std::uint8_t lead = leading;
      leading = 0;
      std::uint16_t cp = 0;
      if (b >= 0x41 && b <= 0xFE) {
        const std::size_t pointer =
            static_cast<std::size_t>(lead - 0x81) * 190U + static_cast<std::uint32_t>(b) - 0x41;
        cp = detail::kEucKr[pointer];
      }
      if (cp != 0) {
        AppendUtf8(out, cp);
      } else {
        if (IsAsciiByte(b)) {
          reader.Restore({static_cast<std::uint8_t>(b)});
        }
        AppendUtf8(out, kReplacementCp);
      }
      continue;
    }
    if (IsAsciiByte(b)) {
      out.push_back(static_cast<char>(b));
    } else if (b >= 0x81 && b <= 0xFE) {
      leading = static_cast<std::uint8_t>(b);
    } else {
      AppendUtf8(out, kReplacementCp);
    }
  }
  if (leading != 0) {
    AppendUtf8(out, kReplacementCp);
  }
}

// ---------------------------------------------------------------------------
// WHATWG ISO-2022-JP decoder.
// ---------------------------------------------------------------------------
void DecodeIso2022Jp(std::string_view bytes, std::string& out)
{
  enum class State
  {
    kAscii,
    kRoman,
    kKatakana,
    kLeadingByte,
    kTrailingByte,
    kEscapeStart,
    kEscape,
  };
  State state = State::kAscii;
  State output_state = State::kAscii;
  std::uint8_t leading = 0;
  bool output = false;

  ByteReader reader(bytes);
  for (int b = reader.Next(); b != -1; b = reader.Next()) {
    switch (state) {
    case State::kAscii:
      if (b == 0x1B) {
        state = State::kEscapeStart;
      } else if ((b >= 0x00 && b <= 0x7F) && b != 0x0E && b != 0x0F && b != 0x1B) {
        output = false;
        out.push_back(static_cast<char>(b));
      } else {
        output = false;
        AppendUtf8(out, kReplacementCp);
      }
      break;
    case State::kRoman:
      if (b == 0x1B) {
        state = State::kEscapeStart;
      } else if (b == 0x5C) {
        output = false;
        AppendUtf8(out, 0x00A5);
      } else if (b == 0x7E) {
        output = false;
        AppendUtf8(out, 0x203E);
      } else if ((b >= 0x00 && b <= 0x7F) && b != 0x0E && b != 0x0F && b != 0x1B && b != 0x5C &&
                 b != 0x7E) {
        output = false;
        out.push_back(static_cast<char>(b));
      } else {
        output = false;
        AppendUtf8(out, kReplacementCp);
      }
      break;
    case State::kKatakana:
      if (b == 0x1B) {
        state = State::kEscapeStart;
      } else if (b >= 0x21 && b <= 0x5F) {
        output = false;
        AppendUtf8(out, 0xFF61U - 0x21U + static_cast<std::uint32_t>(b));
      } else {
        output = false;
        AppendUtf8(out, kReplacementCp);
      }
      break;
    case State::kLeadingByte:
      if (b == 0x1B) {
        state = State::kEscapeStart;
      } else if (b >= 0x21 && b <= 0x7E) {
        output = false;
        leading = static_cast<std::uint8_t>(b);
        state = State::kTrailingByte;
      } else {
        output = false;
        AppendUtf8(out, kReplacementCp);
      }
      break;
    case State::kTrailingByte:
      if (b == 0x1B) {
        state = State::kEscapeStart;
        AppendUtf8(out, kReplacementCp);
      } else if (b >= 0x21 && b <= 0x7E) {
        state = State::kLeadingByte;
        const std::size_t pointer =
            static_cast<std::size_t>(leading - 0x21) * 94U + static_cast<std::uint32_t>(b) - 0x21;
        const std::uint16_t cp = detail::kJis0208[pointer];
        if (cp != 0) {
          AppendUtf8(out, cp);
        } else {
          AppendUtf8(out, kReplacementCp);
        }
      } else {
        state = State::kLeadingByte;
        AppendUtf8(out, kReplacementCp);
      }
      break;
    case State::kEscapeStart:
      if (b == 0x24 || b == 0x28) {
        leading = static_cast<std::uint8_t>(b);
        state = State::kEscape;
      } else {
        if (b != -1) {
          reader.Restore({static_cast<std::uint8_t>(b)});
        }
        output = false;
        state = output_state;
        AppendUtf8(out, kReplacementCp);
      }
      break;
    case State::kEscape: {
      const std::uint8_t esc = leading;
      leading = 0;
      std::optional<State> next;
      if (esc == 0x28 && b == 0x42) {
        next = State::kAscii;
      } else if (esc == 0x28 && b == 0x4A) {
        next = State::kRoman;
      } else if (esc == 0x28 && b == 0x49) {
        next = State::kKatakana;
      } else if (esc == 0x24 && (b == 0x40 || b == 0x42)) {
        next = State::kLeadingByte;
      }
      if (next.has_value()) {
        state = *next;
        output_state = *next;
        const bool was_output = output;
        output = true;
        if (was_output) {
          AppendUtf8(out, kReplacementCp);
        }
      } else {
        reader.Restore({esc, static_cast<std::uint8_t>(b)});
        output = false;
        state = output_state;
        AppendUtf8(out, kReplacementCp);
      }
      break;
    }
    }
  }
  // End-of-queue handling.
  if (state == State::kTrailingByte || state == State::kEscapeStart || state == State::kEscape) {
    AppendUtf8(out, kReplacementCp);
  }
}

// ---------------------------------------------------------------------------
// WHATWG x-user-defined decoder.
// ---------------------------------------------------------------------------
void DecodeXUserDefined(std::string_view bytes, std::string& out)
{
  for (const char c : bytes) {
    const unsigned b = static_cast<unsigned char>(c);
    if (b < 0x80) {
      out.push_back(static_cast<char>(b));
    } else {
      AppendUtf8(out, 0xF780U + b - 0x80U);
    }
  }
}

// ---------------------------------------------------------------------------
// HTML prescan helpers (WHATWG HTML 13.2.3.2)
// ---------------------------------------------------------------------------

constexpr std::size_t kPrescanLimit = 1024;

// "The algorithm for extracting a character encoding from a meta element".
std::optional<Charset> ExtractCharsetFromMetaContent(std::string_view value)
{
  std::size_t pos = 0;
  while (pos < value.size()) {
    // Find an ASCII case-insensitive match for "charset".
    const std::size_t found = value.find("charset", pos);
    if (found == std::string_view::npos) {
      return std::nullopt;
    }
    bool matches = true;
    for (std::size_t k = 0; k < 7; ++k) {
      if (AsciiLower(value[found + k]) != "charset"[k]) {
        matches = false;
        break;
      }
    }
    if (!matches) {
      pos = found + 1;
      continue;
    }
    std::size_t p = found + 7;
    while (p < value.size() && IsAsciiWhitespace(value[p])) {
      ++p;
    }
    if (p >= value.size() || value[p] != '=') {
      pos = found + 1;
      continue;
    }
    ++p;
    while (p < value.size() && IsAsciiWhitespace(value[p])) {
      ++p;
    }
    std::string label;
    if (p < value.size() && (value[p] == '"' || value[p] == '\'')) {
      const char quote = value[p];
      ++p;
      while (p < value.size() && value[p] != quote) {
        label.push_back(AsciiLower(value[p]));
        ++p;
      }
    } else {
      while (p < value.size() && !IsAsciiWhitespace(value[p]) && value[p] != ';') {
        label.push_back(AsciiLower(value[p]));
        ++p;
      }
    }
    if (label.empty()) {
      return std::nullopt;
    }
    return CharsetFromLabel(label);
  }
  return std::nullopt;
}

// "get an attribute" during the prescan.
struct PrescanAttribute
{
  std::string name;
  std::string value;
};

// Returns false when no attribute could be sniffed (position at '>').
bool PrescanGetAttribute(std::string_view bytes, std::size_t& pos, PrescanAttribute& out)
{
  const std::size_t size = bytes.size();
  // Skip whitespace and slashes.
  while (pos < size && (IsAsciiWhitespace(bytes[pos]) || bytes[pos] == '/')) {
    ++pos;
  }
  if (pos >= size || bytes[pos] == '>') {
    return false;
  }
  std::string name;
  std::string value;
  while (pos < size) {
    const char c = bytes[pos];
    if (c == '=' && !name.empty()) {
      ++pos;
      break;
    }
    if (c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ') {
      // "Spaces" step.
      while (pos < size && IsAsciiWhitespace(bytes[pos])) {
        ++pos;
      }
      if (pos >= size || bytes[pos] != '=') {
        out.name = std::move(name);
        out.value.clear();
        return true;
      }
      ++pos; // past '='
      break;
    }
    if (c == '/' || c == '>') {
      out.name = std::move(name);
      out.value.clear();
      return true;
    }
    name.push_back(AsciiLower(c));
    ++pos;
  }
  // Value step.
  while (pos < size && IsAsciiWhitespace(bytes[pos])) {
    ++pos;
  }
  if (pos < size && (bytes[pos] == '"' || bytes[pos] == '\'')) {
    const char quote = bytes[pos];
    ++pos;
    while (pos < size && bytes[pos] != quote) {
      value.push_back(AsciiLower(bytes[pos]));
      ++pos;
    }
    if (pos < size) {
      ++pos; // past closing quote
    }
  } else {
    while (pos < size) {
      const char c = bytes[pos];
      if (c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ' || c == '>') {
        break;
      }
      value.push_back(AsciiLower(c));
      ++pos;
    }
  }
  out.name = std::move(name);
  out.value = std::move(value);
  return true;
}

// "get an XML encoding" fallback.
std::optional<Charset> GetXmlEncoding(std::string_view bytes)
{
  constexpr std::string_view kXml = "<?xml";
  if (bytes.size() < kXml.size() || bytes.substr(0, kXml.size()) != kXml) {
    return std::nullopt;
  }
  const std::size_t end = bytes.find('>');
  if (end == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view decl = bytes.substr(0, end);
  const std::size_t enc = decl.find("encoding");
  if (enc == std::string_view::npos) {
    return std::nullopt;
  }
  std::size_t p = enc + 8;
  while (p < decl.size() && static_cast<unsigned char>(decl[p]) <= 0x20) {
    ++p;
  }
  if (p >= decl.size() || decl[p] != '=') {
    return std::nullopt;
  }
  ++p;
  while (p < decl.size() && static_cast<unsigned char>(decl[p]) <= 0x20) {
    ++p;
  }
  if (p >= decl.size() || (decl[p] != '"' && decl[p] != '\'')) {
    return std::nullopt;
  }
  const char quote = decl[p];
  ++p;
  const std::size_t end_q = decl.find(quote, p);
  if (end_q == std::string_view::npos) {
    return std::nullopt;
  }
  std::string label;
  for (std::size_t k = p; k < end_q; ++k) {
    if (static_cast<unsigned char>(decl[k]) <= 0x20) {
      return std::nullopt;
    }
    label.push_back(AsciiLower(decl[k]));
  }
  auto charset = CharsetFromLabel(label);
  if (!charset.has_value()) {
    return std::nullopt;
  }
  if (*charset == Charset::kUtf16Be || *charset == Charset::kUtf16Le) {
    return Charset::kUtf8;
  }
  return charset;
}

// The prescan algorithm itself.
std::optional<Charset> PrescanBytes(std::string_view bytes)
{
  const std::size_t limit = std::min(bytes.size(), kPrescanLimit);
  std::size_t pos = 0;

  // Prescan for UTF-16 XML declarations.
  if (bytes.size() >= 6) {
    const unsigned char b0 = static_cast<unsigned char>(bytes[0]);
    const unsigned char b1 = static_cast<unsigned char>(bytes[1]);
    const unsigned char b2 = static_cast<unsigned char>(bytes[2]);
    const unsigned char b3 = static_cast<unsigned char>(bytes[3]);
    const unsigned char b4 = static_cast<unsigned char>(bytes[4]);
    const unsigned char b5 = static_cast<unsigned char>(bytes[5]);
    if (b0 == 0x3C && b1 == 0x00 && b2 == 0x3F && b3 == 0x00 && b4 == 0x78 && b5 == 0x00) {
      return Charset::kUtf16Le;
    }
    if (b0 == 0x00 && b1 == 0x3C && b2 == 0x00 && b3 == 0x3F && b4 == 0x00 && b5 == 0x78) {
      return Charset::kUtf16Be;
    }
  }

  while (pos < limit) {
    if (pos + 4 <= limit && bytes[pos] == 0x3C && bytes[pos + 1] == 0x21 &&
        bytes[pos + 2] == 0x2D && bytes[pos + 3] == 0x2D) {
      // <!-- comment: advance to the first '-->'.
      bool found = false;
      for (std::size_t p = pos + 4; p + 2 < limit; ++p) {
        if (bytes[p] == 0x2D && bytes[p + 1] == 0x2D && bytes[p + 2] == 0x3E) {
          pos = p + 3;
          found = true;
          break;
        }
      }
      if (!found) {
        pos = limit;
      }
      continue;
    }
    // <meta followed by whitespace or '/' (case-insensitive).
    if (pos + 6 <= limit && bytes[pos] == 0x3C &&
        AsciiLower(static_cast<char>(bytes[pos + 1])) == 'm' &&
        AsciiLower(static_cast<char>(bytes[pos + 2])) == 'e' &&
        AsciiLower(static_cast<char>(bytes[pos + 3])) == 't' &&
        AsciiLower(static_cast<char>(bytes[pos + 4])) == 'a' &&
        (bytes[pos + 5] == 0x09 || bytes[pos + 5] == 0x0A || bytes[pos + 5] == 0x0C ||
         bytes[pos + 5] == 0x0D || bytes[pos + 5] == 0x20 || bytes[pos + 5] == 0x2F)) {
      pos += 5;
      while (pos < limit && (bytes[pos] == 0x09 || bytes[pos] == 0x0A || bytes[pos] == 0x0C ||
                             bytes[pos] == 0x0D || bytes[pos] == 0x20 || bytes[pos] == 0x2F)) {
        ++pos;
      }
      std::vector<std::string> seen;
      bool got_pragma = false;
      std::optional<bool> need_pragma;
      std::optional<Charset> charset;
      for (;;) {
        PrescanAttribute attr;
        if (!PrescanGetAttribute(bytes, pos, attr)) {
          break;
        }
        if (std::find(seen.begin(), seen.end(), attr.name) != seen.end()) {
          continue;
        }
        seen.push_back(attr.name);
        if (attr.name == "http-equiv") {
          if (attr.value == "content-type") {
            got_pragma = true;
          }
        } else if (attr.name == "content") {
          if (!charset.has_value()) {
            const auto extracted = ExtractCharsetFromMetaContent(attr.value);
            if (extracted.has_value()) {
              charset = extracted;
              need_pragma = true;
            }
          }
        } else if (attr.name == "charset") {
          charset = CharsetFromLabel(attr.value);
          need_pragma = false;
        }
      }
      if (need_pragma.has_value()) {
        if (!need_pragma.value() || got_pragma) {
          if (charset.has_value()) {
            if (*charset == Charset::kUtf16Be || *charset == Charset::kUtf16Le) {
              return Charset::kUtf8;
            }
            if (*charset == Charset::kXUserDefined) {
              return Charset::kWindows1252;
            }
            return charset;
          }
        }
      }
      continue;
    }
    // A tag: '<' optionally followed by '/' then an ASCII letter.
    bool is_tag = bytes[pos] == 0x3C;
    std::size_t letter = pos + 1;
    if (is_tag && letter < limit && bytes[letter] == 0x2F) {
      ++letter;
    }
    if (is_tag && letter < limit && IsAsciiAlpha(static_cast<char>(bytes[letter]))) {
      while (pos < limit && bytes[pos] != 0x09 && bytes[pos] != 0x0A && bytes[pos] != 0x0C &&
             bytes[pos] != 0x0D && bytes[pos] != 0x20 && bytes[pos] != 0x3E) {
        ++pos;
      }
      while (pos < limit) {
        PrescanAttribute attr;
        if (!PrescanGetAttribute(bytes, pos, attr)) {
          break;
        }
      }
      continue;
    }
    // <! , </ or <? -> advance to first '>'.
    if (pos + 1 < limit && bytes[pos] == 0x3C &&
        (bytes[pos + 1] == 0x21 || bytes[pos + 1] == 0x2F || bytes[pos + 1] == 0x3F)) {
      const std::size_t gt = bytes.find(0x3E, pos + 2);
      pos = gt == std::string_view::npos ? limit : gt + 1;
      continue;
    }
    ++pos;
  }
  return std::nullopt;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::string_view CharsetName(Charset charset)
{
  static constexpr std::string_view kNames[] = {
      "UTF-8",        "IBM866",       "ISO-8859-2",   "ISO-8859-3",     "ISO-8859-4",
      "ISO-8859-5",   "ISO-8859-6",   "ISO-8859-7",   "ISO-8859-8",     "ISO-8859-8-I",
      "ISO-8859-10",  "ISO-8859-13",  "ISO-8859-14",  "ISO-8859-15",    "ISO-8859-16",
      "KOI8-R",       "KOI8-U",       "macintosh",    "windows-874",    "windows-1250",
      "windows-1251", "windows-1252", "windows-1253", "windows-1254",   "windows-1255",
      "windows-1256", "windows-1257", "windows-1258", "x-mac-cyrillic", "gb18030",
      "Big5",         "EUC-JP",       "ISO-2022-JP",  "Shift_JIS",      "EUC-KR",
      "replacement",  "UTF-16BE",     "UTF-16LE",     "x-user-defined", "unknown",
  };
  const int idx = static_cast<int>(charset);
  if (idx < 0 || idx >= static_cast<int>(std::size(kNames))) {
    return "unknown";
  }
  return kNames[idx];
}

std::optional<Charset> CharsetFromLabel(std::string_view label)
{
  std::string lowered;
  lowered.reserve(label.size());
  for (const char c : label) {
    if (IsAsciiWhitespace(c)) {
      continue;
    }
    lowered.push_back(AsciiLower(c));
  }
  if (lowered.empty()) {
    return std::nullopt;
  }
  const auto* begin = detail::kLabels;
  const auto* end = begin + detail::kLabelCount;
  const auto* it = std::lower_bound(
      begin, end, lowered, [](const detail::LabelEntry& e, const std::string& key) {
        return std::string_view(e.label) < key;
      });
  if (it != end && std::string_view(it->label) == lowered) {
    if (it->charset == Charset::kReplacement) {
      return std::nullopt;
    }
    return it->charset;
  }
  return std::nullopt;
}

std::optional<Charset> CharsetFromLabelOrReplacement(std::string_view label)
{
  std::string lowered;
  lowered.reserve(label.size());
  for (const char c : label) {
    if (IsAsciiWhitespace(c)) {
      continue;
    }
    lowered.push_back(AsciiLower(c));
  }
  if (lowered.empty()) {
    return std::nullopt;
  }
  const auto* begin = detail::kLabels;
  const auto* end = begin + detail::kLabelCount;
  const auto* it = std::lower_bound(
      begin, end, lowered, [](const detail::LabelEntry& e, const std::string& key) {
        return std::string_view(e.label) < key;
      });
  if (it != end && std::string_view(it->label) == lowered) {
    return it->charset;
  }
  return std::nullopt;
}

std::optional<Charset> CharsetFromHttpHeader(std::string_view content_type)
{
  std::size_t pos = 0;
  const std::size_t size = content_type.size();
  while (pos < size) {
    // Find the next ';'.
    const std::size_t semi = content_type.find(';', pos);
    const std::string_view segment = semi == std::string_view::npos
                                         ? content_type.substr(pos)
                                         : content_type.substr(pos, semi - pos);
    std::size_t p = 0;
    while (p < segment.size() && IsAsciiWhitespace(segment[p])) {
      ++p;
    }
    // Parameter name must be "charset" (ASCII case-insensitive).
    if (segment.size() - p >= 7 && segment.substr(p, 7) == "charset") {
      std::size_t eq = p + 7;
      while (eq < segment.size() && IsAsciiWhitespace(segment[eq])) {
        ++eq;
      }
      if (eq < segment.size() && segment[eq] == '=') {
        ++eq;
        while (eq < segment.size() && IsAsciiWhitespace(segment[eq])) {
          ++eq;
        }
        std::string label;
        while (eq < segment.size() && !IsAsciiWhitespace(segment[eq]) && segment[eq] != ';' &&
               segment[eq] != '"' && segment[eq] != '\'') {
          label.push_back(AsciiLower(segment[eq]));
          ++eq;
        }
        return CharsetFromLabelOrReplacement(label);
      }
    }
    if (semi == std::string_view::npos) {
      break;
    }
    pos = semi + 1;
  }
  return std::nullopt;
}

std::string DecodeToUtf8(std::string_view bytes, Charset charset)
{
  std::string out;
  // BOM sniffing overrides the label (WHATWG decode).
  const std::optional<Charset> bom = SniffBom(bytes);
  if (bom.has_value()) {
    const std::size_t skip = (*bom == Charset::kUtf8) ? 3 : 2;
    if (skip >= bytes.size()) {
      return out;
    }
    bytes.remove_prefix(skip);
    charset = *bom;
  }
  switch (charset) {
  case Charset::kUtf8:
  case Charset::kUnknown:
    DecodeUtf8(bytes, out);
    break;
  case Charset::kUtf16Be:
    DecodeUtf16(bytes, out, true);
    break;
  case Charset::kUtf16Le:
    DecodeUtf16(bytes, out, false);
    break;
  case Charset::kGb18030:
    DecodeGb18030(bytes, out);
    break;
  case Charset::kBig5:
    DecodeBig5(bytes, out);
    break;
  case Charset::kShiftJis:
    DecodeShiftJis(bytes, out);
    break;
  case Charset::kEucJp:
    DecodeEucJp(bytes, out);
    break;
  case Charset::kEucKr:
    DecodeEucKr(bytes, out);
    break;
  case Charset::kIso2022Jp:
    DecodeIso2022Jp(bytes, out);
    break;
  case Charset::kXUserDefined:
    DecodeXUserDefined(bytes, out);
    break;
  case Charset::kReplacement:
    // The replacement encoding emits exactly one U+FFFD.
    AppendUtf8(out, kReplacementCp);
    break;
  case Charset::kIbm866:
    DecodeSingleByte(bytes, out, detail::kSingleByteIbm866);
    break;
  case Charset::kIso88592:
    DecodeSingleByte(bytes, out, detail::kSingleByteIso88592);
    break;
  case Charset::kIso88593:
    DecodeSingleByte(bytes, out, detail::kSingleByteIso88593);
    break;
  case Charset::kIso88594:
    DecodeSingleByte(bytes, out, detail::kSingleByteIso88594);
    break;
  case Charset::kIso88595:
    DecodeSingleByte(bytes, out, detail::kSingleByteIso88595);
    break;
  case Charset::kIso88596:
    DecodeSingleByte(bytes, out, detail::kSingleByteIso88596);
    break;
  case Charset::kIso88597:
    DecodeSingleByte(bytes, out, detail::kSingleByteIso88597);
    break;
  case Charset::kIso88598:
  case Charset::kIso88598I:
    DecodeSingleByte(bytes, out, detail::kSingleByteIso88598);
    break;
  case Charset::kIso885910:
    DecodeSingleByte(bytes, out, detail::kSingleByteIso885910);
    break;
  case Charset::kIso885913:
    DecodeSingleByte(bytes, out, detail::kSingleByteIso885913);
    break;
  case Charset::kIso885914:
    DecodeSingleByte(bytes, out, detail::kSingleByteIso885914);
    break;
  case Charset::kIso885915:
    DecodeSingleByte(bytes, out, detail::kSingleByteIso885915);
    break;
  case Charset::kIso885916:
    DecodeSingleByte(bytes, out, detail::kSingleByteIso885916);
    break;
  case Charset::kKoi8R:
    DecodeSingleByte(bytes, out, detail::kSingleByteKoi8R);
    break;
  case Charset::kKoi8U:
    DecodeSingleByte(bytes, out, detail::kSingleByteKoi8U);
    break;
  case Charset::kMacintosh:
    DecodeSingleByte(bytes, out, detail::kSingleByteMacintosh);
    break;
  case Charset::kWindows874:
    DecodeSingleByte(bytes, out, detail::kSingleByteWindows874);
    break;
  case Charset::kWindows1250:
    DecodeSingleByte(bytes, out, detail::kSingleByteWindows1250);
    break;
  case Charset::kWindows1251:
    DecodeSingleByte(bytes, out, detail::kSingleByteWindows1251);
    break;
  case Charset::kWindows1252:
    DecodeSingleByte(bytes, out, detail::kSingleByteWindows1252);
    break;
  case Charset::kWindows1253:
    DecodeSingleByte(bytes, out, detail::kSingleByteWindows1253);
    break;
  case Charset::kWindows1254:
    DecodeSingleByte(bytes, out, detail::kSingleByteWindows1254);
    break;
  case Charset::kWindows1255:
    DecodeSingleByte(bytes, out, detail::kSingleByteWindows1255);
    break;
  case Charset::kWindows1256:
    DecodeSingleByte(bytes, out, detail::kSingleByteWindows1256);
    break;
  case Charset::kWindows1257:
    DecodeSingleByte(bytes, out, detail::kSingleByteWindows1257);
    break;
  case Charset::kWindows1258:
    DecodeSingleByte(bytes, out, detail::kSingleByteWindows1258);
    break;
  case Charset::kXMacCyrillic:
    DecodeSingleByte(bytes, out, detail::kSingleByteXMacCyrillic);
    break;
  }
  return out;
}

std::optional<Charset> SniffBom(std::string_view bytes)
{
  if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
      static_cast<unsigned char>(bytes[1]) == 0xBB &&
      static_cast<unsigned char>(bytes[2]) == 0xBF) {
    return Charset::kUtf8;
  }
  if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFE &&
      static_cast<unsigned char>(bytes[1]) == 0xFF) {
    return Charset::kUtf16Be;
  }
  if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
      static_cast<unsigned char>(bytes[1]) == 0xFE) {
    return Charset::kUtf16Le;
  }
  return std::nullopt;
}

Charset DetectHtmlCharset(std::string_view bytes, std::optional<Charset> http_hint)
{
  // BOM sniffing is the most authoritative (returns confidence certain).
  if (const auto bom = SniffBom(bytes); bom.has_value()) {
    return *bom;
  }
  // The transport-layer charset (HTTP Content-Type) has priority over the
  // in-document prescan (certain vs tentative confidence).
  if (http_hint.has_value() && *http_hint != Charset::kUnknown) {
    return *http_hint;
  }
  if (const auto prescan = PrescanBytes(bytes); prescan.has_value()) {
    return *prescan;
  }
  if (const auto xml = GetXmlEncoding(bytes); xml.has_value()) {
    return *xml;
  }
  // WHATWG suggested default for "all other locales".
  return Charset::kWindows1252;
}

} // namespace neko::base::encoding
