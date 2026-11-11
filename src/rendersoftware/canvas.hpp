#pragma once
#include "error.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>
namespace regml {
class GameAssets;
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
    void blend_mode(int source_rgb,
                    int destination_rgb,
                    int source_alpha,
                    int destination_alpha,
                    int equation_rgb = 0,
                    int equation_alpha = 0) noexcept {
        blend_factors_ = {source_rgb, destination_rgb, source_alpha, destination_alpha};
        blend_equations_ = {equation_rgb, equation_alpha};
        standard_blend_ =
            blend_factors_ == std::array<int, 4>{5, 6, 5, 6} && blend_equations_ == std::array<int, 2>{0, 0};
    }
    void alpha_test_enable(bool enabled) noexcept { alpha_test_enabled_ = enabled; }
    void alpha_test_ref(unsigned value) noexcept { alpha_test_ref_ = value > 255 ? 255 : value; }
    void color_write_enable(bool red, bool green, bool blue, bool alpha) noexcept {
        color_write_mask_ = (red ? 0x00ff0000U : 0U) | (green ? 0x0000ff00U : 0U) |
                            (blue ? 0x000000ffU : 0U) | (alpha ? 0xff000000U : 0U);
    }
    void pixel_shader(PixelShader shader, void *context = nullptr) noexcept {
        pixel_shader_ = shader;
        pixel_shader_context_ = context;
    }
    [[nodiscard]] bool pixel_shader_active() const noexcept { return pixel_shader_ != nullptr; }
    void font(int id);
    void rectangle(double x1, double y1, double x2, double y2, bool outline);
    void circle(double x, double y, double radius, bool outline);
    void ellipse(double x1, double y1, double x2, double y2, bool outline);
    void round_rectangle(
        double x1, double y1, double x2, double y2, double x_radius, double y_radius, bool outline);
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
    void save_png(const std::filesystem::path &path) const;
    [[nodiscard]] std::uint64_t hash() const noexcept;

  private:
    friend class GameAssets;
    void plot(int x, int y);
    void plot_in_bounds(int x, int y);
    void store_pixel(int x, int y, std::uint32_t rgba) noexcept;
    bool gpu_readback(std::span<std::uint32_t> pixels) const noexcept;
    bool blit_rgba_scaled_opaque(std::span<const std::uint32_t> source,
                                 int source_width,
                                 std::span<const int> source_x,
                                 std::span<const int> source_y,
                                 int destination_x,
                                 int destination_y,
                                 double opacity) noexcept;
    bool blit_rgba_scaled_binary_alpha(std::span<const std::uint32_t> source,
                                       int source_width,
                                       std::span<const int> source_x,
                                       std::span<const int> source_y,
                                       int destination_x,
                                       int destination_y) noexcept;
    bool blit_rgba_scaled_tinted(std::span<const std::uint32_t> source,
                                 int source_width,
                                 std::span<const int> source_x,
                                 std::span<const int> source_y,
                                 int destination_x,
                                 int destination_y,
                                 unsigned tint,
                                 double opacity) noexcept;
    bool color_write_all() const noexcept { return color_write_mask_ == 0xffffffffU; }
    bool standard_blend() const noexcept { return standard_blend_; }
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
    bool standard_blend_ = true;
    std::array<int, 4> blend_factors_{5, 6, 5, 6};
    std::array<int, 2> blend_equations_{0, 0};
    bool alpha_test_enabled_ = false;
    unsigned alpha_test_ref_ = 0;
    std::uint32_t color_write_mask_ = 0xffffffffU;
    double text_profile_ms_ = 0;
    PixelShader pixel_shader_ = nullptr;
    void *pixel_shader_context_ = nullptr;
    int width_, height_;
    std::vector<std::uint32_t> pixels_;
    mutable std::vector<std::uint32_t> readback_scratch_;
    std::uint32_t rgb_ = 0xffffff;
    unsigned alpha_ = 255;
    int font_scale_ = 1;
};
} // namespace regml
