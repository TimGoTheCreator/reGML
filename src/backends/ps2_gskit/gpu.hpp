#pragma once

#if REGML_PS2
#include <array>
#include <cstdint>
#include <gsKit.h>
#include <span>

namespace regml {
struct Ps2GpuProfile {
    double image_ms = 0;
    std::uint64_t image_calls = 0;
    std::uint64_t image_quads = 0;
    std::uint64_t glyph_quads = 0;
    std::uint64_t texture_uploads = 0;
};

struct Ps2TextGlyph {
    unsigned char character = 0;
    std::array<float, 8> corners{};
};

struct Ps2FontGlyph {
    int source_x = 0, source_y = 0, width = 0, height = 0;
    float x = 0, y = 0, draw_width = 0, draw_height = 0;
};

void ps2_gpu_initialize(GSGLOBAL *gs, int canvas_width, int canvas_height);
void ps2_gpu_shutdown() noexcept;
void ps2_gpu_profile(bool enabled) noexcept;
Ps2GpuProfile ps2_gpu_take_profile() noexcept;
void ps2_gpu_begin();
void ps2_gpu_flush();
void ps2_gpu_presented() noexcept;
void ps2_gpu_box(double x, double y, double width, double height, std::uint32_t rgba);
void ps2_gpu_ellipse(double x1, double y1, double x2, double y2, bool outline, std::uint32_t rgba);
void ps2_gpu_round_rectangle(double x1,
                             double y1,
                             double x2,
                             double y2,
                             double x_radius,
                             double y_radius,
                             bool outline,
                             std::uint32_t rgba);
void ps2_gpu_circle(double x, double y, double radius, bool outline, std::uint32_t rgba);
void ps2_gpu_triangle(double x1, double y1, double x2, double y2, double x3, double y3, std::uint32_t rgba);
void ps2_gpu_quad(double x1,
                  double y1,
                  double x2,
                  double y2,
                  double x3,
                  double y3,
                  double x4,
                  double y4,
                  std::uint32_t rgba);
bool ps2_gpu_text_atlas_ready() noexcept;
void ps2_gpu_text_glyphs(std::span<const Ps2TextGlyph> glyphs, std::uint32_t rgba, bool rotated);
bool ps2_gpu_font_glyphs(std::span<const std::uint32_t> pixels,
                         int stride,
                         int source_x,
                         int source_y,
                         int tile_x,
                         int tile_y,
                         std::uint64_t page_key,
                         std::span<const Ps2FontGlyph> glyphs,
                         unsigned tint,
                         double alpha);
bool ps2_gpu_cached_image(int width,
                          int height,
                          double x,
                          double y,
                          double draw_width,
                          double draw_height,
                          double angle,
                          unsigned tint,
                          double opacity,
                          std::uint64_t key);
bool ps2_gpu_prefetch_image(std::span<const std::uint32_t> pixels,
                            int stride,
                            int source_x,
                            int source_y,
                            int width,
                            int height,
                            std::uint64_t key);
void ps2_gpu_image(std::span<const std::uint32_t> pixels,
                   int stride,
                   int left,
                   int top,
                   int width,
                   int height,
                   double x,
                   double y,
                   double draw_width,
                   double draw_height,
                   double angle,
                   unsigned tint,
                   double opacity,
                   std::uint64_t key = 0);
std::uint32_t ps2_gpu_pixel(int x, int y) noexcept;
bool ps2_gpu_readback(std::span<std::uint32_t> pixels) noexcept;
} // namespace regml
#endif
