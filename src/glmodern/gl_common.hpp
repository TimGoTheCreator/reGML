#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace regml::glmodern {

struct Vertex {
    float x = 0.0f;
    float y = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    std::uint32_t color = 0xffffffffU; // RGBA8: R in low byte (byte 0 on LE)
};

[[nodiscard]] constexpr std::uint32_t make_rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) noexcept {
    if constexpr (std::endian::native == std::endian::big) {
        return (static_cast<std::uint32_t>(r) << 24) |
               (static_cast<std::uint32_t>(g) << 16) |
               (static_cast<std::uint32_t>(b) << 8) |
               static_cast<std::uint32_t>(a);
    } else {
        return static_cast<std::uint32_t>(r) |
               (static_cast<std::uint32_t>(g) << 8) |
               (static_cast<std::uint32_t>(b) << 16) |
               (static_cast<std::uint32_t>(a) << 24);
    }
}

// GML colors are 0x00BBGGRR
[[nodiscard]] constexpr std::uint32_t gml_to_rgba(std::uint32_t bgr, double alpha = 1.0) noexcept {
    const auto r = static_cast<std::uint8_t>(bgr & 0xffU);
    const auto g = static_cast<std::uint8_t>((bgr >> 8) & 0xffU);
    const auto b = static_cast<std::uint8_t>((bgr >> 16) & 0xffU);
    const auto a = static_cast<std::uint8_t>(std::clamp(alpha, 0.0, 1.0) * 255.0 + 0.5);
    return make_rgba(r, g, b, a);
}

[[nodiscard]] constexpr std::uint64_t hash_combine_key(std::uint64_t key, int x, int y) noexcept {
    return key ? ((key ^ static_cast<unsigned>(x)) * 1099511628211ULL ^ static_cast<unsigned>(y)) : 0;
}

struct Mat4 {
    std::array<float, 16> m{};

    static Mat4 ortho_2d(float width, float height) noexcept {
        Mat4 res{};
        res.m[0] = 2.0f / width;
        res.m[5] = -2.0f / height;
        res.m[10] = -1.0f;
        res.m[12] = -1.0f;
        res.m[13] = 1.0f;
        res.m[15] = 1.0f;
        return res;
    }

    static Mat4 ortho(float left, float right, float bottom, float top, float near_val = -1.0f, float far_val = 1.0f) noexcept {
        Mat4 res{};
        const float rml = right - left;
        const float tmb = top - bottom;
        const float fmn = far_val - near_val;

        res.m[0] = 2.0f / rml;
        res.m[5] = 2.0f / tmb;
        res.m[10] = -2.0f / fmn;
        res.m[12] = -(right + left) / rml;
        res.m[13] = -(top + bottom) / tmb;
        res.m[14] = -(far_val + near_val) / fmn;
        res.m[15] = 1.0f;
        return res;
    }
};

} // namespace regml::glmodern
