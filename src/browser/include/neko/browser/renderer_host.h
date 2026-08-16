#pragma once

#include "neko/base/status.h"
#include "neko/browser/renderer_protocol.h"

#include <string>

namespace neko::browser {

// Returns the absolute path of the currently running executable (the
// renderer child is the same binary in `--renderer-child` mode; a dedicated
// renderer executable is a later milestone).
std::string SelfExecutablePath();

// The browser-process side of the renderer IPC (ADR 0016 M1).  Each Load()
// spawns a fresh renderer child (no session reuse yet), sends a
// LoadRequest and blocks for the rendered frame + DOM text.  The child runs
// the full engine pipeline in its own address space, so a crash during
// load/rasterize surfaces as an error here instead of taking the browser
// down.
class RendererHost
{
public:
  explicit RendererHost(std::string executable);

  // Loads |target| at |width|x|height| in a child process.  Returns the
  // child's LoadResult (bitmap + DOM + title), or an Io error when the
  // child failed to spawn, crashed, or produced a malformed reply.
  base::Result<RendererLoadResult> Load(const std::string& target, int width, int height);

private:
  std::string executable_;
};

} // namespace neko::browser
