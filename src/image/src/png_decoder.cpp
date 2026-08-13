// PNG decoder, implemented from scratch against the PNG specification
// (W3C/ISO 15948).  zlib is used only to inflate the IDAT stream.
//
// Supported: bit depths 1/2/4/8/16, all color types (0/2/3/4/6), adaptive
// filtering, Adam7 interlacing, chunk CRC verification.  Unsupported
// features (e.g. sBIT scaling) are ignored safely.
//
// Threading: pure functions, no shared state.

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <zlib.h>

#include "neko/base/status.h"
#include "neko/image/image.h"

namespace neko::image {
namespace {

constexpr std::string_view kSignature = "\x89PNG\r\n\x1a\n";

constexpr uint32_t kChunkIHDR = 0x49484452;  // "IHDR"
constexpr uint32_t kChunkPLTE = 0x504C5445;  // "PLTE"
constexpr uint32_t kChunkTRNS = 0x74524E53;  // "tRNS"
constexpr uint32_t kChunkIDAT = 0x49444154;  // "IDAT"
constexpr uint32_t kChunkIEND = 0x49454E44;  // "IEND"

uint32_t ReadU32BE(std::string_view s, size_t off) {
  return (static_cast<uint32_t>(static_cast<uint8_t>(s[off])) << 24) |
         (static_cast<uint32_t>(static_cast<uint8_t>(s[off + 1])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(s[off + 2])) << 8) |
         static_cast<uint32_t>(static_cast<uint8_t>(s[off + 3]));
}

base::Result<std::string> InflateZlib(std::string_view in) {
  z_stream zs{};
  if (inflateInit(&zs) != Z_OK) {
    return base::Error::Unknown("png: inflateInit failed");
  }
  zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
  zs.avail_in = static_cast<uInt>(in.size());
  std::string out;
  char buf[16384];
  int ret = Z_OK;
  while (ret != Z_STREAM_END) {
    zs.next_out = reinterpret_cast<Bytef*>(buf);
    zs.avail_out = sizeof(buf);
    ret = inflate(&zs, Z_NO_FLUSH);
    if (ret != Z_OK && ret != Z_STREAM_END) {
      inflateEnd(&zs);
      return base::Error::Parse("png: corrupt IDAT deflate stream");
    }
    out.append(buf, sizeof(buf) - zs.avail_out);
    // A deflate stream that consumes input without producing output (and
    // never ends) is pathological; bail out.
    if (zs.avail_in == 0 && ret != Z_STREAM_END && zs.avail_out == sizeof(buf)) {
      inflateEnd(&zs);
      return base::Error::Parse("png: truncated IDAT stream");
    }
  }
  inflateEnd(&zs);
  return out;
}

struct PaletteEntry {
  uint8_t r, g, b;
};

// Header / per-pixel geometry.
struct PngInfo {
  int width = 0;
  int height = 0;
  int bit_depth = 0;
  int color_type = 0;
  int interlace = 0;
  int channels = 0;          // 1..4
  int bits_per_pixel = 0;    // channels * bit_depth
  int bytes_per_pixel = 0;   // filter bpp: ceil(bits_per_pixel/8), min 1
  std::vector<PaletteEntry> palette;
  std::vector<uint8_t> trns_alpha;  // per-entry alpha (palette) or raw tRNS
};

base::Result<PngInfo> ParseIHDR(std::string_view data) {
  if (data.size() < 13) {
    return base::Error::Parse("png: truncated IHDR");
  }
  PngInfo info;
  info.width = static_cast<int>(ReadU32BE(data, 0));
  info.height = static_cast<int>(ReadU32BE(data, 4));
  info.bit_depth = static_cast<uint8_t>(data[8]);
  info.color_type = static_cast<uint8_t>(data[9]);
  const uint8_t compression = static_cast<uint8_t>(data[10]);
  const uint8_t filter_method = static_cast<uint8_t>(data[11]);
  info.interlace = static_cast<uint8_t>(data[12]);

  if (info.width <= 0 || info.height <= 0) {
    return base::Error::Parse("png: invalid dimensions");
  }
  if (compression != 0 || filter_method != 0) {
    return base::Error::NotImplemented("png: unsupported compression/filter method");
  }
  if (info.interlace != 0 && info.interlace != 1) {
    return base::Error::Parse("png: invalid interlace method");
  }

  switch (info.color_type) {
    case 0: info.channels = 1; break;  // grayscale
    case 2: info.channels = 3; break;  // RGB
    case 3: info.channels = 1; break;  // palette
    case 4: info.channels = 2; break;  // grayscale + alpha
    case 6: info.channels = 4; break;  // RGBA
    default:
      return base::Error::Parse("png: invalid color type");
  }

  const bool valid_depth =
      (info.color_type == 0 && (info.bit_depth == 1 || info.bit_depth == 2 ||
                                info.bit_depth == 4 || info.bit_depth == 8 ||
                                info.bit_depth == 16)) ||
      (info.color_type == 2 && (info.bit_depth == 8 || info.bit_depth == 16)) ||
      (info.color_type == 3 && (info.bit_depth == 1 || info.bit_depth == 2 ||
                                info.bit_depth == 4 || info.bit_depth == 8)) ||
      (info.color_type == 4 && (info.bit_depth == 8 || info.bit_depth == 16)) ||
      (info.color_type == 6 && (info.bit_depth == 8 || info.bit_depth == 16));
  if (!valid_depth) {
    return base::Error::Parse("png: invalid bit depth for color type");
  }

  info.bits_per_pixel = info.channels * info.bit_depth;
  info.bytes_per_pixel = (info.bits_per_pixel + 7) / 8;
  if (info.bytes_per_pixel < 1) info.bytes_per_pixel = 1;

  // A crafted IHDR may declare a width whose packed scanline width overflows
  // 32-bit arithmetic (e.g. width = 2^26 with 16-bit RGBA makes
  // width * bits_per_pixel = 2^32).  Downstream int-based scanline
  // computations then wrap around and read out of bounds, so reject such
  // images outright instead.
  if (static_cast<int64_t>(info.width) * info.bits_per_pixel > INT32_MAX - 7) {
    return base::Error::Parse("png: scanline width overflows");
  }
  return info;
}

int PaethPredictor(int a, int b, int c) {
  const int p = a + b - c;
  const int pa = std::abs(p - a);
  const int pb = std::abs(p - b);
  const int pc = std::abs(p - c);
  if (pa <= pb && pa <= pc) return a;
  if (pb <= pc) return b;
  return c;
}

// Unfilters |raw| scanlines (each with a leading filter byte) into |out|.
// |width_bytes| is the packed byte width of one scanline, |bpp| the filter
// bytes-per-pixel.
base::Result<void> Unfilter(std::string_view raw, int width_bytes, int bpp,
                            int height, std::vector<uint8_t>& out) {
  const size_t stride = static_cast<size_t>(width_bytes) + 1;
  const size_t bpp_size = static_cast<size_t>(bpp);
  if (raw.size() < stride * static_cast<size_t>(height)) {
    return base::Error::Parse("png: scanline data too short");
  }
  out.assign(static_cast<size_t>(width_bytes) * static_cast<size_t>(height), 0);
  const uint8_t* prev = nullptr;
  for (int y = 0; y < height; ++y) {
    const uint8_t* row = reinterpret_cast<const uint8_t*>(raw.data()) +
                         static_cast<size_t>(y) * stride;
    const uint8_t filter = row[0];
    uint8_t* recon = out.data() + static_cast<size_t>(y) * static_cast<size_t>(width_bytes);
    for (int i = 0; i < width_bytes; ++i) {
      const size_t ui = static_cast<size_t>(i);
      const uint8_t raw_byte = row[1 + ui];
      const bool has_left = ui >= bpp_size;
      const int a = has_left ? recon[ui - bpp_size] : 0;
      const int b = prev != nullptr ? prev[ui] : 0;
      const int c = (has_left && prev != nullptr) ? prev[ui - bpp_size] : 0;
      int value = 0;
      switch (filter) {
        case 0: value = raw_byte; break;                       // None
        case 1: value = raw_byte + a; break;                   // Sub
        case 2: value = raw_byte + b; break;                   // Up
        case 3: value = raw_byte + (a + b) / 2; break;         // Average
        case 4: value = raw_byte + PaethPredictor(a, b, c); break;  // Paeth
        default:
          return base::Error::Parse("png: invalid filter type");
      }
      recon[ui] = static_cast<uint8_t>(value & 0xFF);
    }
    prev = recon;
  }
  return base::Error();
}

// Scale factor for sub-byte gray samples (1 -> 255, 2 -> 85, 4 -> 17).
int SubByteScale(int bit_depth) {
  switch (bit_depth) {
    case 1: return 255;
    case 2: return 85;
    case 4: return 17;
    default: return 1;
  }
}

// Converts one unfiltered, packed row into RGBA pixels.  Pixel (px, py) of
// the block is placed at (offset_x + px * xstep, offset_y + py * ystep);
// for non-interlaced images xstep = ystep = 1 and offsets are 0.
void ConvertRow(const PngInfo& info, const uint8_t* row, int pixel_width,
                int offset_x, int xstep, int offset_y, int ystep, int y,
                std::vector<uint8_t>& rgba) {
  const size_t stride = static_cast<size_t>(info.width) * 4;
  const size_t xstep_u = static_cast<size_t>(xstep);
  const size_t ystep_u = static_cast<size_t>(ystep);
  uint8_t* dst = rgba.data() +
                 (static_cast<size_t>(offset_y) + static_cast<size_t>(y) * ystep_u) * stride;
  const size_t col0 = static_cast<size_t>(offset_x) * 4;
  int bitpos = 0;  // bit offset into |row| for sub-byte depths
  size_t byte_index = 0;

  const auto read_channel = [&](int bits) -> int {
    if (bits == 8) {
      return row[byte_index++];
    }
    if (bits == 16) {
      const int hi = row[byte_index];
      byte_index += 2;
      return hi;  // 16-bit samples are reduced to their high byte
    }
    // Sub-byte: extract |bits| MSB-first.
    int value = 0;
    for (int i = 0; i < bits; ++i) {
      const int bit = (row[static_cast<size_t>(bitpos / 8)] >> (7 - (bitpos % 8))) & 1;
      value = (value << 1) | bit;
      ++bitpos;
    }
    return value;
  };

  for (int x = 0; x < pixel_width; ++x) {
    int r = 0, g = 0, b = 0, a = 255;
    switch (info.color_type) {
      case 0: {  // grayscale
        const int gray = read_channel(info.bit_depth);
        const int v = info.bit_depth < 8 ? gray * SubByteScale(info.bit_depth) : gray;
        r = g = b = v;
        if (!info.trns_alpha.empty()) {
          const bool transparent =
              info.bit_depth == 16
                  ? gray == ((info.trns_alpha[0] << 8) | info.trns_alpha[1])
                  : gray == info.trns_alpha[0];
          if (transparent) a = 0;
        }
        break;
      }
      case 2: {  // RGB
        r = read_channel(info.bit_depth);
        g = read_channel(info.bit_depth);
        b = read_channel(info.bit_depth);
        if (info.trns_alpha.size() >= 6 && r == info.trns_alpha[0] &&
            g == info.trns_alpha[2] && b == info.trns_alpha[4]) {
          a = 0;
        }
        break;
      }
      case 3: {  // palette
        const size_t index = static_cast<size_t>(read_channel(info.bit_depth));
        if (index < info.palette.size()) {
          r = info.palette[index].r;
          g = info.palette[index].g;
          b = info.palette[index].b;
        }
        if (index < info.trns_alpha.size()) {
          a = info.trns_alpha[index];
        }
        break;
      }
      case 4: {  // grayscale + alpha
        const int gray = read_channel(info.bit_depth);
        r = g = b = gray;
        a = read_channel(info.bit_depth);
        break;
      }
      case 6: {  // RGBA
        r = read_channel(info.bit_depth);
        g = read_channel(info.bit_depth);
        b = read_channel(info.bit_depth);
        a = read_channel(info.bit_depth);
        break;
      }
    }
    const size_t d = col0 + static_cast<size_t>(x) * xstep_u * 4;
    dst[d + 0] = static_cast<uint8_t>(r);
    dst[d + 1] = static_cast<uint8_t>(g);
    dst[d + 2] = static_cast<uint8_t>(b);
    dst[d + 3] = static_cast<uint8_t>(a);
  }
}

// Decodes one rectangular block of scanlines (a whole image or an Adam7
// pass) and places its pixels at the pass grid.
base::Result<void> DecodeBlock(const PngInfo& info, std::string_view raw,
                               int block_width, int block_height, int offset_x,
                               int xstep, int offset_y, int ystep,
                               std::vector<uint8_t>& rgba) {
  if (block_width <= 0 || block_height <= 0) return base::Error();
  const int packed_row_bytes = (block_width * info.bits_per_pixel + 7) / 8;
  std::vector<uint8_t> recon;
  auto r = Unfilter(raw, packed_row_bytes, info.bytes_per_pixel, block_height, recon);
  if (!r) return r.error();
  for (int y = 0; y < block_height; ++y) {
    ConvertRow(info, recon.data() + static_cast<size_t>(y) * static_cast<size_t>(packed_row_bytes),
               block_width, offset_x, xstep, offset_y, ystep, y, rgba);
  }
  return base::Error();
}

// Adam7 pass geometry (xstart, ystart, xstep, ystep).
struct Adam7Pass {
  int xstart, ystart, xstep, ystep;
};
constexpr Adam7Pass kAdam7[7] = {
    {0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4},
    {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2},
};

int PassSize(int total, int start, int step) {
  if (total <= start) return 0;
  return (total - start + step - 1) / step;
}

}  // namespace

bool IsPng(std::string_view data) {
  return data.size() >= kSignature.size() && data.substr(0, kSignature.size()) == kSignature;
}

base::Result<Image> DecodePng(std::string_view data) {
  if (!IsPng(data)) {
    return base::Error::InvalidArgument("not a PNG file");
  }
  size_t pos = kSignature.size();

  std::optional<PngInfo> info;
  std::string idat;
  std::vector<uint8_t> all_trns;

  while (pos + 12 <= data.size()) {
    const uint32_t length = ReadU32BE(data, pos);
    const uint32_t type = ReadU32BE(data, pos + 4);
    const size_t chunk_start = pos + 8;
    if (chunk_start + length > data.size()) {
      return base::Error::Parse("png: truncated chunk data");
    }
    const std::string_view chunk(data.data() + chunk_start, length);

    // Verify the chunk CRC (covers type + data).
    const uint32_t expected_crc = ReadU32BE(data, chunk_start + length);
    const uLong crc = crc32(0L,
                            reinterpret_cast<const Bytef*>(data.data() + pos + 4),
                            static_cast<uInt>(4 + length));
    if (crc != expected_crc) {
      return base::Error::Parse("png: chunk CRC mismatch");
    }

    if (type == kChunkIHDR) {
      if (info.has_value()) return base::Error::Parse("png: duplicate IHDR");
      auto r = ParseIHDR(chunk);
      if (!r) return r.error();
      info = r.value();
    } else if (type == kChunkPLTE) {
      if (!info.has_value()) return base::Error::Parse("png: PLTE before IHDR");
      if (length % 3 != 0) return base::Error::Parse("png: bad PLTE length");
      info->palette.clear();
      for (size_t i = 0; i < length; i += 3) {
        info->palette.push_back(
            PaletteEntry{static_cast<uint8_t>(chunk[i]), static_cast<uint8_t>(chunk[i + 1]),
                         static_cast<uint8_t>(chunk[i + 2])});
      }
    } else if (type == kChunkTRNS) {
      if (!info.has_value()) return base::Error::Parse("png: tRNS before IHDR");
      all_trns.assign(chunk.begin(), chunk.end());
    } else if (type == kChunkIDAT) {
      idat.append(chunk);
    } else if (type == kChunkIEND) {
      break;
    }
    // Unknown ancillary chunks are skipped safely.

    pos = chunk_start + length + 4;
  }

  if (!info.has_value()) return base::Error::Parse("png: missing IHDR");
  if (idat.empty()) return base::Error::Parse("png: missing IDAT");

  PngInfo& p = info.value();

  // Interpret tRNS per color type.
  if (p.color_type == 3 && !all_trns.empty()) {
    p.trns_alpha.assign(p.palette.size(), 255);
    const size_t n = std::min(all_trns.size(), p.palette.size());
    for (size_t i = 0; i < n; ++i) p.trns_alpha[i] = all_trns[i];
  } else if (p.color_type == 0 && all_trns.size() >= 2) {
    p.trns_alpha = {all_trns[0], all_trns[1]};
  } else if (p.color_type == 2 && all_trns.size() >= 6) {
    p.trns_alpha = {all_trns[0], all_trns[1], all_trns[2], all_trns[3],
                    all_trns[4], all_trns[5]};
  }

  Image out;
  out.width = p.width;
  out.height = p.height;
  out.rgba.assign(static_cast<size_t>(p.width) * static_cast<size_t>(p.height) * 4, 0);

  auto inflated = InflateZlib(idat);
  if (!inflated) return inflated.error();
  const std::string& zdata = inflated.value();

  size_t consumed = 0;
  if (p.interlace == 0) {
    const int packed_row_bytes = (p.width * p.bits_per_pixel + 7) / 8;
    const size_t needed = static_cast<size_t>(p.height) *
                          (static_cast<size_t>(packed_row_bytes) + 1);
    if (zdata.size() < needed) {
      return base::Error::Parse("png: IDAT data too short");
    }
    auto r = DecodeBlock(p, std::string_view(zdata).substr(0, needed), p.width,
                         p.height, 0, 1, 0, 1, out.rgba);
    if (!r) return r.error();
    consumed = needed;
  } else {
    for (const Adam7Pass& pass : kAdam7) {
      const int pw = PassSize(p.width, pass.xstart, pass.xstep);
      const int ph = PassSize(p.height, pass.ystart, pass.ystep);
      if (pw == 0 || ph == 0) continue;
      const int packed_row_bytes = (pw * p.bits_per_pixel + 7) / 8;
      const size_t needed = static_cast<size_t>(ph) *
                            (static_cast<size_t>(packed_row_bytes) + 1);
      if (consumed + needed > zdata.size()) {
        return base::Error::Parse("png: truncated Adam7 pass data");
      }
      auto r = DecodeBlock(p, std::string_view(zdata).substr(consumed, needed), pw, ph,
                           pass.xstart, pass.xstep, pass.ystart, pass.ystep, out.rgba);
      if (!r) return r.error();
      consumed += needed;
    }
  }
  return out;
}

}  // namespace neko::image
