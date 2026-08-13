#pragma once

#include <cassert>

// Runtime assertions.
//
// NEKO_ASSERT compiles out in Release builds (NDEBUG is defined).  Use it for
// internal invariants that must hold regardless of external input.  For
// invariants that must survive Release builds (e.g. security checks), use an
// explicit if + error return instead.
#define NEKO_ASSERT(condition) assert(condition)

// Marks code that must never be reached.  Lets the compiler optimize and
// helps static analyzers.
#if defined(_MSC_VER)
#define NEKO_UNREACHABLE() __assume(false)
#elif defined(__GNUC__) || defined(__clang__)
#define NEKO_UNREACHABLE() __builtin_unreachable()
#else
#define NEKO_UNREACHABLE() \
  do {                     \
  } while (false)
#endif
