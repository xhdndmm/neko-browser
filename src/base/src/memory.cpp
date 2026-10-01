#include "neko/base/memory.h"

#if defined(__linux__)
#include <malloc.h>
#endif

#if defined(__APPLE__)
#include <malloc/malloc.h>
#endif

namespace neko::base {

void ReleaseFreeMemory()
{
#if defined(__APPLE__)
  // Release as much free memory as possible back to the OS (goal 0 = no
  // lower bound).
  (void)malloc_zone_pressure_relief(nullptr, 0);
#elif defined(__GLIBC__)
  // glibc-only: musl and other Linux libcs do not implement malloc_trim.
  // The argument is the amount of free space to *keep* at the top of each
  // heap; 0 returns everything that can be given back.
  (void)malloc_trim(0);
#endif
}

} // namespace neko::base
