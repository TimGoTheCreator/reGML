#pragma once

#include "gl_common.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace regml::glmodern {

void gl_gpu_begin();
void gl_gpu_flush();
void gl_gpu_presented();

void gl_gpu_box(double x, double y, double w, double h, std::uint32_t rgba);
void gl_gpu_circle(double x, double y, double r, bool outline, std::uint32_t rgba);
void gl_gpu_triangle(double x1, double y1, double x2, double y2, double x3, double y3, std::uint32_t rgba);

void gl_gpu_image(std::span<const std::uint32_t> pixels,
                 int stride,
                 int left,
                 int top,
                 int width,
                 int height,
                 double x,
                 double y,
                 double dw,
                 double dh,
                 double angle,
                 unsigned tint,
                 double opacity,
                 std::uint64_t key = 0);

inline void gl_gpu_sprite(std::uint64_t key,
                          std::span<const std::uint32_t> pixels,
                          int stride,
                          int height,
                          int left,
                          int top,
                          int width,
                          int h,
                          double x,
                          double y,
                          double dw,
                          double dh,
                          double angle,
                          unsigned tint,
                          double opacity) {
    (void)height;
    gl_gpu_image(pixels, stride, left, top, width, h, x, y, dw, dh, angle, tint, opacity, key);
}

bool gl_gpu_cached_sub_image(double x,
                             double y,
                             double dw,
                             double dh,
                             float u0,
                             float v0,
                             float u1,
                             float v1,
                             unsigned tint,
                             double opacity,
                             std::uint64_t key);

bool gl_gpu_cached_image(int width,
                        int height,
                        double x,
                        double y,
                        double dw,
                        double dh,
                        double angle,
                        unsigned tint,
                        double opacity,
                        std::uint64_t key);

std::uint32_t gl_gpu_pixel(int x, int y);
bool gl_gpu_readback(int width, int height, std::span<std::uint32_t> pixels) noexcept;

void gl_gpu_box_batch_begin();
void gl_gpu_box_batch_end();

void gl_gpu_set_viewport(int width, int height);

void gl_gpu_set_blend_enable(bool enabled);
void gl_gpu_set_blend_mode(int src_rgb, int dst_rgb, int src_a, int dst_a, int eq_rgb = 0, int eq_a = 0);
void gl_gpu_set_alpha_test(bool enabled, float ref = 0.0f);
void gl_gpu_set_color_mask(bool r, bool g, bool b, bool a);

void gl_gpu_set_frame_number(std::size_t frame);
std::size_t gl_gpu_flush_draw_calls();
std::size_t gl_gpu_frame_draw_calls();
std::uint32_t gl_gpu_last_texture_id();
std::vector<std::uint32_t> gl_gpu_textures_used();

std::uint32_t gl_gpu_surface_create(int width, int height);
void gl_gpu_surface_destroy(std::uint32_t surface_id);
void gl_gpu_surface_bind(std::uint32_t surface_id, int width, int height);
void gl_gpu_surface_clear(std::uint32_t argb);
bool gl_gpu_surface_draw(std::uint32_t surface_id,
                         double left,
                         double top,
                         double width,
                         double height,
                         double x,
                         double y,
                         double dw,
                         double dh,
                         double angle,
                         unsigned tint,
                         double opacity);

} // namespace regml::glmodern
