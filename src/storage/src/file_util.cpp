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

#if defined(_WIN32)
  if (!MoveFileExA(tmp.c_str(), path_str.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    std::remove(tmp.c_str());
    return base::Error::Io("rename failed for '" + path_str + "'");
  }
#else
  if (::rename(tmp.c_str(), path_str.c_str()) != 0) {
    std::remove(tmp.c_str());
    return base::Error::Io("rename failed for '" + path_str + "': " +
                           std::strerror(errno));
  }
#endif
  return base::Error();
}

}  // namespace neko::storage
