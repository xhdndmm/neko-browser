#pragma once

#include "neko/compositor/gpu_context.h"

#include <memory>

namespace neko::compositor {

// Creates a real OpenGL 3.3 core GPU context through EGL, without requiring a
// window system: the display connection is chosen from the surfaceless
// platform, a GBM render node, or the default display, in that order.
//
// Returns nullptr when EGL/OpenGL is unavailable or no suitable context can be
// created, which keeps the caller's software fallback path honest.
//
// Linux-only today (EGL is the portable mechanism there).  Windows WGL and
// macOS CGL/Metal backends are separate, unimplemented work; this function
// returns nullptr on those platforms.
std::unique_ptr<GpuContext> CreateEglGpuContext();

} // namespace neko::compositor
