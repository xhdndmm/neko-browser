#pragma once

// Internal helpers shared within the neko::javascript module.
//
// This header is NOT part of the public API: only code inside
// src/javascript/ may include it.  It exposes the raw QuickJS context so the
// DOM binder can install globals and prototypes without leaking QuickJS types
// through neko::javascript's public headers.

namespace neko::javascript {

class ScriptEngine;

// Returns the raw QuickJS JSContext* owned by |engine| (nullptr when the
// engine failed to initialize).  The returned pointer is valid only while
// |engine| is alive and must be used from the engine's own thread.
void* ScriptEngineContext(ScriptEngine& engine);

} // namespace neko::javascript
