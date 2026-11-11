#include "rendersoftware/canvas.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using Workload = std::uint64_t (*)();

std::vector<std::uint32_t> make_sprite() {
    constexpr int width = 64;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * width);
    for (int y = 0; y < width; ++y)
        for (int x = 0; x < width; ++x) {
            const unsigned alpha = static_cast<unsigned>((x * 17 + y * 29 + x * y * 3) & 255);
            const unsigned red = static_cast<unsigned>((x * 11 + y * 7) & 255);
            const unsigned green = static_cast<unsigned>((x * 5 + y * 19) & 255);
            const unsigned blue = static_cast<unsigned>((x * 23 + y * 13) & 255);
            pixels[static_cast<std::size_t>(y) * width + x] =
                (alpha << 24) | (red << 16) | (green << 8) | blue;
        }
    return pixels;
}

const std::vector<std::uint32_t> sprite = make_sprite();

std::uint64_t sprite_blits_opaque_background() {
    regml::Canvas canvas(640, 480);
    canvas.clear(0x302010);
    constexpr std::array<unsigned, 4> tints{0xffffff, 0xffc080, 0x80c0ff, 0x80ff80};
    constexpr std::array<double, 3> opacities{1.0, 0.75, 0.5};
    for (int frame = 0; frame < 80; ++frame)
        for (int item = 0; item < 48; ++item) {
            const int x = (item * 73 + frame * 11) % 576;
            const int y = (item * 47 + frame * 17) % 416;
            canvas.blit_rgba(sprite, 64, 0, 0, x, y, 64, 64,
                             tints[static_cast<std::size_t>((frame + item) % tints.size())],
                             opacities[static_cast<std::size_t>((frame * 3 + item) % opacities.size())]);
        }
    return canvas.hash();
}

std::uint64_t sprite_blits_transparent_background() {
    regml::Canvas canvas(640, 480);
    canvas.clear_transparent();
    for (int frame = 0; frame < 80; ++frame)
        for (int item = 0; item < 48; ++item) {
            const int x = (item * 73 + frame * 11) % 576;
            const int y = (item * 47 + frame * 17) % 416;
            canvas.blit_rgba(sprite, 64, 0, 0, x, y, 64, 64, item % 2 ? 0xffffff : 0x80b0ff,
                             item % 3 ? 1.0 : 0.75);
        }
    return canvas.hash();
}

std::uint64_t custom_blend_sprites() {
    regml::Canvas canvas(640, 480);
    canvas.clear(0x302010);
    canvas.blend_mode(5, 2, 5, 2);
    for (int frame = 0; frame < 48; ++frame)
        for (int item = 0; item < 36; ++item) {
            const int x = (item * 73 + frame * 11) % 576;
            const int y = (item * 47 + frame * 17) % 416;
            canvas.blit_rgba(sprite, 64, 0, 0, x, y, 64, 64, item % 2 ? 0xffffff : 0x80b0ff,
                             item % 3 ? 1.0 : 0.75);
        }
    return canvas.hash();
}

std::uint64_t translucent_rectangles() {
    regml::Canvas canvas(640, 480);
    canvas.clear(0x302010);
    for (int frame = 0; frame < 36; ++frame) {
        for (int item = 0; item < 36; ++item) {
            canvas.color(static_cast<std::uint32_t>((item * 0x10203 + frame * 0x30405) & 0xffffff));
            canvas.alpha(0.35 + (item % 6) * 0.1);
            const int x = (item * 97 + frame * 13) % 500;
            const int y = (item * 61 + frame * 19) % 370;
            canvas.rectangle(x, y, x + 120, y + 90, false);
        }
    }
    return canvas.hash();
}

std::uint64_t thick_lines() {
    regml::Canvas canvas(640, 480);
    canvas.clear(0x302010);
    for (int frame = 0; frame < 16; ++frame) {
        for (int item = 0; item < 18; ++item) {
            canvas.color(static_cast<std::uint32_t>((item * 0x10203 + frame * 0x30405) & 0xffffff));
            canvas.alpha(0.4 + (item % 5) * 0.12);
            canvas.line((item * 41 + frame * 7) % 640, (item * 67 + frame * 11) % 480,
                        (item * 41 + frame * 7 + 510) % 640, (item * 67 + frame * 11 + 370) % 480, 20);
        }
    }
    return canvas.hash();
}

std::uint64_t circle_ellipse_rasterization() {
    regml::Canvas canvas(640, 480);
    canvas.clear(0x302010);
    for (int frame = 0; frame < 32; ++frame)
        for (int item = 0; item < 32; ++item) {
            canvas.color(static_cast<std::uint32_t>((item * 0x10203 + frame * 0x30405) & 0xffffff));
            canvas.alpha(0.4 + (item % 5) * 0.12);
            const int x = 24 + (item * 83 + frame * 17) % 560;
            const int y = 24 + (item * 61 + frame * 13) % 400;
            const double rx = 4 + item % 28, ry = 5 + (item * 3) % 22;
            canvas.circle(x, y, rx, item % 3 == 0);
            canvas.ellipse(x - rx, y - ry, x + rx, y + ry, item % 4 == 0);
        }
    return canvas.hash();
}

std::uint64_t filled_triangles() {
    regml::Canvas canvas(640, 480);
    canvas.clear(0x302010);
    for (int frame = 0; frame < 24; ++frame)
        for (int item = 0; item < 32; ++item) {
            canvas.color(static_cast<std::uint32_t>((item * 0x10203 + frame * 0x30405) & 0xffffff));
            canvas.alpha(0.45 + (item % 5) * 0.1);
            const double x = (item * 71 + frame * 19) % 640;
            const double y = (item * 43 + frame * 23) % 480;
            canvas.triangle(x, y, x + 30 + item % 55, y + 4 + item % 31, x - 5 - frame % 27,
                            y + 20 + frame % 44, false);
        }
    return canvas.hash();
}

std::uint64_t rounded_rectangles() {
    regml::Canvas canvas(640, 480);
    canvas.clear(0x302010);
    for (int frame = 0; frame < 28; ++frame)
        for (int item = 0; item < 28; ++item) {
            canvas.color(static_cast<std::uint32_t>((item * 0x10203 + frame * 0x30405) & 0xffffff));
            canvas.alpha(0.45 + (item % 5) * 0.1);
            const int x = (item * 83 + frame * 17) % 560;
            const int y = (item * 61 + frame * 13) % 400;
            canvas.round_rectangle(x, y, x + 32 + item % 68, y + 24 + frame % 56, 4 + item % 24,
                                   4 + frame % 18, item % 3 == 0);
        }
    return canvas.hash();
}

std::uint64_t repeated_text() {
    regml::Canvas canvas(640, 480);
    canvas.clear(0x302010);
    constexpr std::string_view line = "PERFORMANCE BENCHMARK: PIXEL SPRITE TEXT 0123456789";
    for (int frame = 0; frame < 48; ++frame)
        for (int row = 0; row < 10; ++row)
            canvas.text((frame * 13 + row * 7) % 300, row * 42, line);
    return canvas.hash();
}

std::uint64_t rotated_text() {
    regml::Canvas canvas(640, 480);
    canvas.clear(0x302010);
    constexpr std::string_view line = "ROTATED PIXEL TEXT 0123456789";
    for (int frame = 0; frame < 40; ++frame)
        for (int row = 0; row < 6; ++row) {
            canvas.color(static_cast<std::uint32_t>((row * 0x30405 + frame * 0x10203) & 0xffffff));
            canvas.text(80 + (frame * 7) % 240, 30 + row * 70, line, 1.0, 1.0, 10 + (frame % 5) * 7, 0, 0);
        }
    return canvas.hash();
}

void measure(std::string_view name, Workload workload) {
    std::array<double, 3> milliseconds{};
    std::uint64_t checksum = 0;
    for (std::size_t sample = 0; sample < milliseconds.size(); ++sample) {
        const auto start = Clock::now();
        checksum = workload();
        milliseconds[sample] = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }
    std::sort(milliseconds.begin(), milliseconds.end());
    std::cout << name << " median_ms=" << std::fixed << std::setprecision(3) << milliseconds[1]
              << " checksum=" << std::hex << checksum << std::dec << '\n';
}
} // namespace

int main() {
    measure("sprite_blits_opaque_background", sprite_blits_opaque_background);
    measure("sprite_blits_transparent_background", sprite_blits_transparent_background);
    measure("custom_blend_sprites", custom_blend_sprites);
    measure("translucent_rectangles", translucent_rectangles);
    measure("thick_lines", thick_lines);
    measure("circle_ellipse_rasterization", circle_ellipse_rasterization);
    measure("filled_triangles", filled_triangles);
    measure("rounded_rectangles", rounded_rectangles);
    measure("repeated_text", repeated_text);
    measure("rotated_text", rotated_text);
}
