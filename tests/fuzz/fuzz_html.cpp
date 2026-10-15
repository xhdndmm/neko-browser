// libFuzzer driver for the HTML tokenizer + tree builder.  Build with
// -DNEKO_BUILD_FUZZERS=ON under Clang; see docs/testing/fuzzing.md.

#include "fuzz_targets.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
  neko::fuzz::Html(data, size);
  return 0;
}
