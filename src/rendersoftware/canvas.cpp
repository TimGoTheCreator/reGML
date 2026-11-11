#include "canvas.hpp"
#include "platform/ps2/gpu.hpp"
#include "platform/wii/gpu.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <fstream>
#include <png.h>
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64) || _M_IX86_FP >= 2
#include <emmintrin.h>
#define REGML_CANVAS_SSE2 1
#else
#define REGML_CANVAS_SSE2 0
#endif
namespace regml {
namespace {
std::uint32_t to_rgb(std::uint32_t bgr) {
    return ((bgr & 0xff) << 16) | (bgr & 0xff00) | ((bgr >> 16) & 0xff);
}
void finite(double value) {
    if (!std::isfinite(value))
        throw Error(ErrorKind::vm, "drawing coordinate must be finite");
}
void blend_rgba_pixel(std::uint32_t &destination, std::uint32_t source) noexcept {
    const unsigned alpha = source >> 24;
    if (!alpha)
        return;
    if (alpha == 255) {
        destination = 0xff000000 | (source & 0x00ffffff);
        return;
    }

    const unsigned destination_alpha = destination >> 24;
    const unsigned inverse = 255 - alpha;
    if (destination_alpha == 255) {
        const unsigned red =
            (((source >> 16) & 255) * alpha + ((destination >> 16) & 255) * inverse + 127) / 255;
        const unsigned green =
            (((source >> 8) & 255) * alpha + ((destination >> 8) & 255) * inverse + 127) / 255;
        const unsigned blue = ((source & 255) * alpha + (destination & 255) * inverse + 127) / 255;
        destination = 0xff000000 | (red << 16) | (green << 8) | blue;
        return;
    }

    const unsigned output_alpha = alpha + (destination_alpha * inverse + 127) / 255;
    const unsigned red = ((source >> 16) & 255) * alpha +
                         ((((destination >> 16) & 255) * destination_alpha * inverse + 127) / 255);
    const unsigned green = ((source >> 8) & 255) * alpha +
                           ((((destination >> 8) & 255) * destination_alpha * inverse + 127) / 255);
    const unsigned blue =
        (source & 255) * alpha + (((destination & 255) * destination_alpha * inverse + 127) / 255);
    destination = (output_alpha << 24) | (((red + output_alpha / 2) / output_alpha) << 16) |
                  (((green + output_alpha / 2) / output_alpha) << 8) |
                  ((blue + output_alpha / 2) / output_alpha);
}
#if REGML_CANVAS_SSE2
bool composite_rgba4(std::uint32_t *destination,
                     const std::uint32_t *source,
                     unsigned tint = 0xffffff) noexcept {
    const auto source_bytes = _mm_loadu_si128(reinterpret_cast<const __m128i *>(source));
    const auto alpha_mask = _mm_set1_epi32(static_cast<int>(0xff000000U));
    const auto source_alpha = _mm_and_si128(source_bytes, alpha_mask);
    const auto zero = _mm_setzero_si128();
    if (_mm_movemask_epi8(_mm_cmpeq_epi32(source_alpha, zero)) == 0xffff)
        return true;

    const auto rgb_mask = _mm_setr_epi16(-1, -1, -1, 0, -1, -1, -1, 0);
    const auto opaque_alpha = _mm_setr_epi16(0, 0, 0, 255, 0, 0, 0, 255);
    auto source_low = _mm_unpacklo_epi8(source_bytes, zero);
    auto source_high = _mm_unpackhi_epi8(source_bytes, zero);
    const bool plain = tint == 0xffffff;
    if (!plain) {
        const auto tint_channels = _mm_setr_epi16(tint & 255, (tint >> 8) & 255, (tint >> 16) & 255, 255,
                                                  tint & 255, (tint >> 8) & 255, (tint >> 16) & 255, 255);
        const auto ones = _mm_set1_epi16(1);
        const auto tint_half = [&](const __m128i source_half) {
            auto tinted = _mm_mullo_epi16(source_half, tint_channels);
            tinted = _mm_add_epi16(tinted, ones);
            tinted = _mm_add_epi16(tinted, _mm_srli_epi16(tinted, 8));
            return _mm_srli_epi16(tinted, 8);
        };
        source_low = tint_half(source_low);
        source_high = tint_half(source_high);
    }

    const auto source_tinted = _mm_packus_epi16(source_low, source_high);
    const auto source_is_opaque = _mm_cmpeq_epi32(_mm_and_si128(source_tinted, alpha_mask), alpha_mask);
    if (_mm_movemask_epi8(source_is_opaque) == 0xffff) {
        _mm_storeu_si128(reinterpret_cast<__m128i *>(destination), source_tinted);
        return true;
    }

    const auto destination_bytes = _mm_loadu_si128(reinterpret_cast<const __m128i *>(destination));
    const auto destination_is_opaque =
        _mm_cmpeq_epi32(_mm_and_si128(destination_bytes, alpha_mask), alpha_mask);
    if (_mm_movemask_epi8(destination_is_opaque) != 0xffff)
        return false;

    const auto rounding = _mm_set1_epi16(128);
    const auto destination_low = _mm_unpacklo_epi8(destination_bytes, zero);
    const auto destination_high = _mm_unpackhi_epi8(destination_bytes, zero);
    const auto blend_half = [&](const __m128i src, const __m128i dst) {
        auto alpha = _mm_shufflelo_epi16(src, _MM_SHUFFLE(3, 3, 3, 3));
        alpha = _mm_shufflehi_epi16(alpha, _MM_SHUFFLE(3, 3, 3, 3));
        const auto inverse = _mm_sub_epi16(_mm_set1_epi16(255), alpha);
        const auto src_rgb = _mm_or_si128(_mm_and_si128(src, rgb_mask), opaque_alpha);
        auto sum = _mm_add_epi16(_mm_mullo_epi16(src_rgb, alpha), _mm_mullo_epi16(dst, inverse));
        sum = _mm_add_epi16(sum, rounding);
        sum = _mm_add_epi16(sum, _mm_srli_epi16(sum, 8));
        return _mm_srli_epi16(sum, 8);
    };
    const auto output_low = blend_half(source_low, destination_low);
    const auto output_high = blend_half(source_high, destination_high);
    const auto output = _mm_packus_epi16(output_low, output_high);
    _mm_storeu_si128(reinterpret_cast<__m128i *>(destination), output);
    return true;
}
void copy_binary_alpha_rgba4(std::uint32_t *destination, const std::uint32_t *source) noexcept {
    const auto source_pixels = _mm_loadu_si128(reinterpret_cast<const __m128i *>(source));
    const auto alpha_mask = _mm_set1_epi32(static_cast<int>(0xff000000U));
    const auto source_alpha = _mm_and_si128(source_pixels, alpha_mask);
    const auto opaque_mask = _mm_cmpeq_epi32(source_alpha, alpha_mask);
    const int opaque_lanes = _mm_movemask_epi8(opaque_mask);
    if (opaque_lanes == 0)
        return;
    if (opaque_lanes == 0xffff) {
        _mm_storeu_si128(reinterpret_cast<__m128i *>(destination), source_pixels);
        return;
    }
    const auto destination_pixels = _mm_loadu_si128(reinterpret_cast<const __m128i *>(destination));
    const auto output = _mm_or_si128(_mm_and_si128(source_pixels, opaque_mask),
                                     _mm_andnot_si128(opaque_mask, destination_pixels));
    _mm_storeu_si128(reinterpret_cast<__m128i *>(destination), output);
}
#endif

// 5x7 glyphs ja
std::array<unsigned, 7> glyph(char c) {
    if (c >= 'a' && c <= 'z')
        c = static_cast<char>(c - 'a' + 'A');
    switch (c) {
    case 'A':
        return {14, 17, 17, 31, 17, 17, 17};
    case 'B':
        return {30, 17, 17, 30, 17, 17, 30};
    case 'C':
        return {14, 17, 16, 16, 16, 17, 14};
    case 'D':
        return {30, 17, 17, 17, 17, 17, 30};
    case 'E':
        return {31, 16, 16, 30, 16, 16, 31};
    case 'F':
        return {31, 16, 16, 30, 16, 16, 16};
    case 'G':
        return {14, 17, 16, 23, 17, 17, 15};
    case 'H':
        return {17, 17, 17, 31, 17, 17, 17};
    case 'I':
        return {14, 4, 4, 4, 4, 4, 14};
    case 'J':
        return {7, 2, 2, 2, 2, 18, 12};
    case 'K':
        return {17, 18, 20, 24, 20, 18, 17};
    case 'L':
        return {16, 16, 16, 16, 16, 16, 31};
    case 'M':
        return {17, 27, 21, 21, 17, 17, 17};
    case 'N':
        return {17, 25, 21, 19, 17, 17, 17};
    case 'O':
        return {14, 17, 17, 17, 17, 17, 14};
    case 'P':
        return {30, 17, 17, 30, 16, 16, 16};
    case 'Q':
        return {14, 17, 17, 17, 21, 18, 13};
    case 'R':
        return {30, 17, 17, 30, 20, 18, 17};
    case 'S':
        return {15, 16, 16, 14, 1, 1, 30};
    case 'T':
        return {31, 4, 4, 4, 4, 4, 4};
    case 'U':
        return {17, 17, 17, 17, 17, 17, 14};
    case 'V':
        return {17, 17, 17, 17, 17, 10, 4};
    case 'W':
        return {17, 17, 17, 21, 21, 21, 10};
    case 'X':
        return {17, 17, 10, 4, 10, 17, 17};
    case 'Y':
        return {17, 17, 10, 4, 4, 4, 4};
    case 'Z':
        return {31, 1, 2, 4, 8, 16, 31};
    case '0':
        return {14, 17, 19, 21, 25, 17, 14};
    case '1':
        return {4, 12, 4, 4, 4, 4, 14};
    case '2':
        return {14, 17, 1, 2, 4, 8, 31};
    case '3':
        return {30, 1, 1, 14, 1, 1, 30};
    case '4':
        return {2, 6, 10, 18, 31, 2, 2};
    case '5':
        return {31, 16, 16, 30, 1, 1, 30};
    case '6':
        return {14, 16, 16, 30, 17, 17, 14};
    case '7':
        return {31, 1, 2, 4, 8, 8, 8};
    case '8':
        return {14, 17, 17, 14, 17, 17, 14};
    case '9':
        return {14, 17, 17, 15, 1, 1, 14};
    case ':':
        return {0, 4, 4, 0, 4, 4, 0};
    case '.':
        return {0, 0, 0, 0, 0, 6, 6};
    case '/':
        return {1, 2, 2, 4, 8, 8, 16};
    case '-':
        return {0, 0, 0, 31, 0, 0, 0};
    case '_':
        return {0, 0, 0, 0, 0, 0, 31};
    case '>':
        return {16, 8, 4, 2, 4, 8, 16};
    case '+':
        return {0, 4, 4, 31, 4, 4, 0};
    case ' ':
        return {};
    default:
        return {14, 17, 1, 2, 4, 0, 4};
    }
}
} // namespace
std::array<unsigned, 7> font_bitmap(unsigned char character) {
    return glyph(static_cast<char>(character));
}
Canvas::Canvas(int width, int height, bool main_screen)
    : logical_width_(width), logical_height_(height), width_(width), height_(height) {
#if REGML_240P
    if (main_screen) {
        render_scale_ = coordinate_scale_ = 0.5;
        width_ = (width + 1) / 2;
        height_ = (height + 1) / 2;
    }
#else
    (void)main_screen;
#endif
    if (width < 1 || height < 1 || width > 4096 || height > 4096)
        throw Error(ErrorKind::unsupported, "framebuffer dimensions must be between 1 and 4096");
    pixels_.resize(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_), 0xff000000);
}
void Canvas::gpu_target(bool enabled) {
#if REGML_WII || REGML_PS2
    if (enabled == gpu_)
        return;
    if (enabled) {
        std::vector<std::uint32_t>().swap(pixels_);
    } else {
        pixels_.resize(static_cast<std::size_t>(width_) * height_, 0xff000000);
#if REGML_PS2
        if (!ps2_gpu_readback(pixels_))
            std::fill(pixels_.begin(), pixels_.end(), 0xff000000);
#endif
    }
    gpu_ = enabled;
#else
    (void)enabled;
#endif
}
std::uint32_t Canvas::pixel(int x, int y) const {
    if (x < 0 || y < 0 || x >= width_ || y >= height_)
        throw Error(ErrorKind::usage, "pixel is outside framebuffer");
#if REGML_WII
    if (gpu_)
        return wii_gpu_pixel(x, y);
#elif REGML_PS2
    if (gpu_)
        return ps2_gpu_pixel(x, y);
#endif
    return pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
                   static_cast<std::size_t>(x)];
}
void Canvas::clear(std::uint32_t bgr) {
#if REGML_WII
    if (gpu_) {
        wii_gpu_begin();
        wii_gpu_box(0, 0, width_, height_, 0xff000000 | to_rgb(bgr));
        return;
    }
#elif REGML_PS2
    if (gpu_) {
        ps2_gpu_box(0, 0, width_, height_, 0xff000000 | to_rgb(bgr));
        return;
    }
#endif
    const auto rgba = 0xff000000 | to_rgb(bgr);
    if (color_write_all()) {
        std::fill(pixels_.begin(), pixels_.end(), rgba);
        return;
    }
    for (int y = 0; y < height_; ++y)
        for (int x = 0; x < width_; ++x)
            store_pixel(x, y, rgba);
}
void Canvas::clear_transparent() noexcept {
    std::fill(pixels_.begin(), pixels_.end(), 0);
}
void Canvas::rgba_pixel(int x, int y, std::uint32_t rgba, std::uint32_t bgr, double opacity) {
    finite(opacity);
    rgba_pixel_tinted(x, y, rgba, to_rgb(bgr), opacity);
}
void Canvas::store_pixel(int x, int y, std::uint32_t rgba) noexcept {
    auto &dest =
        pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x)];
    if (color_write_all()) {
        dest = rgba;
        return;
    }
    constexpr std::array<std::uint32_t, 4> channel_masks{0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000};
    std::uint32_t enabled = 0;
    for (std::size_t channel = 0; channel < color_write_.size(); ++channel)
        if (color_write_[channel])
            enabled |= channel_masks[channel];
    dest = (rgba & enabled) | (dest & ~enabled);
}
void Canvas::rgba_pixel_tinted(int x, int y, std::uint32_t rgba, unsigned tint, double opacity) noexcept {
    if (x < 0 || y < 0 || x >= width_ || y >= height_)
        return;
    if (!gpu_ && pixel_shader_)
        rgba = pixel_shader_(pixel_shader_context_, x, y, rgba);
    const unsigned source_alpha =
        opacity == 1
            ? rgba >> 24
            : static_cast<unsigned>(static_cast<double>(rgba >> 24) * std::clamp(opacity, 0.0, 1.0) + 0.5);
    if (alpha_test_enabled_ && source_alpha <= alpha_test_ref_)
        return;
#if REGML_WII
    if (gpu_) {
        unsigned rgb = 0;
        for (unsigned shift : {0U, 8U, 16U})
            rgb |= ((rgba >> shift & 255) * (tint >> shift & 255) / 255) << shift;
        wii_gpu_box(x, y, 1, 1, (source_alpha << 24) | rgb);
        return;
    }
#elif REGML_PS2
    if (gpu_) {
        unsigned rgb = 0;
        for (unsigned shift : {0U, 8U, 16U})
            rgb |= ((rgba >> shift & 255) * (tint >> shift & 255) / 255) << shift;
        ps2_gpu_box(x, y, 1, 1, (source_alpha << 24) | rgb);
        return;
    }
#endif
    if (!blend_enabled_) {
        const unsigned red = ((rgba >> 16) & 255) * ((tint >> 16) & 255) / 255;
        const unsigned green = ((rgba >> 8) & 255) * ((tint >> 8) & 255) / 255;
        const unsigned blue = (rgba & 255) * (tint & 255) / 255;
        store_pixel(x, y, (source_alpha << 24) | (red << 16) | (green << 8) | blue);
        return;
    }
    auto &dest =
        pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x)];
    if (!standard_blend()) {
        const unsigned red = ((rgba >> 16) & 255) * ((tint >> 16) & 255) / 255;
        const unsigned green = ((rgba >> 8) & 255) * ((tint >> 8) & 255) / 255;
        const unsigned blue = (rgba & 255) * (tint & 255) / 255;
        const std::array<unsigned, 4> source = {red, green, blue, source_alpha};
        const std::array<unsigned, 4> destination = {(dest >> 16) & 255, (dest >> 8) & 255, dest & 255,
                                                     dest >> 24};
        auto factor = [&](int type, std::size_t channel) -> unsigned {
            const unsigned src = source[channel], dst = destination[channel];
            // These are the 1-based GML bm_* factor constants.
            switch (type) {
            case 1:
                return 0;
            case 2:
                return 255;
            case 3:
                return src;
            case 4:
                return 255 - src;
            case 5:
                return source_alpha;
            case 6:
                return 255 - source_alpha;
            case 7:
                return destination[3];
            case 8:
                return 255 - destination[3];
            case 9:
                return dst;
            case 10:
                return 255 - dst;
            case 11:
                return channel == 3 ? 255 : std::min(source_alpha, 255 - destination[3]);
            default:
                return 0;
            }
        };
        unsigned result = 0;
        constexpr std::array<unsigned, 4> shifts{16, 8, 0, 24};
        for (std::size_t channel = 0; channel < source.size(); ++channel) {
            const int equation = blend_equations_[channel == 3 ? 1 : 0];
            unsigned output = 0;
            if (equation == 3)
                output = std::min(source[channel], destination[channel]);
            else if (equation == 4)
                output = std::max(source[channel], destination[channel]);
            else {
                const auto source_factor = factor(blend_factors_[channel == 3 ? 2 : 0], channel);
                const auto destination_factor = factor(blend_factors_[channel == 3 ? 3 : 1], channel);
                const auto source_term = (source[channel] * source_factor + 127) / 255;
                const auto destination_term = (destination[channel] * destination_factor + 127) / 255;
                if (equation == 1)
                    output = destination_term > source_term ? destination_term - source_term : 0;
                else if (equation == 2)
                    output = source_term > destination_term ? source_term - destination_term : 0;
                else
                    output = std::min(source_term + destination_term, 255U);
            }
            result |= std::min(output, 255U) << shifts[channel];
        }
        store_pixel(x, y, result);
        return;
    }
    if (source_alpha == 0)
        return;
    if (source_alpha == 255) {
        if (tint == 0xffffff) {
            store_pixel(x, y, rgba);
            return;
        }
        const unsigned red = ((rgba >> 16) & 255) * ((tint >> 16) & 255) / 255;
        const unsigned green = ((rgba >> 8) & 255) * ((tint >> 8) & 255) / 255;
        const unsigned blue = (rgba & 255) * (tint & 255) / 255;
        store_pixel(x, y, 0xff000000 | (red << 16) | (green << 8) | blue);
        return;
    }
    const unsigned destination_alpha = dest >> 24;
    if (destination_alpha == 255) {
        const unsigned inverse = 255 - source_alpha;
        const unsigned red = ((rgba >> 16) & 255) * ((tint >> 16) & 255) / 255;
        const unsigned green = ((rgba >> 8) & 255) * ((tint >> 8) & 255) / 255;
        const unsigned blue = (rgba & 255) * (tint & 255) / 255;
        const unsigned previous_red = (dest >> 16) & 255;
        const unsigned previous_green = (dest >> 8) & 255;
        const unsigned previous_blue = dest & 255;
        const unsigned output_red = (red * source_alpha + previous_red * inverse + 127) / 255;
        const unsigned output_green = (green * source_alpha + previous_green * inverse + 127) / 255;
        const unsigned output_blue = (blue * source_alpha + previous_blue * inverse + 127) / 255;
        store_pixel(x, y, 0xff000000 | (output_red << 16) | (output_green << 8) | output_blue);
        return;
    }
    const unsigned output_alpha = source_alpha + (destination_alpha * (255 - source_alpha) + 127) / 255;
    unsigned result = output_alpha << 24;
    for (unsigned shift : {0U, 8U, 16U}) {
        const unsigned source = ((rgba >> shift) & 255) * ((tint >> shift) & 255) / 255;
        const unsigned previous = (dest >> shift) & 255;
        const unsigned premultiplied =
            source * source_alpha + (previous * destination_alpha * (255 - source_alpha) + 127) / 255;
        result |= ((premultiplied + output_alpha / 2) / output_alpha) << shift;
    }
    store_pixel(x, y, result);
}
void Canvas::rgba_span(int x, int y, int count, std::uint32_t rgba, unsigned tint, double opacity) noexcept {
    if (y < 0 || y >= height_ || count <= 0 || x >= width_ || opacity <= 0)
        return;
    const int end =
        static_cast<int>(std::min(static_cast<std::int64_t>(x) + count, static_cast<std::int64_t>(width_)));
    x = std::max(x, 0);
    if (x >= end)
        return;
    if (!color_write_all() || !blend_enabled_ || alpha_test_enabled_ || !standard_blend() || pixel_shader_) {
        for (int px = x; px < end; ++px)
            rgba_pixel_tinted(px, y, rgba, tint, opacity);
        return;
    }
    const bool plain = tint == 0xffffff && opacity == 1;
    const unsigned alpha =
        plain ? rgba >> 24
              : static_cast<unsigned>(static_cast<double>(rgba >> 24) * std::clamp(opacity, 0.0, 1.0) + 0.5);
    if (!alpha)
        return;
    const unsigned red = plain ? (rgba >> 16) & 255 : ((rgba >> 16) & 255) * ((tint >> 16) & 255) / 255;
    const unsigned green = plain ? (rgba >> 8) & 255 : ((rgba >> 8) & 255) * ((tint >> 8) & 255) / 255;
    const unsigned blue = plain ? rgba & 255 : (rgba & 255) * (tint & 255) / 255;
#if REGML_WII
    if (gpu_) {
        wii_gpu_box(x, y, end - x, 1, (alpha << 24) | (red << 16) | (green << 8) | blue);
        return;
    }
#elif REGML_PS2
    if (gpu_) {
        ps2_gpu_box(x, y, end - x, 1, (alpha << 24) | (red << 16) | (green << 8) | blue);
        return;
    }
#endif
    auto *row = pixels_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width_);
    if (alpha == 255) {
        std::fill(row + x, row + end, 0xff000000 | (red << 16) | (green << 8) | blue);
        return;
    }
    const auto source_pixel = (alpha << 24) | (red << 16) | (green << 8) | blue;
    const std::array<std::uint32_t, 4> source_pixels{source_pixel, source_pixel, source_pixel, source_pixel};
    int vector_x = x;
#if REGML_CANVAS_SSE2
    for (; vector_x + 4 <= end; vector_x += 4) {
        if (!composite_rgba4(row + vector_x, source_pixels.data()))
            for (int pixel = 0; pixel < 4; ++pixel)
                blend_rgba_pixel(row[vector_x + pixel], source_pixel);
    }
#endif
    for (; vector_x < end; ++vector_x)
        blend_rgba_pixel(row[vector_x], source_pixel);
}
void Canvas::blit_rgba(std::span<const std::uint32_t> source,
                       int source_width,
                       int source_x,
                       int source_y,
                       int destination_x,
                       int destination_y,
                       int width,
                       int height,
                       unsigned tint,
                       double opacity,
                       std::int8_t source_alpha_mode) noexcept {
    if (opacity <= 0)
        return;
    if (!color_write_all() || !blend_enabled_ || alpha_test_enabled_ || !standard_blend()) {
        for (int row = 0; row < height; ++row)
            for (int column = 0; column < width; ++column)
                rgba_pixel_tinted(
                    destination_x + column, destination_y + row,
                    source[static_cast<std::size_t>(source_y + row) * static_cast<std::size_t>(source_width) +
                           static_cast<std::size_t>(source_x + column)],
                    tint, opacity);
        return;
    }
#if REGML_PS2
    if (gpu_) {
        ps2_gpu_image(source, source_width, source_x, source_y, width, height, destination_x, destination_y,
                      width, height, 0, tint, opacity);
        return;
    }
#endif
    const bool plain = tint == 0xffffff && opacity == 1;
    const double clamped_opacity = std::clamp(opacity, 0.0, 1.0);
    const unsigned tint_red = (tint >> 16) & 255;
    const unsigned tint_green = (tint >> 8) & 255;
    const unsigned tint_blue = tint & 255;
    const auto transform_pixel = [&](const std::uint32_t pixel) {
        const unsigned alpha =
            opacity == 1 ? pixel >> 24 : static_cast<unsigned>((pixel >> 24) * clamped_opacity + 0.5);
        if (!alpha)
            return 0U;
        const unsigned red = plain ? (pixel >> 16) & 255 : ((pixel >> 16) & 255) * tint_red / 255;
        const unsigned green = plain ? (pixel >> 8) & 255 : ((pixel >> 8) & 255) * tint_green / 255;
        const unsigned blue = plain ? pixel & 255 : (pixel & 255) * tint_blue / 255;
        return (alpha << 24) | (red << 16) | (green << 8) | blue;
    };
    const auto prepare_vector_pixel = [&](const std::uint32_t pixel) {
        const unsigned alpha =
            opacity == 1 ? pixel >> 24 : static_cast<unsigned>((pixel >> 24) * clamped_opacity + 0.5);
        return alpha ? (alpha << 24) | (pixel & 0x00ffffff) : 0U;
    };
    for (int row = 0; row < height; ++row) {
        const auto *src = source.data() +
                          static_cast<std::size_t>(source_y + row) * static_cast<std::size_t>(source_width) +
                          static_cast<std::size_t>(source_x);
        auto *dst = pixels_.data() +
                    static_cast<std::size_t>(destination_y + row) * static_cast<std::size_t>(width_) +
                    static_cast<std::size_t>(destination_x);
        if (plain && source_alpha_mode == 2) {
            std::copy_n(src, width, dst);
            continue;
        }
        if (plain && source_alpha_mode == 1) {
            int column = 0;
#if REGML_CANVAS_SSE2
            for (; column + 4 <= width; column += 4)
                copy_binary_alpha_rgba4(dst + column, src + column);
#endif
            while (column < width) {
                if ((src[column] >> 24) == 255) {
                    dst[column] = src[column];
                } else {
                    // Transparent source pixels leave the destination unchanged.
                }
                ++column;
            }
            continue;
        }
        if (!pixel_shader_) {
            int column = 0;
#if REGML_CANVAS_SSE2
            std::array<std::uint32_t, 4> transformed{};
            for (; column + static_cast<int>(transformed.size()) <= width;
                 column += static_cast<int>(transformed.size())) {
                if (clamped_opacity == 1) {
                    for (std::size_t lane = 0; lane < transformed.size(); ++lane)
                        transformed[lane] = src[column + static_cast<int>(lane)];
                } else {
                    for (std::size_t lane = 0; lane < transformed.size(); ++lane)
                        transformed[lane] = prepare_vector_pixel(src[column + static_cast<int>(lane)]);
                }
                if (!composite_rgba4(dst + column, transformed.data(), tint))
                    for (std::size_t lane = 0; lane < transformed.size(); ++lane)
                        blend_rgba_pixel(dst[column + static_cast<int>(lane)],
                                         transform_pixel(src[column + static_cast<int>(lane)]));
            }
#endif
            for (; column < width; ++column)
                blend_rgba_pixel(dst[column], transform_pixel(src[column]));
            continue;
        }
        for (int column = 0; column < width; ++column) {
            const auto pixel = src[column];
            const auto alpha = pixel >> 24;
            if (alpha == 0)
                continue;
            if (plain && alpha == 255) {
                dst[column] = pixel;
                continue;
            }
            if (plain && (dst[column] >> 24) == 255) {
                const unsigned inverse = 255 - alpha;
                const auto previous = dst[column];
                unsigned result = 0xff000000;
                for (unsigned shift : {0U, 8U, 16U})
                    result |=
                        (((pixel >> shift & 255) * alpha + (previous >> shift & 255) * inverse + 127) / 255)
                        << shift;
                dst[column] = result;
                continue;
            }
            rgba_pixel_tinted(destination_x + column, destination_y + row, pixel, tint, opacity);
        }
    }
}
bool Canvas::blit_rgba_scaled_opaque(std::span<const std::uint32_t> source,
                                     int source_width,
                                     std::span<const int> source_x,
                                     std::span<const int> source_y,
                                     int destination_x,
                                     int destination_y,
                                     double opacity) noexcept {
    if (gpu_ || !color_write_all() || !blend_enabled_ || alpha_test_enabled_ || !standard_blend() ||
        pixel_shader_ || source_width <= 0 || destination_x < 0 || destination_y < 0 ||
        destination_x > width_ || destination_y > height_ || opacity <= 0 ||
        source_x.size() > static_cast<std::size_t>(width_ - destination_x) ||
        source_y.size() > static_cast<std::size_t>(height_ - destination_y))
        return false;

    for (const int x : source_x)
        if (x < 0 || x >= source_width)
            return false;
    for (const int y : source_y)
        if (y < 0 || static_cast<std::size_t>(y) * static_cast<std::size_t>(source_width) + source_width >
                         source.size())
            return false;

    const unsigned source_alpha = static_cast<unsigned>(255.0 * std::clamp(opacity, 0.0, 1.0) + 0.5);
    if (source_alpha == 0)
        return true;
    const bool repeated_x = std::adjacent_find(source_x.begin(), source_x.end()) != source_x.end();
    const auto process_rows = [&](std::size_t first_row, std::size_t last_row) {
        for (std::size_t row = first_row; row < last_row; ++row) {
            const auto *src = source.data() + static_cast<std::size_t>(source_y[row]) * source_width;
            auto *dst = pixels_.data() +
                        static_cast<std::size_t>(destination_y + static_cast<int>(row)) * width_ +
                        destination_x;
            if (source_alpha == 255) {
                if (repeated_x) {
                    for (std::size_t column = 0; column < source_x.size();) {
                        const int x = source_x[column];
                        std::size_t end = column + 1;
                        while (end < source_x.size() && source_x[end] == x)
                            ++end;
                        std::fill_n(dst + column, end - column, src[x]);
                        column = end;
                    }
                } else {
                    for (std::size_t column = 0; column < source_x.size(); ++column)
                        dst[column] = src[source_x[column]];
                }
                continue;
            }
            std::size_t column = 0;
#if REGML_CANVAS_SSE2
            std::array<std::uint32_t, 4> source_pixels{};
            for (; column + 4 <= source_x.size(); column += 4) {
                for (std::size_t lane = 0; lane < source_pixels.size(); ++lane)
                    source_pixels[lane] = (source_alpha << 24) | (src[source_x[column + lane]] & 0x00ffffff);
                if (!composite_rgba4(dst + column, source_pixels.data()))
                    for (std::size_t lane = 0; lane < source_pixels.size(); ++lane)
                        blend_rgba_pixel(dst[column + lane], source_pixels[lane]);
            }
#endif
            for (; column < source_x.size(); ++column) {
                const auto pixel = src[source_x[column]];
                blend_rgba_pixel(dst[column], (source_alpha << 24) | (pixel & 0x00ffffff));
            }
        }
    };
    process_rows(0, source_y.size());
    return true;
}
bool Canvas::blit_rgba_scaled_binary_alpha(std::span<const std::uint32_t> source,
                                           int source_width,
                                           std::span<const int> source_x,
                                           std::span<const int> source_y,
                                           int destination_x,
                                           int destination_y) noexcept {
    // Binary-alpha sprite pixels can only be transparent or opaque. On the
    // common software path, avoid the full blend-state and channel math for
    // each scaled pixel; transparent samples leave the framebuffer untouched.
    if (gpu_ || !color_write_all() || !blend_enabled_ || alpha_test_enabled_ || !standard_blend() ||
        pixel_shader_ || source_width <= 0 || destination_x < 0 || destination_y < 0 ||
        destination_x > width_ || destination_y > height_ ||
        source_x.size() > static_cast<std::size_t>(width_ - destination_x) ||
        source_y.size() > static_cast<std::size_t>(height_ - destination_y))
        return false;

    for (const int x : source_x)
        if (x < 0 || x >= source_width)
            return false;
    for (const int y : source_y)
        if (y < 0 || static_cast<std::size_t>(y) * static_cast<std::size_t>(source_width) + source_width >
                         source.size())
            return false;

    const bool repeated_x = std::adjacent_find(source_x.begin(), source_x.end()) != source_x.end();
    const auto process_rows = [&](std::size_t first_row, std::size_t last_row) {
        for (std::size_t row = first_row; row < last_row; ++row) {
            const auto *src = source.data() + static_cast<std::size_t>(source_y[row]) * source_width;
            auto *dst = pixels_.data() +
                        static_cast<std::size_t>(destination_y + static_cast<int>(row)) * width_ +
                        destination_x;
            if (repeated_x) {
                for (std::size_t column = 0; column < source_x.size();) {
                    const int x = source_x[column];
                    std::size_t end = column + 1;
                    while (end < source_x.size() && source_x[end] == x)
                        ++end;
                    const auto pixel = src[x];
                    if (pixel >> 24)
                        std::fill_n(dst + column, end - column, pixel);
                    column = end;
                }
                continue;
            }
            std::size_t column = 0;
#if REGML_CANVAS_SSE2
            std::array<std::uint32_t, 4> source_pixels{};
            for (; column + source_pixels.size() <= source_x.size(); column += source_pixels.size()) {
                for (std::size_t lane = 0; lane < source_pixels.size(); ++lane)
                    source_pixels[lane] = src[source_x[column + lane]];
                copy_binary_alpha_rgba4(dst + column, source_pixels.data());
            }
#endif
            for (; column < source_x.size(); ++column) {
                const auto pixel = src[source_x[column]];
                if (pixel >> 24)
                    dst[column] = pixel;
            }
        }
    };
    process_rows(0, source_y.size());
    return true;
}
bool Canvas::blit_rgba_scaled_tinted(std::span<const std::uint32_t> source,
                                     int source_width,
                                     std::span<const int> source_x,
                                     std::span<const int> source_y,
                                     int destination_x,
                                     int destination_y,
                                     unsigned tint,
                                     double opacity) noexcept {
    // This path handles the common software blend state without routing each
    // sample through rgba_pixel_tinted(). Repeated horizontal samples use the
    // run-based span path in GameAssets::blit(), which can reuse the tinted
    // source color across the whole run.
    if (gpu_ || !color_write_all() || !blend_enabled_ || alpha_test_enabled_ || !standard_blend() ||
        pixel_shader_ || source_width <= 0 || destination_x < 0 || destination_y < 0 ||
        destination_x > width_ || destination_y > height_ || opacity <= 0 ||
        source_x.size() > static_cast<std::size_t>(width_ - destination_x) ||
        source_y.size() > static_cast<std::size_t>(height_ - destination_y))
        return false;

    for (const int x : source_x)
        if (x < 0 || x >= source_width)
            return false;
    for (const int y : source_y)
        if (y < 0 || static_cast<std::size_t>(y) * static_cast<std::size_t>(source_width) + source_width >
                         source.size())
            return false;

    const double clamped_opacity = std::clamp(opacity, 0.0, 1.0);
    const bool plain = tint == 0xffffff;
    const unsigned tint_red = (tint >> 16) & 255;
    const unsigned tint_green = (tint >> 8) & 255;
    const unsigned tint_blue = tint & 255;
    const auto transform_pixel = [&](const std::uint32_t pixel) {
        const unsigned alpha =
            clamped_opacity == 1 ? pixel >> 24 : static_cast<unsigned>((pixel >> 24) * clamped_opacity + 0.5);
        if (!alpha)
            return 0U;
        const unsigned red = plain ? (pixel >> 16) & 255 : ((pixel >> 16) & 255) * tint_red / 255;
        const unsigned green = plain ? (pixel >> 8) & 255 : ((pixel >> 8) & 255) * tint_green / 255;
        const unsigned blue = plain ? pixel & 255 : (pixel & 255) * tint_blue / 255;
        return (alpha << 24) | (red << 16) | (green << 8) | blue;
    };
    const auto prepare_vector_pixel = [&](const std::uint32_t pixel) {
        const unsigned alpha =
            clamped_opacity == 1 ? pixel >> 24 : static_cast<unsigned>((pixel >> 24) * clamped_opacity + 0.5);
        return alpha ? (alpha << 24) | (pixel & 0x00ffffff) : 0U;
    };
    const auto process_rows = [&](std::size_t first_row, std::size_t last_row) {
        for (std::size_t row = first_row; row < last_row; ++row) {
            const auto *src = source.data() + static_cast<std::size_t>(source_y[row]) * source_width;
            auto *dst = pixels_.data() +
                        static_cast<std::size_t>(destination_y + static_cast<int>(row)) * width_ +
                        destination_x;
            std::size_t column = 0;
#if REGML_CANVAS_SSE2
            std::array<std::uint32_t, 4> transformed{};
            for (; column + transformed.size() <= source_x.size(); column += transformed.size()) {
                for (std::size_t lane = 0; lane < transformed.size(); ++lane) {
                    const auto pixel = src[source_x[column + lane]];
                    transformed[lane] = clamped_opacity == 1 ? pixel : prepare_vector_pixel(pixel);
                }
                if (!composite_rgba4(dst + column, transformed.data(), tint))
                    for (std::size_t lane = 0; lane < transformed.size(); ++lane)
                        blend_rgba_pixel(dst[column + lane], transform_pixel(src[source_x[column + lane]]));
            }
#endif
            for (; column < source_x.size(); ++column) {
                blend_rgba_pixel(dst[column], transform_pixel(src[source_x[column]]));
            }
        }
    };
    process_rows(0, source_y.size());
    return true;
}
void Canvas::draw_surface(const Canvas &source,
                          double x,
                          double y,
                          double sx,
                          double sy,
                          double angle,
                          std::uint32_t tint,
                          double opacity) {
    x *= render_scale_;
    y *= render_scale_;
    sx *= render_scale_;
    sy *= render_scale_;

    finite(x);
    finite(y);
    finite(sx);
    finite(sy);
    finite(angle);
    finite(opacity);
    if (sx == 0 || sy == 0 || source.width_ == 0 || source.height_ == 0 || opacity <= 0)
        return;
#if REGML_WII
    if (gpu_) {
        wii_gpu_image(source.pixels_, source.width_, 0, 0, source.width_, source.height_, x, y,
                      source.width_ * sx, source.height_ * sy, angle, tint, opacity);
        return;
    }
#elif REGML_PS2
    if (gpu_) {
        if (&source == this) {
            std::vector<std::uint32_t> snapshot(static_cast<std::size_t>(width_) * height_);
            if (!ps2_gpu_readback(snapshot))
                return;
            ps2_gpu_image(snapshot, source.width_, 0, 0, source.width_, source.height_, x, y,
                          source.width_ * sx, source.height_ * sy, angle, to_rgb(tint), opacity);
        } else {
            ps2_gpu_image(source.pixels_, source.width_, 0, 0, source.width_, source.height_, x, y,
                          source.width_ * sx, source.height_ * sy, angle, to_rgb(tint), opacity);
        }
        return;
    }
#endif
    if (render_scale_ != 1) {
        const double radians = angle * 3.141592653589793 / 180, cs = std::cos(radians),
                     sn = std::sin(radians);
        const double w = source.width_ * sx, h = source.height_ * sy;
        const double xx[] = {x, x + w * cs, x + h * sn, x + w * cs + h * sn};
        const double yy[] = {y, y - w * sn, y + h * cs, y - w * sn + h * cs};
        const int left =
            static_cast<int>(std::clamp(std::floor(*std::min_element(xx, xx + 4)), 0.0, double(width_)));
        const int right =
            static_cast<int>(std::clamp(std::ceil(*std::max_element(xx, xx + 4)), 0.0, double(width_)));
        const int top =
            static_cast<int>(std::clamp(std::floor(*std::min_element(yy, yy + 4)), 0.0, double(height_)));
        const int bottom =
            static_cast<int>(std::clamp(std::ceil(*std::max_element(yy, yy + 4)), 0.0, double(height_)));
        const unsigned rgb = to_rgb(tint);
        for (int py = top; py < bottom; ++py)
            for (int px = left; px < right; ++px) {
                const double dx = px + 0.5 - x, dy = py + 0.5 - y;
                const double u = (dx * cs - dy * sn) / sx, v = (dx * sn + dy * cs) / sy;
                if (u >= 0 && v >= 0 && u < source.width_ && v < source.height_)
                    rgba_pixel_tinted(px, py, source.pixel(static_cast<int>(u), static_cast<int>(v)), rgb,
                                      opacity);
            }
        return;
    }
    // use the general path when a blit overlaps itself
    if (color_write_all() && blend_enabled_ && !alpha_test_enabled_ && standard_blend() && this != &source &&
        sy == 1 && angle == 0 && tint == 0xffffff && opacity == 1) {
        // clip first, before converting the coordinates to int
        if (x < -static_cast<double>(source.width_) - 1 || x > width_ + 1.0 ||
            y < -static_cast<double>(source.height_) - 1 || y > height_ + 1.0)
            return;
        const auto origin_x = static_cast<int>(std::lround(x));
        const auto origin_y = static_cast<int>(std::lround(y));
        const int source_x = std::max(0, -origin_x);
        const int source_y = std::max(0, -origin_y);
        const int dest_x = std::max(0, origin_x);
        const int dest_y = std::max(0, origin_y);
        const int copy_width = std::min(source.width_ - source_x, width_ - dest_x);
        const int copy_height = std::min(source.height_ - source_y, height_ - dest_y);
        if (copy_width <= 0 || copy_height <= 0)
            return;

        for (int row = 0; row < copy_height; ++row) {
            const auto *src =
                source.pixels_.data() + static_cast<std::size_t>(source_y + row) * source.width_ + source_x;
            auto *dst = pixels_.data() + static_cast<std::size_t>(dest_y + row) * width_ + dest_x;
            for (int column = 0; column < copy_width; ++column) {
                const auto rgba = src[column];
                const auto alpha = rgba >> 24;
                if (alpha == 255) {
                    dst[column] = rgba;
                } else if (alpha != 0) {
                    const auto previous = dst[column];
                    if ((previous >> 24) != 255) {
                        rgba_pixel_tinted(dest_x + column, dest_y + row, rgba, 0xffffff, 1);
                        continue;
                    }
                    const auto inverse_alpha = 255 - alpha;
                    unsigned result = 0xff000000;
                    for (unsigned shift : {0U, 8U, 16U}) {
                        const auto source_channel = (rgba >> shift) & 255;
                        const auto dest_channel = (previous >> shift) & 255;
                        result |= ((source_channel * alpha + dest_channel * inverse_alpha + 127) / 255)
                                  << shift;
                    }
                    dst[column] = result;
                }
            }
        }
        return;
    }
    const double radians = angle * 3.14159265358979323846 / 180.0;
    const double cs = std::cos(radians), sn = std::sin(radians);
    const unsigned rgb = to_rgb(tint);
    for (int py = 0; py < source.height_; ++py) {
        for (int px = 0; px < source.width_; ++px) {
            const auto rgba = source.pixels_[static_cast<std::size_t>(py) * source.width_ + px];
            if ((rgba >> 24) == 0 && blend_enabled_ && standard_blend())
                continue;
            const double u = px * sx, v = py * sy;
            const int dx = static_cast<int>(std::lround(x + u * cs + v * sn));
            const int dy = static_cast<int>(std::lround(y - u * sn + v * cs));
            rgba_pixel_tinted(dx, dy, rgba, rgb, opacity);
        }
    }
}
void Canvas::color(std::uint32_t bgr) noexcept {
    rgb_ = to_rgb(bgr);
}
void Canvas::alpha(double value) {
    finite(value);
    alpha_ = static_cast<unsigned>(std::lround(std::clamp(value, 0.0, 1.0) * 255));
}
void Canvas::font(int id) {
    if (id != 0 && id != 1)
        throw Error(ErrorKind::unsupported, "fixture font ID must be 0 or 1");
    font_scale_ = id + 1;
}
void Canvas::plot(int x, int y) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_)
        return;
    if (alpha_test_enabled_ && alpha_ <= alpha_test_ref_)
        return;
#if REGML_WII
    if (gpu_) {
        wii_gpu_box(x, y, 1, 1, (alpha_ << 24) | rgb_);
        return;
    }
#elif REGML_PS2
    if (gpu_) {
        ps2_gpu_box(x, y, 1, 1, (alpha_ << 24) | rgb_);
        return;
    }
#endif
    auto &dest =
        pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x)];
    if (!blend_enabled_) {
        store_pixel(x, y, (alpha_ << 24) | rgb_);
        return;
    }
    if (!standard_blend()) {
        rgba_pixel_tinted(x, y, (alpha_ << 24) | rgb_, 0xffffff, 1);
        return;
    }
    if (alpha_ == 255) {
        store_pixel(x, y, 0xff000000 | rgb_);
        return;
    }
    std::uint32_t result = 0xff000000;
    for (unsigned shift : {0U, 8U, 16U}) {
        const auto source = (rgb_ >> shift) & 255;
        const auto previous = (dest >> shift) & 255;
        result |= ((source * alpha_ + previous * (255 - alpha_) + 127) / 255) << shift;
    }
    store_pixel(x, y, result);
}
void Canvas::rectangle(double x1, double y1, double x2, double y2, bool outline) {
    CoordinateScale coordinates(coordinate_scale_);
    x1 *= coordinates.factor;
    y1 *= coordinates.factor;
    x2 *= coordinates.factor;
    y2 *= coordinates.factor;

    finite(x1);
    finite(y1);
    finite(x2);
    finite(y2);
    const auto left = std::floor(std::min(x1, x2)), right = std::ceil(std::max(x1, x2));
    const auto top = std::floor(std::min(y1, y2)), bottom = std::ceil(std::max(y1, y2));
    const int l = static_cast<int>(std::clamp(left, 0.0, static_cast<double>(width_)));
    const int r = static_cast<int>(std::clamp(right, 0.0, static_cast<double>(width_)));
    const int t = static_cast<int>(std::clamp(top, 0.0, static_cast<double>(height_)));
    const int b = static_cast<int>(std::clamp(bottom, 0.0, static_cast<double>(height_)));
    if (l >= r || t >= b)
        return;
#if REGML_WII
    if (gpu_ && !outline) {
        wii_gpu_box(l, t, r - l, b - t, (alpha_ << 24) | rgb_);
        return;
    }
#elif REGML_PS2
    if (gpu_ && !outline) {
        ps2_gpu_box(l, t, r - l, b - t, (alpha_ << 24) | rgb_);
        return;
    }
#endif
    const auto span = [&](int y, int first, int last) {
        if (!color_write_all() || !blend_enabled_ || alpha_test_enabled_ || !standard_blend()) {
            const auto rgba = (alpha_ << 24) | rgb_;
            for (int x = first; x < last; ++x)
                rgba_pixel_tinted(x, y, rgba, 0xffffff, 1);
            return;
        }
#if REGML_WII
        if (gpu_) {
            wii_gpu_box(first, y, last - first, 1, (alpha_ << 24) | rgb_);
            return;
        }
#elif REGML_PS2
        if (gpu_) {
            ps2_gpu_box(first, y, last - first, 1, (alpha_ << 24) | rgb_);
            return;
        }
#endif
        auto *row = pixels_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width_);
        if (alpha_ == 255) {
            std::fill(row + first, row + last, 0xff000000 | rgb_);
            return;
        }
        const unsigned inverse = 255 - alpha_;
        const unsigned red = (rgb_ >> 16 & 255) * alpha_, green = (rgb_ >> 8 & 255) * alpha_,
                       blue = (rgb_ & 255) * alpha_;
        for (int x = first; x < last; ++x) {
            const auto previous = row[x];
            row[x] = 0xff000000 | (((red + (previous >> 16 & 255) * inverse + 127) / 255) << 16) |
                     (((green + (previous >> 8 & 255) * inverse + 127) / 255) << 8) |
                     ((blue + (previous & 255) * inverse + 127) / 255);
        }
    };
    for (int y = t; y < b; ++y) {
        if (!outline || y == top || y == bottom - 1) {
            span(y, l, r);
        } else {
            if (l == left)
                span(y, l, l + 1);
            if (r - 1 == right - 1 && r - 1 != left)
                span(y, r - 1, r);
        }
    }
}
void Canvas::circle(double x, double y, double radius, bool outline) {
    CoordinateScale coordinates(coordinate_scale_);
    x *= coordinates.factor;
    y *= coordinates.factor;
    radius *= coordinates.factor;

    finite(x);
    finite(y);
    finite(radius);
    if (radius < 0)
        return;
    // Large finite circles are valid draw requests. Clip their bounding box to
    // the canvas and use hypot below so the distance test cannot overflow.
    if (x + radius < 0 || x - radius > width_ || y + radius < 0 || y - radius > height_)
        return;
#if REGML_WII
    if (gpu_) {
        wii_gpu_circle(x, y, radius, outline, (alpha_ << 24) | rgb_);
        return;
    }
#elif REGML_PS2
    if (gpu_) {
        ps2_gpu_circle(x, y, radius, outline, (alpha_ << 24) | rgb_);
        return;
    }
#endif
    const int left = static_cast<int>(std::clamp(std::floor(x - radius), 0.0, static_cast<double>(width_)));
    const int right =
        static_cast<int>(std::clamp(std::ceil(x + radius + 1), 0.0, static_cast<double>(width_)));
    const int top = static_cast<int>(std::clamp(std::floor(y - radius), 0.0, static_cast<double>(height_)));
    const int bottom =
        static_cast<int>(std::clamp(std::ceil(y + radius + 1), 0.0, static_cast<double>(height_)));
    const bool large_circle = radius > 8192 || std::fabs(x) > 1e9 || std::fabs(y) > 1e9;
    const auto outer = radius * radius;
    const auto inner = std::max(0.0, radius - 1) * std::max(0.0, radius - 1);
    for (int py = top; py < bottom; ++py)
        for (int px = left; px < right; ++px) {
            if (large_circle) {
                const double distance = std::hypot(px - x, py - y);
                if (distance <= radius && (!outline || distance >= std::max(0.0, radius - 1)))
                    plot(px, py);
            } else {
                const auto distance = (px - x) * (px - x) + (py - y) * (py - y);
                if (distance <= outer && (!outline || distance >= inner))
                    plot(px, py);
            }
        }
}
void Canvas::ellipse(double x1, double y1, double x2, double y2, bool outline) {
    CoordinateScale coordinates(coordinate_scale_);
    x1 *= coordinates.factor;
    y1 *= coordinates.factor;
    x2 *= coordinates.factor;
    y2 *= coordinates.factor;
    finite(x1);
    finite(y1);
    finite(x2);
    finite(y2);
    if (std::max({std::fabs(x1), std::fabs(y1), std::fabs(x2), std::fabs(y2)}) > 1e9)
        throw Error(ErrorKind::unsupported, "ellipse is outside supported coordinate limits");
    const double left_edge = std::min(x1, x2), right_edge = std::max(x1, x2);
    const double top_edge = std::min(y1, y2), bottom_edge = std::max(y1, y2);
    const double center_x = (left_edge + right_edge) / 2;
    const double center_y = (top_edge + bottom_edge) / 2;
    const double radius_x = (right_edge - left_edge) / 2;
    const double radius_y = (bottom_edge - top_edge) / 2;
    if (radius_x <= 0 || radius_y <= 0)
        return;
#if REGML_PS2
    if (gpu_) {
        if (!alpha_test_enabled_ || alpha_ > alpha_test_ref_)
            ps2_gpu_ellipse(left_edge, top_edge, right_edge, bottom_edge, outline, (alpha_ << 24) | rgb_);
        return;
    }
#endif
    const int left = static_cast<int>(std::clamp(std::floor(left_edge), 0.0, static_cast<double>(width_)));
    const int right = static_cast<int>(std::clamp(std::ceil(right_edge), 0.0, static_cast<double>(width_)));
    const int top = static_cast<int>(std::clamp(std::floor(top_edge), 0.0, static_cast<double>(height_)));
    const int bottom =
        static_cast<int>(std::clamp(std::ceil(bottom_edge), 0.0, static_cast<double>(height_)));
    const double inner_x = std::max(0.0, radius_x - 1.0);
    const double inner_y = std::max(0.0, radius_y - 1.0);
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const double dx = (x + 0.5 - center_x) / radius_x;
            const double dy = (y + 0.5 - center_y) / radius_y;
            if (dx * dx + dy * dy > 1)
                continue;
            if (outline && inner_x > 0 && inner_y > 0) {
                const double ix = (x + 0.5 - center_x) / inner_x;
                const double iy = (y + 0.5 - center_y) / inner_y;
                if (ix * ix + iy * iy < 1)
                    continue;
            }
            plot(x, y);
        }
    }
}
void Canvas::round_rectangle(
    double x1, double y1, double x2, double y2, double x_radius, double y_radius, bool outline) {
    CoordinateScale coordinates(coordinate_scale_);
    x1 *= coordinates.factor;
    y1 *= coordinates.factor;
    x2 *= coordinates.factor;
    y2 *= coordinates.factor;
    x_radius *= coordinates.factor;
    y_radius *= coordinates.factor;
    finite(x1);
    finite(y1);
    finite(x2);
    finite(y2);
    finite(x_radius);
    finite(y_radius);
    if (std::max({std::fabs(x1), std::fabs(y1), std::fabs(x2), std::fabs(y2), x_radius, y_radius}) > 1e9)
        throw Error(ErrorKind::unsupported, "rounded rectangle is outside supported limits");
    const double left_edge = std::min(x1, x2), right_edge = std::max(x1, x2);
    const double top_edge = std::min(y1, y2), bottom_edge = std::max(y1, y2);
    const double half_width = (right_edge - left_edge) / 2;
    const double half_height = (bottom_edge - top_edge) / 2;
    if (half_width <= 0 || half_height <= 0)
        return;
    x_radius = std::clamp(x_radius, 0.0, half_width);
    y_radius = std::clamp(y_radius, 0.0, half_height);
    if (x_radius == 0 || y_radius == 0) {
        rectangle(x1, y1, x2, y2, outline);
        return;
    }
#if REGML_PS2
    if (gpu_) {
        if (!alpha_test_enabled_ || alpha_ > alpha_test_ref_)
            ps2_gpu_round_rectangle(left_edge, top_edge, right_edge, bottom_edge, x_radius, y_radius, outline,
                                    (alpha_ << 24) | rgb_);
        return;
    }
#endif
    const double center_x = (left_edge + right_edge) / 2;
    const double center_y = (top_edge + bottom_edge) / 2;
    const int left = static_cast<int>(std::clamp(std::floor(left_edge), 0.0, static_cast<double>(width_)));
    const int right = static_cast<int>(std::clamp(std::ceil(right_edge), 0.0, static_cast<double>(width_)));
    const int top = static_cast<int>(std::clamp(std::floor(top_edge), 0.0, static_cast<double>(height_)));
    const int bottom =
        static_cast<int>(std::clamp(std::ceil(bottom_edge), 0.0, static_cast<double>(height_)));
    const auto contains = [&](double px, double py, double inset) {
        const double rx = std::max(0.0, x_radius - inset);
        const double ry = std::max(0.0, y_radius - inset);
        const double half_w = std::max(0.0, half_width - inset);
        const double half_h = std::max(0.0, half_height - inset);
        if (std::fabs(px - center_x) > half_w || std::fabs(py - center_y) > half_h)
            return false;
        if (rx == 0 || ry == 0)
            return true;
        const double corner_x = std::max(0.0, std::fabs(px - center_x) - (half_w - rx));
        const double corner_y = std::max(0.0, std::fabs(py - center_y) - (half_h - ry));
        const double dx = corner_x / rx, dy = corner_y / ry;
        return dx * dx + dy * dy <= 1;
    };
    for (int y = top; y < bottom; ++y)
        for (int x = left; x < right; ++x) {
            const double px = x + 0.5, py = y + 0.5;
            if (contains(px, py, 0) && (!outline || !contains(px, py, 1)))
                plot(x, y);
        }
}
void Canvas::line(double x1, double y1, double x2, double y2, double width) {
    CoordinateScale coordinates(coordinate_scale_);
    x1 *= coordinates.factor;
    y1 *= coordinates.factor;
    x2 *= coordinates.factor;
    y2 *= coordinates.factor;
    width *= coordinates.factor;

    finite(x1);
    finite(y1);
    finite(x2);
    finite(y2);
    finite(width);
    if (width <= 0 || width > 2048 ||
        std::max({std::fabs(x1), std::fabs(y1), std::fabs(x2), std::fabs(y2)}) > 1e9)
        throw Error(ErrorKind::unsupported, "line is outside supported limits");
#if REGML_WII
    if (gpu_) {
        const double dx = x2 - x1, dy = y2 - y1, length = std::hypot(dx, dy);
        if (length == 0) {
            wii_gpu_box(x1, y1, width, width, (alpha_ << 24) | rgb_);
            return;
        }
        const double nx = -dy / length * width / 2, ny = dx / length * width / 2;
        const unsigned rgba = (alpha_ << 24) | rgb_;
        wii_gpu_triangle(x1 + nx, y1 + ny, x2 + nx, y2 + ny, x2 - nx, y2 - ny, rgba);
        wii_gpu_triangle(x1 + nx, y1 + ny, x2 - nx, y2 - ny, x1 - nx, y1 - ny, rgba);
        return;
    }
#elif REGML_PS2
    if (gpu_) {
        const double dx = x2 - x1, dy = y2 - y1, length = std::hypot(dx, dy);
        if (length == 0) {
            ps2_gpu_box(x1, y1, width, width, (alpha_ << 24) | rgb_);
            return;
        }
        const double nx = -dy / length * width / 2, ny = dx / length * width / 2;
        const auto rgba = (alpha_ << 24) | rgb_;
        ps2_gpu_triangle(x1 + nx, y1 + ny, x2 + nx, y2 + ny, x2 - nx, y2 - ny, rgba);
        ps2_gpu_triangle(x1 + nx, y1 + ny, x2 - nx, y2 - ny, x1 - nx, y1 - ny, rgba);
        return;
    }
#endif
    const double dx = x2 - x1, dy = y2 - y1;
    const auto steps = static_cast<std::size_t>(std::ceil(std::max(std::fabs(dx), std::fabs(dy))));
    if (steps > 1'000'000)
        throw Error(ErrorKind::unsupported, "line exceeds rasterization limit");
    const double radius = (width - 1) / 2;
    for (std::size_t i = 0; i <= steps; ++i) {
        const double t = steps ? static_cast<double>(i) / steps : 0;
        const double x = x1 + dx * t, y = y1 + dy * t;
        if (radius < 0.5)
            plot(static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)));
        else
            circle(x, y, radius, false);
    }
}
void Canvas::triangle(double x1, double y1, double x2, double y2, double x3, double y3, bool outline) {
    CoordinateScale coordinates(coordinate_scale_);
    x1 *= coordinates.factor;
    y1 *= coordinates.factor;
    x2 *= coordinates.factor;
    y2 *= coordinates.factor;
    x3 *= coordinates.factor;
    y3 *= coordinates.factor;

    finite(x1);
    finite(y1);
    finite(x2);
    finite(y2);
    finite(x3);
    finite(y3);
    if (outline) {
        line(x1, y1, x2, y2);
        line(x2, y2, x3, y3);
        line(x3, y3, x1, y1);
        return;
    }
#if REGML_WII
    if (gpu_) {
        wii_gpu_triangle(x1, y1, x2, y2, x3, y3, (alpha_ << 24) | rgb_);
        return;
    }
#elif REGML_PS2
    if (gpu_) {
        ps2_gpu_triangle(x1, y1, x2, y2, x3, y3, (alpha_ << 24) | rgb_);
        return;
    }
#endif
    const double area = (x2 - x1) * (y3 - y1) - (y2 - y1) * (x3 - x1);
    if (std::fabs(area) < 1e-12)
        return;
    const int left =
        static_cast<int>(std::clamp(std::floor(std::min({x1, x2, x3})), 0.0, static_cast<double>(width_)));
    const int right =
        static_cast<int>(std::clamp(std::ceil(std::max({x1, x2, x3})), 0.0, static_cast<double>(width_)));
    const int top =
        static_cast<int>(std::clamp(std::floor(std::min({y1, y2, y3})), 0.0, static_cast<double>(height_)));
    const int bottom =
        static_cast<int>(std::clamp(std::ceil(std::max({y1, y2, y3})), 0.0, static_cast<double>(height_)));
    for (int y = top; y < bottom; ++y)
        for (int x = left; x < right; ++x) {
            const double px = x + 0.5, py = y + 0.5;
            const double a = (x2 - px) * (y3 - py) - (y2 - py) * (x3 - px);
            const double b = (x3 - px) * (y1 - py) - (y3 - py) * (x1 - px);
            const double c = (x1 - px) * (y2 - py) - (y1 - py) * (x2 - px);
            if ((a >= 0 && b >= 0 && c >= 0) || (a <= 0 && b <= 0 && c <= 0))
                plot(x, y);
        }
}
void Canvas::text(double x, double y, std::string_view value) {
    text(x, y, value, 1, 1, 0, 0, 0);
}
void Canvas::text(
    double x, double y, std::string_view value, double sx, double sy, double angle, int halign, int valign) {
    const auto profile_start =
        profile_text_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    finite(x);
    finite(y);
    finite(sx);
    finite(sy);
    finite(angle);
    if (std::fabs(x) > 1e9 || std::fabs(y) > 1e9 || value.size() > 4096)
        throw Error(ErrorKind::unsupported, "text exceeds fixture drawing limits");
    if (sx == 0 || sy == 0)
        return;

    const auto newline = [](char c) { return c == '\n' || c == '#'; };
    std::size_t lines = 1, line_length = 0, longest_line = 0;
    for (char c : value) {
        if (newline(c)) {
            longest_line = std::max(longest_line, line_length);
            line_length = 0;
            ++lines;
            continue;
        }
        ++line_length;
    }
    longest_line = std::max(longest_line, line_length);
    const double width = static_cast<double>(longest_line) * 6 * font_scale_;
    const double height = (8 + static_cast<double>(lines - 1) * 9) * font_scale_;
    double pen_x = -width * halign / 2.0;
    double pen_y = -height * valign / 2.0;
    const double radians = std::remainder(angle, 360.0) * 3.141592653589793 / 180.0;
    const double cosine = std::cos(radians), sine = std::sin(radians);
    const double scale = coordinate_scale_;
    const double anchor_x = x * scale, anchor_y = y * scale;
#if REGML_PS2
    const bool ps2_text_atlas = gpu_ && ps2_gpu_text_atlas_ready();
    std::array<Ps2TextGlyph, 64> ps2_text_batch{};
    std::size_t ps2_text_count = 0;
    const auto flush_ps2_text = [&] {
        if (ps2_text_count) {
            ps2_gpu_text_glyphs(std::span<const Ps2TextGlyph>(ps2_text_batch.data(), ps2_text_count),
                                (alpha_ << 24) | rgb_, angle != 0 || sx <= 0 || sy <= 0);
            ps2_text_count = 0;
        }
    };
#endif
    for (char c : value) {
        if (newline(c)) {
            pen_x = -width * halign / 2.0;
            pen_y += 9 * font_scale_;
            continue;
        }
#if REGML_PS2
        if (ps2_text_atlas) {
            const double left = pen_x, right = pen_x + 5 * font_scale_;
            const double top = pen_y, bottom = pen_y + 7 * font_scale_;
            if (c == ' ') {
                pen_x += 6 * font_scale_;
                continue;
            }
            const auto transform_x = [&](double px, double py) {
                return anchor_x + (px * sx * cosine + py * sy * sine) * scale;
            };
            const auto transform_y = [&](double px, double py) {
                return anchor_y + (-px * sx * sine + py * sy * cosine) * scale;
            };
            const std::array<double, 8> corners = {transform_x(left, top),     transform_y(left, top),
                                                   transform_x(right, top),    transform_y(right, top),
                                                   transform_x(right, bottom), transform_y(right, bottom),
                                                   transform_x(left, bottom),  transform_y(left, bottom)};
            const double min_x = std::min({corners[0], corners[2], corners[4], corners[6]});
            const double max_x = std::max({corners[0], corners[2], corners[4], corners[6]});
            const double min_y = std::min({corners[1], corners[3], corners[5], corners[7]});
            const double max_y = std::max({corners[1], corners[3], corners[5], corners[7]});
            if (max_x > 0 && max_y > 0 && min_x < width_ && min_y < height_) {
                auto &glyph = ps2_text_batch[ps2_text_count++];
                glyph.character = static_cast<unsigned char>(c);
                for (std::size_t i = 0; i < corners.size(); ++i)
                    glyph.corners[i] = static_cast<float>(corners[i]);
                if (ps2_text_count == ps2_text_batch.size())
                    flush_ps2_text();
            }
            pen_x += 6 * font_scale_;
            continue;
        }
#endif
        const auto bitmap = font_bitmap(static_cast<unsigned char>(c));
        struct GlyphRectangle {
            int left, top, right, bottom;
        };
        std::array<GlyphRectangle, 21> rectangles{};
        std::array<int, 3> active_rectangles{};
        int rectangle_count = 0, active_count = 0;
        for (int row = 0; row < 7; ++row) {
            std::array<int, 3> next_active{};
            int next_count = 0;
            const auto bits = bitmap[static_cast<std::size_t>(row)];
            for (int column = 0; column < 5;) {
                while (column < 5 && (bits & (1U << static_cast<unsigned>(4 - column))) == 0)
                    ++column;
                const int first = column;
                while (column < 5 && (bits & (1U << static_cast<unsigned>(4 - column))) != 0)
                    ++column;
                if (first == column)
                    continue;

                int rectangle_index = -1;
                for (int i = 0; i < active_count; ++i) {
                    const int candidate = active_rectangles[static_cast<std::size_t>(i)];
                    auto &active = rectangles[static_cast<std::size_t>(candidate)];
                    if (active.left == first && active.right == column) {
                        rectangle_index = candidate;
                        active.bottom = row + 1;
                        break;
                    }
                }
                if (rectangle_index < 0) {
                    rectangle_index = rectangle_count++;
                    rectangles[static_cast<std::size_t>(rectangle_index)] = {first, row, column, row + 1};
                }
                next_active[static_cast<std::size_t>(next_count++)] = rectangle_index;
            }
            active_rectangles = next_active;
            active_count = next_count;
        }

        for (int i = 0; i < rectangle_count; ++i) {
            const auto &shape = rectangles[static_cast<std::size_t>(i)];
            const double left = pen_x + shape.left * font_scale_;
            const double right = pen_x + shape.right * font_scale_;
            const double top = pen_y + shape.top * font_scale_;
            const double bottom = pen_y + shape.bottom * font_scale_;
            if (sine == 0) {
                rectangle(x + left * sx, y + top * sy, x + right * sx, y + bottom * sy, false);
                continue;
            }

            const auto transform_x = [&](double px, double py) {
                return anchor_x + (px * sx * cosine + py * sy * sine) * scale;
            };
            const auto transform_y = [&](double px, double py) {
                return anchor_y + (-px * sx * sine + py * sy * cosine) * scale;
            };
            const double corners_x[] = {transform_x(left, top), transform_x(right, top),
                                        transform_x(left, bottom), transform_x(right, bottom)};
            const double corners_y[] = {transform_y(left, top), transform_y(right, top),
                                        transform_y(left, bottom), transform_y(right, bottom)};
            const double min_x = *std::min_element(std::begin(corners_x), std::end(corners_x));
            const double max_x = *std::max_element(std::begin(corners_x), std::end(corners_x));
            const double min_y = *std::min_element(std::begin(corners_y), std::end(corners_y));
            const double max_y = *std::max_element(std::begin(corners_y), std::end(corners_y));
#if REGML_PS2
            if (gpu_) {
                if (max_x > 0 && max_y > 0 && min_x < width_ && min_y < height_)
                    ps2_gpu_quad(corners_x[0], corners_y[0], corners_x[1], corners_y[1], corners_x[3],
                                 corners_y[3], corners_x[2], corners_y[2], (alpha_ << 24) | rgb_);
                continue;
            }
#endif
            const int x0 = static_cast<int>(std::clamp(std::floor(min_x), 0.0, static_cast<double>(width_)));
            const int x1 = static_cast<int>(std::clamp(std::ceil(max_x), 0.0, static_cast<double>(width_)));
            const int y0 = static_cast<int>(std::clamp(std::floor(min_y), 0.0, static_cast<double>(height_)));
            const int y1 = static_cast<int>(std::clamp(std::ceil(max_y), 0.0, static_cast<double>(height_)));
            const double inverse_width = 1.0 / (sx * scale);
            const double inverse_height = 1.0 / (sy * scale);
            for (int py = y0; py < y1; ++py)
                for (int px = x0; px < x1; ++px) {
                    const double dx = px + 0.5 - anchor_x;
                    const double dy = py + 0.5 - anchor_y;
                    const double local_x = (dx * cosine - dy * sine) * inverse_width;
                    const double local_y = (dx * sine + dy * cosine) * inverse_height;
                    if (local_x >= left && local_x < right && local_y >= top && local_y < bottom)
                        plot(px, py);
                }
        }
        pen_x += 6 * font_scale_;
    }
#if REGML_PS2
    flush_ps2_text();
#endif
    if (profile_text_)
        text_profile_ms_ +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - profile_start)
                .count();
}
void Canvas::debug_text_overlay(std::span<const std::string_view> lines) {
    CoordinateScale coordinates(coordinate_scale_);

    if (lines.empty())
        return;
    const auto saved_rgb = rgb_;
    const auto saved_alpha = alpha_;
    const auto saved_font_scale = font_scale_;
    const int max_lines = std::max(0, (height_ - 12) / 9);
    const auto visible_lines = std::min<std::size_t>(lines.size(), static_cast<std::size_t>(max_lines));
    std::size_t longest = 0;
    for (std::size_t i = 0; i < visible_lines; ++i)
        longest = std::max(longest, lines[i].size());
    const int max_columns = std::max(0, (width_ - 12) / 6);
    const int panel_width = std::min(
        width_ - 4,
        8 + static_cast<int>(std::min<std::size_t>(longest, static_cast<std::size_t>(max_columns))) * 6);
    const int panel_height = 8 + static_cast<int>(visible_lines) * 9;
#if REGML_WII
    if (gpu_)
        wii_gpu_box_batch_begin();
#endif
    font_scale_ = 1;
    rgb_ = 0x000000;
    alpha_ = 204;
    rectangle(2, 2, panel_width, panel_height, false);
    rgb_ = 0xffffff;
    alpha_ = 255;
    for (std::size_t i = 0; i < visible_lines; ++i) {
        const auto length = std::min<std::size_t>(lines[i].size(), static_cast<std::size_t>(max_columns));
        text(6, 6 + static_cast<double>(i) * 9, lines[i].substr(0, length));
    }
    rgb_ = saved_rgb;
    alpha_ = saved_alpha;
    font_scale_ = saved_font_scale;
#if REGML_WII
    if (gpu_)
        wii_gpu_box_batch_end();
#endif
}
std::uint64_t Canvas::hash() const noexcept {
    std::uint64_t value = 14695981039346656037ULL;
#if REGML_WII
    if (gpu_) {
        for (int y = 0; y < height_; ++y)
            for (int x = 0; x < width_; ++x) {
                const auto rgba = wii_gpu_pixel(x, y);
                for (unsigned shift : {0U, 8U, 16U, 24U}) {
                    value ^= (rgba >> shift) & 255;
                    value *= 1099511628211ULL;
                }
            }
        return value;
    }
#elif REGML_PS2
    if (gpu_) {
        for (int y = 0; y < height_; ++y)
            for (int x = 0; x < width_; ++x) {
                const auto rgba = ps2_gpu_pixel(x, y);
                for (unsigned shift : {0U, 8U, 16U, 24U}) {
                    value ^= (rgba >> shift) & 255;
                    value *= 1099511628211ULL;
                }
            }
        return value;
    }
#endif
    for (auto pixel : pixels_)
        for (unsigned shift : {0U, 8U, 16U, 24U}) {
            value ^= (pixel >> shift) & 255;
            value *= 1099511628211ULL;
        }
    return value;
}
void Canvas::save_bmp(const std::filesystem::path &path) const {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
        throw Error(ErrorKind::io, "cannot open screenshot: " + path.string());
    const auto stride = (static_cast<std::uint32_t>(width_) * 3 + 3) & ~3U;
    const auto bytes = stride * static_cast<std::uint32_t>(height_);
    auto write32 = [&](std::uint32_t v) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            file.put(static_cast<char>((v >> shift) & 255));
    };
    auto write16 = [&](unsigned v) {
        file.put(static_cast<char>(v & 255));
        file.put(static_cast<char>((v >> 8) & 255));
    };
    file.put('B');
    file.put('M');
    write32(54 + bytes);
    write32(0);
    write32(54);
    write32(40);
    write32(static_cast<std::uint32_t>(width_));
    write32(static_cast<std::uint32_t>(height_));
    write16(1);
    write16(24);
    write32(0);
    write32(bytes);
    write32(2835);
    write32(2835);
    write32(0);
    write32(0);
    for (int y = height_ - 1; y >= 0; --y) {
        for (int x = 0; x < width_; ++x) {
            const auto p = pixel(x, y);
            file.put(static_cast<char>(p & 255));
            file.put(static_cast<char>((p >> 8) & 255));
            file.put(static_cast<char>((p >> 16) & 255));
        }
        for (auto padding = static_cast<unsigned>(width_) * 3; padding < stride; ++padding)
            file.put(0);
    }
    file.close();
    if (!file)
        throw Error(ErrorKind::io, "screenshot write failed");
}
void Canvas::save_png(const std::filesystem::path &path) const {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = static_cast<png_uint_32>(width_);
    image.height = static_cast<png_uint_32>(height_);
    if constexpr (std::endian::native == std::endian::little)
        image.format = PNG_FORMAT_BGRA;
    else
        image.format = PNG_FORMAT_ARGB;
    const auto filename = path.string();
    if (!png_image_write_to_file(&image, filename.c_str(), 0,
                                 reinterpret_cast<const unsigned char *>(pixels_.data()), 0, nullptr))
        throw Error(ErrorKind::io, "screenshot PNG write failed" +
                                       (image.message[0] ? ": " + std::string(image.message) : ""));
    png_image_free(&image);
}
} // namespace regml
