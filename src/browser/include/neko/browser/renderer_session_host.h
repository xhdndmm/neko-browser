#pragma once

namespace neko::browser {

// Renderer child entry point for a live session (ADR 0016 M2): serves the
// renderer session protocol on stdin/stdout until the browser sends kShutdown
// or closes the pipe, then exits.  The child runs the engine pipeline (HTML
// parse, style, layout, paint, page scripts) in its own address space; the
// browser process owns navigation and the network stack.
//
// Returns the process exit code.
int RunRendererSession();

} // namespace neko::browser
