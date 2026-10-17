// Windows implementation of neko/platform/fs.h.

#include "neko/platform/fs.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <windows.h>

namespace neko::platform {

base::Result<void> MakeOneDirectory(const std::string& path)
{
  // A drive root ("C:") is an existing filesystem root; _mkdir would fail
  // with EACCES.
  if (path.size() == 2 && path[1] == ':') {
    return base::Error();
  }
  if (_mkdir(path.c_str()) != 0 && errno != EEXIST) {
    return base::Error::Io("mkdir(" + path + ") failed: " + std::strerror(errno));
  }
  return base::Error();
}

base::Result<void> ReplaceFileAtomic(const std::string& path, std::string_view content)
{
  // MOVEFILE_WRITE_THROUGH makes the replace durable.
  const std::string tmp = path + ".tmp";
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
  if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    const unsigned long win_err = ::GetLastError();
    std::remove(tmp.c_str());
    return base::Error::Io("rename failed for '" + path + "' (windows error " +
                           std::to_string(win_err) + ")");
  }
  return base::Error();
}

std::FILE* OpenExclusiveBinary(const std::string& path)
{
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
}

bool ReplaceFileOverwrite(const std::string& from, const std::string& to)
{
  return MoveFileExA(
             from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

unsigned long ProcessId()
{
  return static_cast<unsigned long>(GetCurrentProcessId());
}

} // namespace neko::platform
