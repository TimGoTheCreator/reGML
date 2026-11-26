#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace regml::gx2 {

void gx2_gpu_begin();
void gx2_gpu_flush();
void gx2_gpu_presented();

void gx2_gpu_box(double x, double y, double w, double h, std::uint32_t rgba);
void gx2_gpu_circle(double x, double y, double r, bool outline, std::uint32_t rgba);
void gx2_gpu_triangle(double x1, double y1, double x2, double y2, double x3, double y3, std::uint32_t rgba);

void gx2_gpu_image(std::span<const std::uint32_t> pixels,
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

bool gx2_gpu_cached_sub_image(double x,
                              double y,
                              double dw,
                              double dh,
                              float u0,
                              float v0,
                              float u1,
                              float v1,
                              unsigned tint,
                              double opacity,
                              std::uint64_t key,
                              double angle = 0.0);

bool gx2_gpu_has_cached_image(std::uint64_t key);
void gx2_gpu_preload_image(std::span<const std::uint32_t> pixels,
                           int stride,
                           int left,
                           int top,
                           int width,
                           int height,
                           std::uint64_t key);

bool gx2_gpu_cached_image(int width,
                          int height,
                          double x,
                          double y,
                          double dw,
                          double dh,
                          double angle,
                          unsigned tint,
                          double opacity,
                          std::uint64_t key);

std::uint32_t gx2_gpu_pixel(int x, int y);
bool gx2_gpu_readback(int width, int height, std::span<std::uint32_t> pixels) noexcept;

void gx2_gpu_box_batch_begin();
void gx2_gpu_box_batch_end();

void gx2_gpu_set_viewport(int width, int height);

void gx2_gpu_set_blend_enable(bool enabled);
void gx2_gpu_set_blend_mode(int src_rgb, int dst_rgb, int src_a, int dst_a, int eq_rgb = 0, int eq_a = 0);
void gx2_gpu_set_alpha_test(bool enabled, float ref = 0.0f);
void gx2_gpu_set_color_mask(bool r, bool g, bool b, bool a);

void gx2_gpu_set_frame_number(std::size_t frame);
std::size_t gx2_gpu_flush_draw_calls();
std::size_t gx2_gpu_frame_draw_calls();
std::uint32_t gx2_gpu_last_texture_id();
std::vector<std::uint32_t> gx2_gpu_textures_used();

std::uint32_t gx2_gpu_surface_create(int width, int height);
void gx2_gpu_surface_destroy(std::uint32_t surface_id);
void gx2_gpu_surface_bind(std::uint32_t surface_id, int width, int height);
void gx2_gpu_surface_clear(std::uint32_t argb);
bool gx2_gpu_surface_draw(std::uint32_t surface_id,
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

} // namespace regml::gx2
