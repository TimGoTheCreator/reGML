#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace regml {

[[nodiscard]] std::string md5_hex(std::span<const std::uint8_t> input);

} // namespace regml
