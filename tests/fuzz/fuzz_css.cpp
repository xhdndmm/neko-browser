// libFuzzer driver for the CSS tokenizer + stylesheet parser.  Build with
// -DNEKO_BUILD_FUZZERS=ON under Clang; see docs/testing/fuzzing.md.

#include "fuzz_targets.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
  neko::fuzz::Css(data, size);
  return 0;
}
