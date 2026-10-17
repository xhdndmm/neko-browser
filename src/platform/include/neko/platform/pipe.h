#pragma once

// OS pipe primitives (AGENTS.md §11: Win32/POSIX calls live in the platform
// layer).  Frame encoding and channel semantics stay in neko::ipc; this
// module only moves bytes through OS handles.

#include <cstddef>
#include <cstdint>

namespace neko::platform {

#if defined(_WIN32)
using PipeHandle = void*; // HANDLE
#else
using PipeHandle = int;
#endif

PipeHandle InvalidPipeHandle();
bool PipeHandleValid(PipeHandle handle);

// The two ends of one anonymous pipe: |read| receives what |write| sends.
struct PipePair
{
  PipeHandle read = InvalidPipeHandle();
  PipeHandle write = InvalidPipeHandle();
};

// Creates an anonymous pipe pair.  The ends are inheritable (so a child
// process can be given one of them as its stdio).
bool CreateAnonymousPipe(PipePair* out);

// This process's stdio ends as a channel pair: read from stdin, write to
// stdout (the child side of the spawn protocol; the parent redirected them
// to the pipe).
PipePair StandardIoPipes();

// Reads exactly |n| bytes into |buf|.  Returns false on EOF/error.  On a
// clean EOF before the first byte, sets |clean_eof| so the caller can
// distinguish "peer closed between frames" from "truncated frame".
bool PipeReadExactly(PipeHandle handle, std::uint8_t* buf, std::size_t n, bool* clean_eof);

// Writes exactly |n| bytes.  Returns false on error or a zero-length write.
bool PipeWriteAll(PipeHandle handle, const std::uint8_t* buf, std::size_t n);

// Closes |handle| if valid.
void ClosePipe(PipeHandle handle);

} // namespace neko::platform
