// JPEG decoding via libjpeg, wrapped behind the neko::image::Image
// interface (see docs/development/dependency-policy.md: libjpeg is treated
// as general-purpose codec infrastructure, not browser logic).
//
// Baseline and progressive JPEGs are decoded to 8-bit RGB and converted to
// RGBA.  Errors surface through libjpeg's error handler (setjmp/longjmp)
// and are converted to base::Error.

#include <csetjmp>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <jpeglib.h>

#include "neko/base/status.h"
#include "neko/image/image.h"

namespace neko::image {
namespace {

struct JpegErrorMgr {
  jpeg_error_mgr pub;
  std::jmp_buf setjmp_buffer;
  std::string message;
};

// Called by libjpeg on any fatal error.  Must be a plain function with C
// linkage so it can be invoked through the jpeg_error_mgr function pointer.
extern "C" void JpegErrorExit(j_common_ptr cinfo) {
  auto* err = reinterpret_cast<JpegErrorMgr*>(cinfo->err);
  char buffer[JMSG_LENGTH_MAX];
  (*cinfo->err->format_message)(cinfo, buffer);
  err->message = buffer;
  std::longjmp(err->setjmp_buffer, 1);
}

}  // namespace

bool IsJpeg(std::string_view data) {
  return data.size() >= 3 && static_cast<uint8_t>(data[0]) == 0xFF &&
         static_cast<uint8_t>(data[1]) == 0xD8 && static_cast<uint8_t>(data[2]) == 0xFF;
}

base::Result<Image> DecodeJpeg(std::string_view data) {
  if (!IsJpeg(data)) {
    return base::Error::InvalidArgument("not a JPEG file");
  }

  jpeg_decompress_struct cinfo{};
  JpegErrorMgr jerr{};
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = JpegErrorExit;

  if (setjmp(jerr.setjmp_buffer) != 0) {
    // Error path: libjpeg has longjmp'd here after emitting its message.
    jpeg_destroy_decompress(&cinfo);
    return base::Error::Parse("jpeg: " + jerr.message);
  }

  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, reinterpret_cast<unsigned char*>(const_cast<char*>(data.data())),
               static_cast<unsigned long>(data.size()));
  jpeg_read_header(&cinfo, TRUE);

  cinfo.out_color_space = JCS_RGB;
  jpeg_start_decompress(&cinfo);

  Image out;
  out.width = static_cast<int>(cinfo.output_width);
  out.height = static_cast<int>(cinfo.output_height);
  if (out.width <= 0 || out.height <= 0) {
    jpeg_destroy_decompress(&cinfo);
    return base::Error::Parse("jpeg: invalid dimensions");
  }
  out.rgba.assign(static_cast<size_t>(out.width) * static_cast<size_t>(out.height) * 4, 0);

  std::vector<unsigned char> row(static_cast<size_t>(out.width) * 3);
  while (cinfo.output_scanline < cinfo.output_height) {
    unsigned char* row_ptr = row.data();
    if (jpeg_read_scanlines(&cinfo, &row_ptr, 1) != 1) {
      jpeg_destroy_decompress(&cinfo);
      return base::Error::Parse("jpeg: failed to read scanline");
    }
    const size_t y = cinfo.output_scanline - 1;
    unsigned char* dst = out.rgba.data() + y * static_cast<size_t>(out.width) * 4;
    for (int x = 0; x < out.width; ++x) {
      dst[x * 4 + 0] = row[static_cast<size_t>(x) * 3 + 0];
      dst[x * 4 + 1] = row[static_cast<size_t>(x) * 3 + 1];
      dst[x * 4 + 2] = row[static_cast<size_t>(x) * 3 + 2];
      dst[x * 4 + 3] = 255;
    }
  }

  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  return out;
}

}  // namespace neko::image
