#pragma once

#include <cstddef>
#include <span>

namespace neko::security {

[[nodiscard]] bool FillRandomBytes(std::span<unsigned char> bytes);

} // namespace neko::security