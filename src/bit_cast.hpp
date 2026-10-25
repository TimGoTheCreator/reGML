#pragma once

#include <cstring>
#include <type_traits>

namespace regml {
template <class To, class From> [[nodiscard]] inline To bit_cast(const From &source) noexcept {
    static_assert(sizeof(To) == sizeof(From));
    static_assert(std::is_trivially_copyable<To>::value);
    static_assert(std::is_trivially_copyable<From>::value);

    To destination;
    std::memcpy(&destination, &source, sizeof(destination));
    return destination;
}
} // namespace regml
