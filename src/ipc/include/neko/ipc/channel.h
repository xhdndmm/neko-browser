#pragma once

#include "neko/base/status.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace neko::ipc {

// One framed message on a Channel: [u32le payload length][payload].
// The frame cap keeps a hostile or corrupted peer from exhausting memory.
inline constexpr std::size_t kMaxFrameBytes = 64u * 1024u * 1024u;

// Encodes |payload| as a framed message (header + payload).  Returns
// InvalidArgument when the payload exceeds the frame cap.
base::Result<std::string> EncodeFrame(std::string_view payload);

// Decodes a framed message.  On success returns the payload; returns
// InvalidArgument for a malformed/truncated header, a frame larger than the
// cap, or a short body.
base::Result<std::string> DecodeFrame(std::string_view frame);

// A blocking byte-stream channel between two processes (ADR 0016).  Reads a
// complete frame on Receive() (EINTR-safe); the caller never sees a partial
// frame.  Not thread-safe; each end is used by one thread.
//
// Platform handles: POSIX pipe descriptors / Windows anonymous pipes.  A
// subprocess attaches its own stdin/stdout as its end of the channel (see
// ipc::Subprocess); the parent keeps the other ends.
class Channel
{
public:
  Channel();
  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;
  Channel(Channel&& other) noexcept;
  Channel& operator=(Channel&& other) noexcept;
  ~Channel();

  // True while at least one direction is open.
  bool open() const;

  // Closes both directions.
  void Close();

  // Writes the whole |payload| as one frame.  Returns false when the peer
  // closed its end.
  bool Send(const std::string& payload);

  // Reads one complete frame (blocking).  Returns:
  //  - Ok(payload) on a full frame
  //  - Error::Io("peer closed") on a clean EOF between frames
  //  - Error::InvalidArgument on a malformed/oversized frame
  base::Result<std::string> Receive();

  // Wraps raw platform handles.  The channel takes ownership and closes
  // them in Close()/destructor; the child side passes its own stdio
  // descriptors, which are reclaimed by the OS at process exit.
#ifdef _WIN32
  using Handle = void*;
#else
  using Handle = int;
#endif
  static bool HandleValid(Handle h);
  static Channel FromHandles(Handle read_handle, Handle write_handle);
  Handle read_handle() const
  {
    return read_handle_;
  }
  Handle write_handle() const
  {
    return write_handle_;
  }

private:
  Handle read_handle_;
  Handle write_handle_;
};

} // namespace neko::ipc
