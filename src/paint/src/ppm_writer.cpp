#include "neko/paint/ppm_writer.h"

#include "neko/base/status.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace neko::paint {
namespace {

// Opens |path| read/write, creating it exclusively so a pre-existing file --
// or a symlink an attacker placed there -- can neither be followed nor
// clobbered (the same discipline storage::WriteFileAtomic uses for its
// temporary files).  Returns nullptr on failure.
std::FILE* OpenExclusive(const std::string& path)
{
#if defined(_WIN32)
  int fd = -1;
  if (_sopen_s(&fd,
               path.c_str(),
               _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
               _SH_DENYNO,
               _S_IREAD | _S_IWRITE) != 0) {
    return nullptr;
  }
  std::FILE* file = _fdopen(fd, "wb");
  if (file == nullptr) {
    _close(fd);
  }
  return file;
#else
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) {
    return nullptr;
  }
  std::FILE* file = ::fdopen(fd, "wb");
  if (file == nullptr) {
    ::close(fd);
  }
  return file;
#endif
}

// Renames |from| over |to|.  POSIX rename() replaces atomically; on Windows
// std::rename() refuses an existing destination, so MoveFileEx() is used with
// MOVEFILE_REPLACE_EXISTING (same approach as storage::WriteFileAtomic).
bool ReplaceFile(const std::string& from, const std::string& to)
{
#if defined(_WIN32)
  return MoveFileExA(
             from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

// A temporary path next to the destination, unique per process and call.
std::string MakeTempPath(const std::string& path)
{
  static unsigned int counter = 0;
#if defined(_WIN32)
  const unsigned long pid = GetCurrentProcessId();
#else
  const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
  return path + ".tmp." + std::to_string(pid) + "." + std::to_string(counter++);
}

} // namespace

base::Result<PpmWriter> PpmWriter::Create(std::string_view path, int width, int height)
{
  if (width <= 0 || height <= 0) {
    return base::Err(base::Error::InvalidArgument("PpmWriter: non-positive image dimensions"));
  }
  if (path.empty()) {
    return base::Err(base::Error::InvalidArgument("PpmWriter: empty output path"));
  }
  PpmWriter writer;
  writer.width_ = width;
  writer.height_ = height;
  writer.final_path_ = std::string(path);
  writer.temp_path_ = MakeTempPath(writer.final_path_);
  writer.file_ = OpenExclusive(writer.temp_path_);
  if (writer.file_ == nullptr) {
    return base::Err(base::Error::Io("cannot open '" + writer.temp_path_ +
                                     "' for writing: " + std::strerror(errno)));
  }
  if (std::fprintf(writer.file_, "P6\n%d %d\n255\n", width, height) < 0) {
    writer.Close();
    return base::Err(base::Error::Io("write failed: " + writer.temp_path_));
  }
  return writer;
}

PpmWriter::PpmWriter(PpmWriter&& other) noexcept
    : file_(std::exchange(other.file_, nullptr)), final_path_(std::move(other.final_path_)),
      temp_path_(std::move(other.temp_path_)), row_buffer_(std::move(other.row_buffer_)),
      width_(other.width_), height_(other.height_), rows_written_(other.rows_written_),
      finished_(other.finished_)
{}

PpmWriter& PpmWriter::operator=(PpmWriter&& other) noexcept
{
  if (this != &other) {
    Close();
    file_ = std::exchange(other.file_, nullptr);
    final_path_ = std::move(other.final_path_);
    temp_path_ = std::move(other.temp_path_);
    row_buffer_ = std::move(other.row_buffer_);
    width_ = other.width_;
    height_ = other.height_;
    rows_written_ = other.rows_written_;
    finished_ = other.finished_;
  }
  return *this;
}

PpmWriter::~PpmWriter()
{
  Close();
}

void PpmWriter::Close()
{
  if (file_ != nullptr) {
    std::fclose(file_);
    file_ = nullptr;
    if (!finished_) {
      // A partially written file must not be observable under any name.
      std::remove(temp_path_.c_str());
    }
  }
}

base::Result<void> PpmWriter::AppendRows(const std::uint8_t* rgba, int row_count)
{
  if (file_ == nullptr || finished_) {
    return base::Err(base::Error::InvalidArgument("PpmWriter: not open for writing"));
  }
  if (row_count < 0) {
    return base::Err(base::Error::InvalidArgument("PpmWriter: negative row count"));
  }
  if (rows_written_ + row_count > height_) {
    return base::Err(base::Error::InvalidArgument("PpmWriter: more rows than the declared height"));
  }
  if (row_count == 0) {
    return base::Ok();
  }
  if (rgba == nullptr) {
    return base::Err(base::Error::InvalidArgument("PpmWriter: null row pointer"));
  }

  // One reusable row buffer: rows are encoded and flushed as they arrive.
  if (row_buffer_.size() != static_cast<std::size_t>(width_) * 3) {
    row_buffer_.resize(static_cast<std::size_t>(width_) * 3);
  }
  const auto row_bytes = static_cast<std::size_t>(width_) * 4;
  for (int y = 0; y < row_count; ++y) {
    const std::uint8_t* src = rgba + static_cast<std::size_t>(y) * row_bytes;
    std::uint8_t* dst = row_buffer_.data();
    for (int x = 0; x < width_; ++x) {
      const std::uint8_t r = src[0];
      const std::uint8_t g = src[1];
      const std::uint8_t b = src[2];
      const std::uint8_t a = src[3];
      // Composite over white (identical to the encoding WritePpm always used).
      const float alpha = static_cast<float>(a) / 255.0f;
      auto composite = [alpha](std::uint8_t channel) -> std::uint8_t {
        return static_cast<std::uint8_t>(static_cast<float>(channel) * alpha +
                                         255.0f * (1.0f - alpha) + 0.5f);
      };
      *dst++ = composite(r);
      *dst++ = composite(g);
      *dst++ = composite(b);
      src += 4;
    }
    if (std::fwrite(row_buffer_.data(), 1, row_buffer_.size(), file_) != row_buffer_.size()) {
      return base::Err(base::Error::Io("write failed: " + temp_path_));
    }
  }
  rows_written_ += row_count;
  return base::Ok();
}

base::Result<void> PpmWriter::Finish()
{
  if (file_ == nullptr || finished_) {
    return base::Err(base::Error::InvalidArgument("PpmWriter: writer is not open"));
  }
  if (rows_written_ != height_) {
    return base::Err(base::Error::InvalidArgument("PpmWriter: wrote " +
                                                  std::to_string(rows_written_) + " of " +
                                                  std::to_string(height_) + " rows"));
  }
  const bool flushed = std::fflush(file_) == 0;
  const bool closed = std::fclose(file_) == 0;
  file_ = nullptr;
  if (!flushed || !closed) {
    std::remove(temp_path_.c_str());
    return base::Err(base::Error::Io("write failed: " + temp_path_));
  }
  if (!ReplaceFile(temp_path_, final_path_)) {
    std::remove(temp_path_.c_str());
    return base::Err(base::Error::Io("rename failed for '" + final_path_ + "'"));
  }
  finished_ = true;
  return base::Ok();
}

} // namespace neko::paint
