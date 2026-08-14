#include "neko/storage/file_util.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "neko/base/logging.h"

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace neko::storage {
namespace {

// Recursively creates |dir|.  Empty string or "." succeeds trivially.
base::Result<void> MkdirAll(const std::string& dir) {
  if (dir.empty() || dir == "." || dir == "/") return base::Error();
  // Create the parent first.
  const size_t slash = dir.find_last_of("/\\");
  if (slash != std::string::npos && slash > 0) {
    const std::string parent = dir.substr(0, slash);
    auto r = MkdirAll(parent);
    if (!r) return r;
  }
#if defined(_WIN32)
  if (_mkdir(dir.c_str()) != 0 && errno != EEXIST) {
    return base::Error::Io("mkdir(" + dir + ") failed: " + std::strerror(errno));
  }
#else
  if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
    return base::Error::Io("mkdir(" + dir + ") failed: " + std::strerror(errno));
  }
#endif
  return base::Error();
}

}  // namespace

base::Result<std::string> ReadFile(std::string_view path) {
  FILE* f = std::fopen(std::string(path).c_str(), "rb");
  if (f == nullptr) {
    return base::Error::Io("cannot open '" + std::string(path) + "' for reading");
  }
  std::string data;
  char buf[8192];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    data.append(buf, n);
  }
  const bool failed = std::ferror(f) != 0;
  std::fclose(f);
  if (failed) {
    return base::Error::Io("read failed on '" + std::string(path) + "'");
  }
  return data;
}

base::Result<void> CreateDirectory(std::string_view dir) {
  return MkdirAll(std::string(dir));
}

base::Result<void> WriteFileAtomic(std::string_view path, std::string_view content) {
  const std::string path_str(path);
  const size_t slash = path_str.find_last_of("/\\");
  if (slash != std::string::npos && slash > 0) {
    auto r = MkdirAll(path_str.substr(0, slash));
    if (!r) return r;
  }

#if defined(_WIN32)
  // Windows: MOVEFILE_WRITE_THROUGH makes the replace durable.
  const std::string tmp = path_str + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (f == nullptr) {
    return base::Error::Io("cannot open '" + tmp + "' for writing");
  }
  const size_t written = std::fwrite(content.data(), 1, content.size(), f);
  const bool flush_ok = std::fflush(f) == 0;
  const bool close_ok = std::fclose(f) == 0;
  if (written != content.size() || !flush_ok || !close_ok) {
    std::remove(tmp.c_str());
    return base::Error::Io("failed writing '" + tmp + "'");
  }
  if (!MoveFileExA(tmp.c_str(), path_str.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    std::remove(tmp.c_str());
    return base::Error::Io("rename failed for '" + path_str + "'");
  }
#else
  // POSIX: write to an unpredictable temp file created with O_EXCL (so an
  // attacker-placed symlink cannot be followed), fsync the data and then the
  // parent directory, and rename into place.  fsync before rename makes the
  // replace durable across power loss.
  static unsigned int counter = 0;
  const std::string tmp = path_str + ".tmp." + std::to_string(::getpid()) + "." +
                          std::to_string(counter++);
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) {
    return base::Error::Io("cannot open '" + tmp + "' for writing: " +
                           std::strerror(errno));
  }
  bool ok = true;
  size_t written = 0;
  while (written < content.size()) {
    const ssize_t n = ::write(fd, content.data() + written, content.size() - written);
    if (n < 0) {
      if (errno == EINTR) continue;
      ok = false;
      break;
    }
    written += static_cast<size_t>(n);
  }
  if (ok && ::fsync(fd) != 0) {
    ok = false;
  }
  if (::close(fd) != 0) {
    ok = false;
  }
  if (!ok || written != content.size()) {
    ::unlink(tmp.c_str());
    return base::Error::Io("failed writing '" + tmp + "'");
  }
  if (::rename(tmp.c_str(), path_str.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return base::Error::Io("rename failed for '" + path_str + "': " +
                           std::strerror(errno));
  }
  // Persist the directory entry (rename durability).
  const size_t dslash = path_str.find_last_of('/');
  const std::string dir = dslash == std::string::npos ? "." : path_str.substr(0, dslash);
  const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd >= 0) {
    ::fsync(dfd);
    ::close(dfd);
  }
#endif
  return base::Error();
}

}  // namespace neko::storage
