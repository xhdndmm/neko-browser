#pragma once

#include "neko/base/status.h"
#include "neko/ipc/channel.h"

#include <cstdint>
#include <string>
#include <vector>

namespace neko::ipc {

// A child process whose stdin/stdout are the other end of a Channel
// (ADR 0016).  The child attaches its own stdin/stdout (the parent end stays
// with the returned channel), so the pair forms a bidirectional byte stream.
//
// Platform: POSIX fork+exec with pipe()/dup2; Windows CreateProcess with
// anonymous pipes (SetHandleInformation inheritance + quoted command line).
class Subprocess
{
public:
  Subprocess() = default;
  Subprocess(const Subprocess&) = delete;
  Subprocess& operator=(const Subprocess&) = delete;
  Subprocess(Subprocess&& other) noexcept;
  Subprocess& operator=(Subprocess&& other) noexcept;
  ~Subprocess();

  // Spawns |argv| (argv[0] = executable path).  On success the child runs
  // with its stdin/stdout redirected to the returned channel.
  static base::Result<Subprocess> Spawn(const std::vector<std::string>& argv);

  Channel& channel()
  {
    return channel_;
  }

  // Blocks until the child exits; returns its exit code (negated signal
  // number on POSIX when killed by a signal).
  int Wait();

  // Asks the child to terminate (SIGTERM / TerminateProcess).  No-op when
  // not running.
  void Terminate();

private:
  void CloseProcessHandle();

  Channel channel_;
#ifdef _WIN32
  void* process_handle_ = nullptr;
#else
  int pid_ = -1;
#endif
};

} // namespace neko::ipc
