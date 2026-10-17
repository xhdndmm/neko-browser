// POSIX implementation of neko/platform/pipe.h.

#include "neko/platform/pipe.h"

#include <cerrno>
#include <unistd.h>

namespace neko::platform {

PipeHandle InvalidPipeHandle()
{
  return -1;
}

bool PipeHandleValid(PipeHandle handle)
{
  return handle >= 0;
}

PipePair StandardIoPipes()
{
  return PipePair{STDIN_FILENO, STDOUT_FILENO};
}

bool PipeReadExactly(PipeHandle handle, std::uint8_t* buf, std::size_t n, bool* clean_eof)
{
  *clean_eof = false;
  std::size_t done = 0;
  while (done < n) {
    const ssize_t got = ::read(handle, buf + done, n - done);
    if (got == 0) {
      if (done == 0) {
        *clean_eof = true;
      }
      return false;
    }
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    done += static_cast<std::size_t>(got);
  }
  return true;
}

bool PipeWriteAll(PipeHandle handle, const std::uint8_t* buf, std::size_t n)
{
  std::size_t done = 0;
  while (done < n) {
    const ssize_t wrote = ::write(handle, buf + done, n - done);
    if (wrote < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (wrote == 0) {
      return false;
    }
    done += static_cast<std::size_t>(wrote);
  }
  return true;
}

void ClosePipe(PipeHandle handle)
{
  if (PipeHandleValid(handle)) {
    ::close(handle);
  }
}

} // namespace neko::platform
