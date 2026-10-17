// POSIX implementation of neko/platform/fs.h.

#include "neko/platform/fs.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace neko::platform {

base::Result<void> MakeOneDirectory(const std::string& path)
{
  if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
    return base::Error::Io("mkdir(" + path + ") failed: " + std::strerror(errno));
  }
  return base::Error();
}

base::Result<void> ReplaceFileAtomic(const std::string& path, std::string_view content)
{
  // Write to an unpredictable temp file created with O_EXCL (so an
  // attacker-placed symlink cannot be followed), fsync the data and then the
  // parent directory, and rename into place.  fsync before rename makes the
  // replace durable across power loss.
  // Atomic counter: this runs on whichever thread persists state (browser
  // worker, script callbacks), and a shared non-atomic counter++ is a data
  // race (AGENTS.md §10) that can also collide two temp names.
  static std::atomic<unsigned int> counter{0};
  const std::string tmp = path + ".tmp." + std::to_string(::getpid()) + "." +
                          std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) {
    return base::Error::Io("cannot open '" + tmp + "' for writing: " + std::strerror(errno));
  }
  bool ok = true;
  size_t written = 0;
  while (written < content.size()) {
    const ssize_t n = ::write(fd, content.data() + written, content.size() - written);
    if (n < 0) {
      if (errno == EINTR)
        continue;
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
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return base::Error::Io("rename failed for '" + path + "': " + std::strerror(errno));
  }
  // Persist the directory entry (rename durability).
  const size_t dslash = path.find_last_of('/');
  const std::string dir = dslash == std::string::npos ? "." : path.substr(0, dslash);
  const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd >= 0) {
    ::fsync(dfd);
    ::close(dfd);
  }
  return base::Error();
}

std::FILE* OpenExclusiveBinary(const std::string& path)
{
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) {
    return nullptr;
  }
  std::FILE* file = ::fdopen(fd, "wb");
  if (file == nullptr) {
    ::close(fd);
  }
  return file;
}

bool ReplaceFileOverwrite(const std::string& from, const std::string& to)
{
  return std::rename(from.c_str(), to.c_str()) == 0;
}

unsigned long ProcessId()
{
  return static_cast<unsigned long>(::getpid());
}

} // namespace neko::platform
