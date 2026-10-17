// POSIX implementation of neko/platform/process.h.

#include "neko/platform/process.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace neko::platform {

base::Result<ChildProcess> SpawnProcessWithStdio(const std::vector<std::string>& argv)
{
  if (argv.empty()) {
    return base::Err(base::Error::InvalidArgument("empty argv"));
  }
  int to_child[2] = {-1, -1};
  int from_child[2] = {-1, -1};
  if (::pipe(to_child) != 0 || ::pipe(from_child) != 0) {
    const std::string reason = std::strerror(errno);
    if (to_child[0] >= 0) {
      ::close(to_child[0]);
      ::close(to_child[1]);
    }
    if (from_child[0] >= 0) {
      ::close(from_child[0]);
      ::close(from_child[1]);
    }
    return base::Err(base::Error::Io("pipe failed: " + reason));
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    const std::string reason = std::strerror(errno);
    ::close(to_child[0]);
    ::close(to_child[1]);
    ::close(from_child[0]);
    ::close(from_child[1]);
    return base::Err(base::Error::Io("fork failed: " + reason));
  }
  if (pid == 0) {
    // Child: attach the pipes to stdin/stdout, then exec.  execvp keeps the
    // descriptors across exec (no CLOEXEC on plain pipes).
    ::dup2(to_child[0], STDIN_FILENO);
    ::dup2(from_child[1], STDOUT_FILENO);
    ::close(to_child[0]);
    ::close(to_child[1]);
    ::close(from_child[0]);
    ::close(from_child[1]);
    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const std::string& arg : argv) {
      args.push_back(const_cast<char*>(arg.c_str()));
    }
    args.push_back(nullptr);
    ::execvp(args[0], args.data());
    _exit(127); // exec failed
  }

  // Parent: keep its ends, close the child's.
  ::close(to_child[0]);
  ::close(from_child[1]);

  ChildProcess spawned;
  spawned.pid = static_cast<int>(pid);
  spawned.from_child_read = from_child[0];
  spawned.to_child_write = to_child[1];
  return spawned;
}

long ChildProcessId(const ChildProcess& child)
{
  return child.pid < 0 ? 0 : static_cast<long>(child.pid);
}

int WaitForChild(ChildProcess& child)
{
  if (child.pid < 0) {
    return -1;
  }
  int status = 0;
  while (::waitpid(child.pid, &status, 0) < 0) {
    if (errno == EINTR) {
      continue;
    }
    child.pid = -1;
    return -1;
  }
  child.pid = -1;
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return -WTERMSIG(status);
  }
  return -1;
}

void TerminateChild(ChildProcess& child)
{
  if (child.pid >= 0) {
    ::kill(child.pid, SIGTERM);
  }
}

void CloseChildProcess(ChildProcess& child)
{
  ClosePipe(child.to_child_write);
  ClosePipe(child.from_child_read);
  child.to_child_write = InvalidPipeHandle();
  child.from_child_read = InvalidPipeHandle();
  child.pid = -1;
}

} // namespace neko::platform
