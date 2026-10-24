#pragma once
#include "error.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>
namespace regml {
std::array<unsigned, 7> font_bitmap(unsigned char character);
// the CPU draws into this buffer; SDL just puts it on screen
class Canvas {
  public:
    using PixelShader = std::uint32_t (*)(void *context, int x, int y, std::uint32_t rgba) noexcept;
    Canvas(int width, int height, bool main_screen = false);
    double render_scale() const noexcept { return render_scale_; }
    int logical_width() const noexcept { return logical_width_; }
    int logical_height() const noexcept { return logical_height_; }
    void gpu_target(bool enabled);
    bool gpu_target() const noexcept { return gpu_; }
    void profile_text(bool enabled) noexcept { profile_text_ = enabled; }
    double take_text_profile_ms() noexcept {
        const double result = text_profile_ms_;
        text_profile_ms_ = 0;
        return result;
    }
    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }
    [[nodiscard]] std::span<std::uint32_t> pixels() noexcept { return pixels_; }
    [[nodiscard]] std::span<const std::uint32_t> pixels() const noexcept { return pixels_; }
    [[nodiscard]] std::uint32_t pixel(int x, int y) const;
    void clear(std::uint32_t gml_bgr);
    void clear_transparent() noexcept;
    void color(std::uint32_t gml_bgr) noexcept;
    void alpha(double alpha);
    void blend_enable(bool enabled) noexcept { blend_enabled_ = enabled; }
    void alpha_test_enable(bool enabled) noexcept { alpha_test_enabled_ = enabled; }
    void alpha_test_ref(unsigned value) noexcept { alpha_test_ref_ = value > 255 ? 255 : value; }
    void color_write_enable(bool red, bool green, bool blue, bool alpha) noexcept {
        color_write_ = {red, green, blue, alpha};
    }
    void pixel_shader(PixelShader shader, void *context = nullptr) noexcept {
        pixel_shader_ = shader;
        pixel_shader_context_ = context;
    }
    void font(int id);
    void rectangle(double x1, double y1, double x2, double y2, bool outline);
    void circle(double x, double y, double radius, bool outline);
    void ellipse(double x1, double y1, double x2, double y2, bool outline);
    void round_rectangle(double x1, double y1, double x2, double y2, double x_radius, double y_radius, bool outline);
    void line(double x1, double y1, double x2, double y2, double width = 1);
    void triangle(double x1, double y1, double x2, double y2, double x3, double y3, bool outline);
    void draw_surface(const Canvas &source,
                      double x,
                      double y,
                      double sx = 1,
                      double sy = 1,
                      double angle = 0,
                      std::uint32_t tint = 0xffffff,
                      double opacity = 1);
    void blit_rgba(std::span<const std::uint32_t> source,
                   int source_width,
                   int source_x,
                   int source_y,
                   int destination_x,
                   int destination_y,
                   int width,
                   int height,
                   unsigned tint,
                   double opacity,
                   std::int8_t source_alpha_mode = 0) noexcept;
    void text(double x, double y, std::string_view text);
    void text(double x,
              double y,
              std::string_view text,
              double sx,
              double sy,
              double angle,
              int halign,
              int valign);
    void debug_text_overlay(std::span<const std::string_view> lines);
    void rgba_pixel(int x, int y, std::uint32_t rgba, std::uint32_t bgr, double opacity);
    void rgba_pixel_tinted(int x, int y, std::uint32_t rgba, unsigned tint, double opacity) noexcept;
    void rgba_span(int x, int y, int count, std::uint32_t rgba, unsigned tint, double opacity) noexcept;
    void save_bmp(const std::filesystem::path &path) const;
    [[nodiscard]] std::uint64_t hash() const noexcept;

  private:
    void plot(int x, int y);
    void store_pixel(int x, int y, std::uint32_t rgba) noexcept;
    bool color_write_all() const noexcept {
        return color_write_[0] && color_write_[1] && color_write_[2] && color_write_[3];
    }
    struct CoordinateScale {
        double &active;
        double factor;
        explicit CoordinateScale(double &scale) : active(scale), factor(scale) { active = 1; }
        ~CoordinateScale() { active = factor; }
    };
    double render_scale_ = 1, coordinate_scale_ = 1;
    int logical_width_, logical_height_;
    bool gpu_ = false;
    bool profile_text_ = false;
    bool blend_enabled_ = true;
    bool alpha_test_enabled_ = false;
    unsigned alpha_test_ref_ = 0;
    std::array<bool, 4> color_write_{true, true, true, true};
    double text_profile_ms_ = 0;
    PixelShader pixel_shader_ = nullptr;
    void *pixel_shader_context_ = nullptr;
    int width_, height_;
    std::vector<std::uint32_t> pixels_;
    std::uint32_t rgb_ = 0xffffff;
    unsigned alpha_ = 255;
    int font_scale_ = 1;
};
} // namespace regml
