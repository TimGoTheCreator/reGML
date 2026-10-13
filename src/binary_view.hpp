#pragma once
#include "error.hpp"
#include <bit>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
namespace regml {
// Reads use view-relative offsets; errors use file offsets.
class BinaryView {
public:
    explicit BinaryView(std::span<const std::uint8_t> bytes, std::size_t origin = 0)
        : bytes_(bytes), origin_(origin) {
        if (origin > std::numeric_limits<std::size_t>::max() - bytes.size())
            throw Error(ErrorKind::malformed, "binary view offset overflows");
    }
    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
    [[nodiscard]] std::size_t origin() const noexcept { return origin_; }
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept { return bytes_; }
    void require(std::size_t offset, std::size_t length) const {
        if (offset > size() || length > size() - offset) {
            const auto reported = offset <= std::numeric_limits<std::size_t>::max() - origin_ ? origin_ + offset : origin_;
            throw Error(ErrorKind::malformed, "read exceeds bounded data", reported);
        }
    }
    [[nodiscard]] BinaryView slice(std::size_t offset, std::size_t length) const {
        require(offset, length); return BinaryView(bytes_.subspan(offset, length), origin_ + offset);
    }
    [[nodiscard]] std::uint8_t u8(std::size_t offset) const { require(offset, 1); return bytes_[offset]; }
    [[nodiscard]] std::uint16_t u16(std::size_t offset) const {
        require(offset, 2);
        return static_cast<std::uint16_t>(bytes_[offset] | (static_cast<std::uint16_t>(bytes_[offset + 1]) << 8));
    }
    [[nodiscard]] std::uint32_t u32(std::size_t offset) const {
        require(offset, 4); std::uint32_t value = 0;
        for (std::size_t i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(bytes_[offset + i]) << (8 * i);
        return value;
    }
    [[nodiscard]] std::uint64_t u64(std::size_t offset) const {
        require(offset, 8); std::uint64_t value = 0;
        for (std::size_t i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(bytes_[offset + i]) << (8 * i);
        return value;
    }
    [[nodiscard]] std::int16_t i16(std::size_t offset) const { return std::bit_cast<std::int16_t>(u16(offset)); }
    [[nodiscard]] std::int32_t i32(std::size_t offset) const { return std::bit_cast<std::int32_t>(u32(offset)); }
    [[nodiscard]] std::int64_t i64(std::size_t offset) const { return std::bit_cast<std::int64_t>(u64(offset)); }
    [[nodiscard]] double f64(std::size_t offset) const { return std::bit_cast<double>(u64(offset)); }
    [[nodiscard]] std::string_view text(std::size_t offset, std::size_t length) const {
        require(offset, length); if (length == 0) return {};
        return {reinterpret_cast<const char*>(bytes_.data() + offset), length};
    }
private:
    std::span<const std::uint8_t> bytes_;
    std::size_t origin_;
};
} // namespace regml
