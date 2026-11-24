#pragma once

#include "batcher.hpp"
#include "error.hpp"
#include "fbo.hpp"
#include "gl_common.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace regml::glmodern {

class GLModernCanvas {
public:
    using PixelShader = std::uint32_t (*)(void *context, int x, int y, std::uint32_t rgba) noexcept;

    GLModernCanvas(int width, int height, bool main_screen = false);
    ~GLModernCanvas();

    GLModernCanvas(const GLModernCanvas &) = delete;
    GLModernCanvas &operator=(const GLModernCanvas &) = delete;

    GLModernCanvas(GLModernCanvas &&other) noexcept;
    GLModernCanvas &operator=(GLModernCanvas &&other) noexcept;

    [[nodiscard]] double render_scale() const noexcept { return render_scale_; }
    [[nodiscard]] int logical_width() const noexcept { return logical_width_; }
    [[nodiscard]] int logical_height() const noexcept { return logical_height_; }
    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }

    void gpu_target(bool enabled) noexcept { (void)enabled; }
    [[nodiscard]] bool gpu_target() const noexcept { return true; }

    [[nodiscard]] const GLFramebuffer &fbo() const noexcept { return fbo_; }
    [[nodiscard]] GLFramebuffer &fbo() noexcept { return fbo_; }
    [[nodiscard]] GLuint texture_id() const noexcept { return fbo_.texture_id(); }

    [[nodiscard]] SpriteBatcher &batcher() noexcept { return batcher_; }
    [[nodiscard]] const SpriteBatcher &batcher() const noexcept { return batcher_; }

    [[nodiscard]] std::uint32_t pixel(int x, int y) const;
    bool readback(std::span<std::uint32_t> out_pixels) const;

    [[nodiscard]] std::span<const std::uint32_t> pixels() const;
    [[nodiscard]] std::span<std::uint32_t> pixels();

    void clear(std::uint32_t gml_bgr);
    void clear_transparent() noexcept;

    void color(std::uint32_t gml_bgr) noexcept;
    void alpha(double alpha);

    void blend_enable(bool enabled) noexcept;
    void blend_mode(int source_rgb,
                    int destination_rgb,
                    int source_alpha,
                    int destination_alpha,
                    int equation_rgb = 0,
                    int equation_alpha = 0) noexcept;

    void alpha_test_enable(bool enabled) noexcept;
    void alpha_test_ref(unsigned value) noexcept;

    void color_write_enable(bool red, bool green, bool blue, bool alpha) noexcept;

    void rectangle(double x1, double y1, double x2, double y2, bool outline);
    void circle(double x, double y, double radius, bool outline);
    void ellipse(double x1, double y1, double x2, double y2, bool outline);
    void round_rectangle(double x1, double y1, double x2, double y2, double x_radius, double y_radius, bool outline);
    void line(double x1, double y1, double x2, double y2, double width = 1);
    void triangle(double x1, double y1, double x2, double y2, double x3, double y3, bool outline);

    void draw_surface(const GLModernCanvas &source,
                      double x,
                      double y,
                      double sx = 1,
                      double sy = 1,
                      double angle = 0,
                      std::uint32_t tint = 0xffffff,
                      double opacity = 1);

    void draw_surface_part(const GLModernCanvas &source,
                           double left,
                           double top,
                           double width,
                           double height,
                           double x,
                           double y,
                           double sx,
                           double sy,
                           std::uint32_t tint,
                           double opacity);

    void flush();

    void font(int id) noexcept { (void)id; }
    void pixel_shader(PixelShader shader, void *context = nullptr) noexcept {
        pixel_shader_ = shader;
        pixel_shader_context_ = context;
    }
    [[nodiscard]] bool pixel_shader_active() const noexcept { return pixel_shader_ != nullptr; }

    [[nodiscard]] std::uint64_t hash() const noexcept;

private:
    void ensure_render_target() const;

    struct CoordinateScale {
        double &active;
        double factor;
        explicit CoordinateScale(double &scale) : active(scale), factor(scale) { active = 1; }
        ~CoordinateScale() { active = factor; }
    };

    int logical_width_ = 0;
    int logical_height_ = 0;
    int width_ = 0;
    int height_ = 0;
    double render_scale_ = 1.0;
    mutable double coordinate_scale_ = 1.0;

    GLFramebuffer fbo_;
    mutable SpriteBatcher batcher_;
    Mat4 projection_{};

    std::uint32_t gml_color_ = 0xffffff;
    double alpha_ = 1.0;
    std::uint32_t current_rgba_ = 0xffffffff;

    bool blend_enabled_ = true;
    std::array<int, 4> blend_factors_{5, 6, 5, 6};
    std::array<int, 2> blend_equations_{0, 0};

    bool alpha_test_enabled_ = false;
    unsigned alpha_test_ref_ = 0;

    std::uint32_t color_write_mask_ = 0xffffffffU;
    PixelShader pixel_shader_ = nullptr;
    void *pixel_shader_context_ = nullptr;

    mutable std::vector<std::uint32_t> pixels_cache_;
    mutable bool pixels_cache_dirty_ = true;
};

} // namespace regml::glmodern
