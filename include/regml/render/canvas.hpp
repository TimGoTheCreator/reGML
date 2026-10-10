#pragma once
#include "regml/core/error.hpp"
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>
namespace regml {
// Deterministic CPU framebuffer. SDL presents these same pixels on screen.
class Canvas {
public:
    Canvas(int width, int height);
    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }
    [[nodiscard]] std::span<const std::uint32_t> pixels() const noexcept { return pixels_; }
    [[nodiscard]] std::uint32_t pixel(int x, int y) const;
    void clear(std::uint32_t gml_bgr);
    void color(std::uint32_t gml_bgr) noexcept;
    void alpha(double alpha);
    void font(int id);
    void rectangle(double x1, double y1, double x2, double y2, bool outline);
    void circle(double x, double y, double radius, bool outline);
    void text(double x, double y, std::string_view text);
    void rgba_pixel(int x,int y,std::uint32_t rgba,std::uint32_t bgr,double opacity);
    void rgba_pixel_tinted(int x,int y,std::uint32_t rgba,unsigned tint,double opacity) noexcept;
    void save_bmp(const std::filesystem::path& path) const;
    [[nodiscard]] std::uint64_t hash() const noexcept;
private:
    void plot(int x, int y);
    int width_, height_;
    std::vector<std::uint32_t> pixels_;
    std::uint32_t rgb_ = 0xffffff;
    unsigned alpha_ = 255;
    int font_scale_ = 1;
};
} // namespace regml
