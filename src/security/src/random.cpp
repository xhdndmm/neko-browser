#include "neko/security/random.h"

#include <limits>
#include <openssl/rand.h>

namespace neko::security {

bool FillRandomBytes(std::span<unsigned char> bytes)
{
  if (bytes.empty()) {
    return true;
  }
  if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  return RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) == 1;
}

} // namespace neko::security