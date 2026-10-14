#pragma once
#include "error.hpp"
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>
namespace regml {
// CPU framebuffer; SDL only handles presentation.
class Canvas {
  public:
    Canvas(int width, int height, bool main_screen = false);
    double render_scale() const noexcept { return render_scale_; }
    int logical_width() const noexcept { return logical_width_; }
    int logical_height() const noexcept { return logical_height_; }
    void gpu_target(bool enabled);
    bool gpu_target() const noexcept { return gpu_; }
    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }
    [[nodiscard]] std::span<std::uint32_t> pixels() noexcept { return pixels_; }
    [[nodiscard]] std::span<const std::uint32_t> pixels() const noexcept { return pixels_; }
    [[nodiscard]] std::uint32_t pixel(int x, int y) const;
    void clear(std::uint32_t gml_bgr);
    void clear_transparent() noexcept;
    void color(std::uint32_t gml_bgr) noexcept;
    void alpha(double alpha);
    void font(int id);
    void rectangle(double x1, double y1, double x2, double y2, bool outline);
    void circle(double x, double y, double radius, bool outline);
    void line(double x1, double y1, double x2, double y2, double width = 1);
    void triangle(double x1, double y1, double x2, double y2, double x3, double y3, bool outline);
    void draw_surface(const Canvas &source, double x, double y, double sx = 1, double sy = 1, double angle = 0,
                      std::uint32_t tint = 0xffffff, double opacity = 1);
    void blit_rgba(std::span<const std::uint32_t> source, int source_width, int source_x, int source_y,
                   int destination_x, int destination_y, int width, int height, unsigned tint, double opacity,
                   std::int8_t source_alpha_mode = 0) noexcept;
    void text(double x, double y, std::string_view text);
    void debug_text_overlay(std::span<const std::string_view> lines);
    void rgba_pixel(int x, int y, std::uint32_t rgba, std::uint32_t bgr, double opacity);
    void rgba_pixel_tinted(int x, int y, std::uint32_t rgba, unsigned tint, double opacity) noexcept;
    void rgba_span(int x, int y, int count, std::uint32_t rgba, unsigned tint, double opacity) noexcept;
    void save_bmp(const std::filesystem::path &path) const;
    [[nodiscard]] std::uint64_t hash() const noexcept;

  private:
    void plot(int x, int y);
    struct CoordinateScale {
        double &active;
        double factor;
        explicit CoordinateScale(double &scale) : active(scale), factor(scale) { active = 1; }
        ~CoordinateScale() { active = factor; }
    };
    double render_scale_ = 1, coordinate_scale_ = 1;
    int logical_width_, logical_height_;
    bool gpu_ = false;
    int width_, height_;
    std::vector<std::uint32_t> pixels_;
    std::uint32_t rgb_ = 0xffffff;
    unsigned alpha_ = 255;
    int font_scale_ = 1;
};
} // namespace regml
