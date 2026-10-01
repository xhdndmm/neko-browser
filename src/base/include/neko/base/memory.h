#pragma once

namespace neko::base {

// Asks the platform allocator to return free pages to the operating system.
//
// Why this exists: glibc keeps freed blocks in its per-thread arenas, so the
// process RSS does not fall when a large structure (a page's DOM, style
// store, decoded images, frame buffers) is destroyed -- the memory becomes
// reusable but stays resident.  Measured in the GUI: closing a tab that had
// loaded ~126 MiB of page data released only ~1 MiB of RSS without this call,
// and ~107 MiB with it.
//
// Call this after dropping large structures on user-frequency events (tab
// close, navigation replacement).  It walks every allocator arena, which is
// fine at that rate but must not be used per frame.
//
// glibc (Linux): malloc_trim(0).  macOS: malloc_zone_pressure_relief().
// Other platforms (Windows) currently do nothing.
void ReleaseFreeMemory();

} // namespace neko::base
