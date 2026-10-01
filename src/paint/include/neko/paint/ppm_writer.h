#pragma once

#include "neko/base/status.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace neko::paint {

// Streaming P6 (binary PPM) encoder for tightly packed RGBA8888 rows.
//
// Pixels are encoded as RGB888 with alpha composited over white -- the same
// encoding Rasterizer output has always used (see WritePpm), so output is byte
// identical to a single full-image encode.  Rows are appended as they are
// produced, which is what lets a very tall page screenshot be rasterized band
// by band instead of allocating one full-page RGBA buffer (a 100k-row page at
// 800 px wide is ~300 MiB per buffer, and the CLI used to hold two).
//
// Output is atomic: rows go to an exclusively created temporary file next to
// |path| and Finish() renames it into place.  A writer destroyed without
// Finish() removes the temporary file.  Not thread-safe; use one writer per
// output file.
class PpmWriter
{
public:
  // Opens the temporary file and writes the P6 header.  |width| x |height| is
  // the final image size; the caller must append exactly |height| rows.
  static base::Result<PpmWriter> Create(std::string_view path, int width, int height);

  PpmWriter(PpmWriter&& other) noexcept;
  PpmWriter& operator=(PpmWriter&& other) noexcept;
  PpmWriter(const PpmWriter&) = delete;
  PpmWriter& operator=(const PpmWriter&) = delete;
  ~PpmWriter();

  // Appends |row_count| rows of |width| pixels each (RGBA8888, no padding).
  base::Result<void> AppendRows(const std::uint8_t* rgba, int row_count);

  // Flushes, closes and renames the temporary file into place.  Fails when
  // fewer than |height| rows were appended or an I/O error occurred.
  base::Result<void> Finish();

private:
  PpmWriter() = default;

  // Closes the stream and, unless Finish() already renamed the file, removes
  // the temporary file.
  void Close();

  std::FILE* file_ = nullptr;
  std::string final_path_;
  std::string temp_path_;
  std::vector<std::uint8_t> row_buffer_;
  int width_ = 0;
  int height_ = 0;
  int rows_written_ = 0;
  bool finished_ = false;
};

} // namespace neko::paint
