#pragma once
#if REGML_WII
#include <cstdint>
#include <span>
namespace regml {
void wii_gpu_begin();
void wii_gpu_presented();
void wii_gpu_circle(double x, double y, double radius, bool outline, std::uint32_t rgba);
void wii_gpu_box(double x, double y, double width, double height, std::uint32_t rgba);
void wii_gpu_triangle(double x1, double y1, double x2, double y2, double x3, double y3, std::uint32_t rgba);
bool wii_gpu_cached_image(int width, int height, double x, double y, double draw_width, double draw_height,
                          double angle, unsigned tint, double opacity, std::uint64_t key);
std::uint32_t wii_gpu_pixel(int x, int y);
void wii_gpu_image(std::span<const std::uint32_t> pixels, int stride, int left, int top, int width, int height,
                   double x, double y, double draw_width, double draw_height, double angle, unsigned tint,
                   double opacity, std::uint64_t key = 0);
} // namespace regml
#endif
