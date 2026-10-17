#pragma once

// OS child-process primitives (AGENTS.md §11: Win32/POSIX calls live in the
// platform layer).  Spawn semantics (argv, exit codes, stdio wiring) are the
// IPC protocol's; this module only performs them.

#include "neko/base/status.h"
#include "neko/platform/pipe.h"

#include <string>
#include <vector>

namespace neko::platform {

// A spawned child whose stdio is wired to this process as a byte channel:
// write |to_child_write| for the child's stdin, read |from_child_read| for
// its stdout.  Portable handle/pid pair; the OS details stay in the
// per-OS implementation.
struct ChildProcess
{
  void* handle = nullptr; // Windows process HANDLE; unused on POSIX
  int pid = -1;           // POSIX pid; unused on Windows
  PipeHandle to_child_write = InvalidPipeHandle();
  PipeHandle from_child_read = InvalidPipeHandle();
};

// Spawns argv[0] with |argv| as its argument list; the child's stdin and
// stdout are the returned pipe ends (the parent keeps only its own ends).
// Closes everything it opened when the spawn fails.
base::Result<ChildProcess> SpawnProcessWithStdio(const std::vector<std::string>& argv);

// Blocks until the child exits; returns its exit code (negated signal
// number on POSIX when killed by a signal).  Consumes the process handle /
// pid, matching Subprocess::Wait's historical behaviour.
int WaitForChild(ChildProcess& child);

// Asks the child to terminate (SIGTERM / TerminateProcess).  No-op when not
// running.
void TerminateChild(ChildProcess& child);

// The child's process id (0 when not running).
long ChildProcessId(const ChildProcess& child);

// Closes the parent's pipe ends and process handle (idempotent).
void CloseChildProcess(ChildProcess& child);

} // namespace neko::platform
