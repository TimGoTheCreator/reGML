#include "canvas.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace regml::glmodern {

namespace {

void finite(double value) {
    if (!std::isfinite(value)) {
        throw Error(ErrorKind::vm, "drawing coordinate must be finite");
    }
}

} // namespace

GLModernCanvas::GLModernCanvas(int width, int height, bool main_screen)
    : logical_width_(width), logical_height_(height), width_(width), height_(height) {
#if REGML_240P
    if (main_screen) {
        render_scale_ = coordinate_scale_ = 0.5;
        width_ = (width + 1) / 2;
        height_ = (height + 1) / 2;
    }
#else
    (void)main_screen;
#endif

    constexpr int max_canvas_dimension = 8192;
    constexpr std::uint64_t max_canvas_pixels = 64ULL * 1024 * 1024;
    if (width < 1 || height < 1 || width > max_canvas_dimension || height > max_canvas_dimension ||
        static_cast<std::uint64_t>(width_) * static_cast<std::uint64_t>(height_) > max_canvas_pixels) {
        throw Error(ErrorKind::unsupported, "canvas dimensions exceed the supported 8192x8192 limit");
    }

    fbo_.init(width_, height_);
    projection_ = Mat4::ortho_2d(static_cast<float>(width_), static_cast<float>(height_));

    clear_transparent();
}

GLModernCanvas::~GLModernCanvas() {
    flush();
}

GLModernCanvas::GLModernCanvas(GLModernCanvas &&other) noexcept
    : logical_width_(other.logical_width_),
      logical_height_(other.logical_height_),
      width_(other.width_),
      height_(other.height_),
      render_scale_(other.render_scale_),
      coordinate_scale_(other.coordinate_scale_),
      fbo_(std::move(other.fbo_)),
      batcher_(std::move(other.batcher_)),
      projection_(other.projection_),
      gml_color_(other.gml_color_),
      alpha_(other.alpha_),
      current_rgba_(other.current_rgba_),
      blend_enabled_(other.blend_enabled_),
      blend_factors_(other.blend_factors_),
      blend_equations_(other.blend_equations_),
      alpha_test_enabled_(other.alpha_test_enabled_),
      alpha_test_ref_(other.alpha_test_ref_),
      color_write_mask_(other.color_write_mask_),
      pixel_shader_(other.pixel_shader_),
      pixel_shader_context_(other.pixel_shader_context_),
      pixels_cache_(std::move(other.pixels_cache_)),
      pixels_cache_dirty_(other.pixels_cache_dirty_) {
    other.logical_width_ = 0;
    other.logical_height_ = 0;
    other.width_ = 0;
    other.height_ = 0;
}

GLModernCanvas &GLModernCanvas::operator=(GLModernCanvas &&other) noexcept {
    if (this != &other) {
        flush();
        logical_width_ = other.logical_width_;
        logical_height_ = other.logical_height_;
        width_ = other.width_;
        height_ = other.height_;
        render_scale_ = other.render_scale_;
        coordinate_scale_ = other.coordinate_scale_;
        fbo_ = std::move(other.fbo_);
        batcher_ = std::move(other.batcher_);
        projection_ = other.projection_;
        gml_color_ = other.gml_color_;
        alpha_ = other.alpha_;
        current_rgba_ = other.current_rgba_;
        blend_enabled_ = other.blend_enabled_;
        blend_factors_ = other.blend_factors_;
        blend_equations_ = other.blend_equations_;
        alpha_test_enabled_ = other.alpha_test_enabled_;
        alpha_test_ref_ = other.alpha_test_ref_;
        color_write_mask_ = other.color_write_mask_;
        pixel_shader_ = other.pixel_shader_;
        pixel_shader_context_ = other.pixel_shader_context_;
        pixels_cache_ = std::move(other.pixels_cache_);
        pixels_cache_dirty_ = other.pixels_cache_dirty_;

        other.logical_width_ = 0;
        other.logical_height_ = 0;
        other.width_ = 0;
        other.height_ = 0;
    }
    return *this;
}

void GLModernCanvas::ensure_render_target() const {
    fbo_.bind(true);
    if (!batcher_.is_active()) {
        batcher_.begin(projection_);
    }
}

void GLModernCanvas::flush() {
    batcher_.flush();
    pixels_cache_dirty_ = true;
}

std::uint32_t GLModernCanvas::pixel(int x, int y) const {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) {
        throw Error(ErrorKind::usage, "pixel is outside framebuffer");
    }
    const_cast<GLModernCanvas *>(this)->flush();
    return fbo_.pixel(x, y);
}

bool GLModernCanvas::readback(std::span<std::uint32_t> out_pixels) const {
    const_cast<GLModernCanvas *>(this)->flush();
    return fbo_.readback(out_pixels);
}

std::span<const std::uint32_t> GLModernCanvas::pixels() const {
    const_cast<GLModernCanvas *>(this)->flush();
    if (pixels_cache_dirty_) {
        pixels_cache_.resize(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_));
        fbo_.readback(pixels_cache_);
        pixels_cache_dirty_ = false;
    }
    return pixels_cache_;
}

std::span<std::uint32_t> GLModernCanvas::pixels() {
    flush();
    if (pixels_cache_dirty_) {
        pixels_cache_.resize(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_));
        fbo_.readback(pixels_cache_);
        pixels_cache_dirty_ = false;
    }
    return pixels_cache_;
}

void GLModernCanvas::clear(std::uint32_t gml_bgr) {
    batcher_.flush();
    fbo_.bind(true);
    pixels_cache_dirty_ = true;

    const float r = static_cast<float>(gml_bgr & 0xffU) / 255.0f;
    const float g = static_cast<float>((gml_bgr >> 8) & 0xffU) / 255.0f;
    const float b = static_cast<float>((gml_bgr >> 16) & 0xffU) / 255.0f;
    if (glClearColor && glClear) {
        glClearColor(r, g, b, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
}

void GLModernCanvas::clear_transparent() noexcept {
    batcher_.flush();
    fbo_.bind(true);
    pixels_cache_dirty_ = true;

    if (glClearColor && glClear) {
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
}

void GLModernCanvas::color(std::uint32_t gml_bgr) noexcept {
    gml_color_ = gml_bgr;
    current_rgba_ = gml_to_rgba(gml_color_, alpha_);
}

void GLModernCanvas::alpha(double alpha) {
    finite(alpha);
    alpha_ = std::clamp(alpha, 0.0, 1.0);
    current_rgba_ = gml_to_rgba(gml_color_, alpha_);
}

void GLModernCanvas::blend_enable(bool enabled) noexcept {
    blend_enabled_ = enabled;
    batcher_.set_blend_enable(enabled);
}

void GLModernCanvas::blend_mode(int source_rgb,
                                int destination_rgb,
                                int source_alpha,
                                int destination_alpha,
                                int equation_rgb,
                                int equation_alpha) noexcept {
    blend_factors_ = {source_rgb, destination_rgb, source_alpha, destination_alpha};
    blend_equations_ = {equation_rgb, equation_alpha};
    batcher_.set_blend_mode(source_rgb, destination_rgb, source_alpha, destination_alpha, equation_rgb, equation_alpha);
}

void GLModernCanvas::alpha_test_enable(bool enabled) noexcept {
    alpha_test_enabled_ = enabled;
    batcher_.set_alpha_test(alpha_test_enabled_, static_cast<float>(alpha_test_ref_) / 255.0f);
}

void GLModernCanvas::alpha_test_ref(unsigned value) noexcept {
    alpha_test_ref_ = value > 255 ? 255 : value;
    batcher_.set_alpha_test(alpha_test_enabled_, static_cast<float>(alpha_test_ref_) / 255.0f);
}

void GLModernCanvas::color_write_enable(bool red, bool green, bool blue, bool alpha) noexcept {
    const std::uint32_t new_mask = (red ? 0x00ff0000U : 0U) | (green ? 0x0000ff00U : 0U) |
                                   (blue ? 0x000000ffU : 0U) | (alpha ? 0xff000000U : 0U);
    if (new_mask == color_write_mask_) {
        return;
    }
    batcher_.flush();
    color_write_mask_ = new_mask;
    if (glColorMask) {
        glColorMask(red ? GL_TRUE : GL_FALSE,
                    green ? GL_TRUE : GL_FALSE,
                    blue ? GL_TRUE : GL_FALSE,
                    alpha ? GL_TRUE : GL_FALSE);
    }
}

void GLModernCanvas::rectangle(double x1, double y1, double x2, double y2, bool outline) {
    CoordinateScale coordinates(coordinate_scale_);
    x1 *= coordinates.factor;
    y1 *= coordinates.factor;
    x2 *= coordinates.factor;
    y2 *= coordinates.factor;

    finite(x1);
    finite(y1);
    finite(x2);
    finite(y2);

    const auto left = std::floor(std::min(x1, x2)), right = std::ceil(std::max(x1, x2));
    const auto top = std::floor(std::min(y1, y2)), bottom = std::ceil(std::max(y1, y2));
    const int l = static_cast<int>(std::clamp(left, 0.0, static_cast<double>(width_)));
    const int r = static_cast<int>(std::clamp(right, 0.0, static_cast<double>(width_)));
    const int t = static_cast<int>(std::clamp(top, 0.0, static_cast<double>(height_)));
    const int b = static_cast<int>(std::clamp(bottom, 0.0, static_cast<double>(height_)));
    if (l >= r || t >= b) {
        return;
    }

    ensure_render_target();
    pixels_cache_dirty_ = true;

    if (!outline) {
        batcher_.draw_quad(0, static_cast<float>(l), static_cast<float>(t),
                           static_cast<float>(r - l), static_cast<float>(b - t),
                           0.0f, 0.0f, 0.0f, 0.0f, current_rgba_);
    } else {
        batcher_.draw_quad(0, static_cast<float>(l), static_cast<float>(t),
                           static_cast<float>(r - l), 1.0f,
                           0.0f, 0.0f, 0.0f, 0.0f, current_rgba_);
        if (b > t + 1) {
            batcher_.draw_quad(0, static_cast<float>(l), static_cast<float>(b - 1),
                               static_cast<float>(r - l), 1.0f,
                               0.0f, 0.0f, 0.0f, 0.0f, current_rgba_);
        }
        if (b > t + 2) {
            batcher_.draw_quad(0, static_cast<float>(l), static_cast<float>(t + 1),
                               1.0f, static_cast<float>(b - t - 2),
                               0.0f, 0.0f, 0.0f, 0.0f, current_rgba_);
        }
        if (b > t + 2 && r > l + 1) {
            batcher_.draw_quad(0, static_cast<float>(r - 1), static_cast<float>(t + 1),
                               1.0f, static_cast<float>(b - t - 2),
                               0.0f, 0.0f, 0.0f, 0.0f, current_rgba_);
        }
    }
}

void GLModernCanvas::line(double x1, double y1, double x2, double y2, double width) {
    CoordinateScale coordinates(coordinate_scale_);
    x1 *= coordinates.factor;
    y1 *= coordinates.factor;
    x2 *= coordinates.factor;
    y2 *= coordinates.factor;
    width *= coordinates.factor;

    finite(x1);
    finite(y1);
    finite(x2);
    finite(y2);
    finite(width);

    if (width <= 0.0 || width > 2048.0 ||
        std::max({std::fabs(x1), std::fabs(y1), std::fabs(x2), std::fabs(y2)}) > 1e9) {
        throw Error(ErrorKind::unsupported, "line is outside supported limits");
    }

    ensure_render_target();
    pixels_cache_dirty_ = true;

    const double dx = x2 - x1;
    const double dy = y2 - y1;
    const double length = std::hypot(dx, dy);

    if (length == 0.0) {
        batcher_.draw_quad(0, static_cast<float>(x1), static_cast<float>(y1),
                           static_cast<float>(width), static_cast<float>(width),
                           0.0f, 0.0f, 0.0f, 0.0f, current_rgba_);
        return;
    }

    const double nx = -dy / length * (width * 0.5);
    const double ny = dx / length * (width * 0.5);

    const Vertex v0{static_cast<float>(x1 + nx), static_cast<float>(y1 + ny), 0.0f, 0.0f, current_rgba_};
    const Vertex v1{static_cast<float>(x2 + nx), static_cast<float>(y2 + ny), 0.0f, 0.0f, current_rgba_};
    const Vertex v2{static_cast<float>(x2 - nx), static_cast<float>(y2 - ny), 0.0f, 0.0f, current_rgba_};
    const Vertex v3{static_cast<float>(x1 - nx), static_cast<float>(y1 - ny), 0.0f, 0.0f, current_rgba_};

    batcher_.draw_quad_corners(0, {v0, v1, v2, v3});
}

void GLModernCanvas::triangle(double x1, double y1, double x2, double y2, double x3, double y3, bool outline) {
    CoordinateScale coordinates(coordinate_scale_);
    x1 *= coordinates.factor;
    y1 *= coordinates.factor;
    x2 *= coordinates.factor;
    y2 *= coordinates.factor;
    x3 *= coordinates.factor;
    y3 *= coordinates.factor;

    finite(x1);
    finite(y1);
    finite(x2);
    finite(y2);
    finite(x3);
    finite(y3);

    if (outline) {
        line(x1, y1, x2, y2, 1.0);
        line(x2, y2, x3, y3, 1.0);
        line(x3, y3, x1, y1, 1.0);
        return;
    }

    ensure_render_target();
    pixels_cache_dirty_ = true;

    const Vertex va{static_cast<float>(x1), static_cast<float>(y1), 0.0f, 0.0f, current_rgba_};
    const Vertex vb{static_cast<float>(x2), static_cast<float>(y2), 0.0f, 0.0f, current_rgba_};
    const Vertex vc{static_cast<float>(x3), static_cast<float>(y3), 0.0f, 0.0f, current_rgba_};

    // Submitting {va, vb, vc, vc} yields triangle (va, vb, vc) and zero-area degenerate (vc, vc, va)
    batcher_.draw_quad_corners(0, {va, vb, vc, vc});
}

void GLModernCanvas::circle(double x, double y, double radius, bool outline) {
    ellipse(x - radius, y - radius, x + radius, y + radius, outline);
}

void GLModernCanvas::ellipse(double x1, double y1, double x2, double y2, bool outline) {
    CoordinateScale coordinates(coordinate_scale_);
    x1 *= coordinates.factor;
    y1 *= coordinates.factor;
    x2 *= coordinates.factor;
    y2 *= coordinates.factor;

    finite(x1);
    finite(y1);
    finite(x2);
    finite(y2);

    if (std::max({std::fabs(x1), std::fabs(y1), std::fabs(x2), std::fabs(y2)}) > 1e9) {
        throw Error(ErrorKind::unsupported, "ellipse is outside supported coordinate limits");
    }

    const double left_edge = std::min(x1, x2), right_edge = std::max(x1, x2);
    const double top_edge = std::min(y1, y2), bottom_edge = std::max(y1, y2);
    const double center_x = (left_edge + right_edge) * 0.5;
    const double center_y = (top_edge + bottom_edge) * 0.5;
    const double radius_x = (right_edge - left_edge) * 0.5;
    const double radius_y = (bottom_edge - top_edge) * 0.5;

    if (radius_x <= 0.0 || radius_y <= 0.0) {
        return;
    }

    ensure_render_target();
    pixels_cache_dirty_ = true;

    const int segments = std::clamp(static_cast<int>(std::max(radius_x, radius_y) * 2.0), 16, 64);
    constexpr double two_pi = 2.0 * std::numbers::pi;

    if (outline) {
        double prev_x = center_x + radius_x;
        double prev_y = center_y;
        for (int i = 1; i <= segments; ++i) {
            const double angle = (static_cast<double>(i) / segments) * two_pi;
            const double cur_x = center_x + radius_x * std::cos(angle);
            const double cur_y = center_y + radius_y * std::sin(angle);
            line(prev_x, prev_y, cur_x, cur_y, 1.0);
            prev_x = cur_x;
            prev_y = cur_y;
        }
    } else {
        const Vertex center_vertex{static_cast<float>(center_x), static_cast<float>(center_y), 0.0f, 0.0f, current_rgba_};
        for (int i = 0; i < segments; ++i) {
            const double a0 = (static_cast<double>(i) / segments) * two_pi;
            const double a1 = (static_cast<double>(i + 1) / segments) * two_pi;
            const Vertex v0{static_cast<float>(center_x + radius_x * std::cos(a0)),
                           static_cast<float>(center_y + radius_y * std::sin(a0)),
                           0.0f, 0.0f, current_rgba_};
            const Vertex v1{static_cast<float>(center_x + radius_x * std::cos(a1)),
                           static_cast<float>(center_y + radius_y * std::sin(a1)),
                           0.0f, 0.0f, current_rgba_};
            batcher_.draw_quad_corners(0, {center_vertex, v0, v1, v1});
        }
    }
}

void GLModernCanvas::round_rectangle(
    double x1, double y1, double x2, double y2, double x_radius, double y_radius, bool outline) {
    CoordinateScale coordinates(coordinate_scale_);
    x1 *= coordinates.factor;
    y1 *= coordinates.factor;
    x2 *= coordinates.factor;
    y2 *= coordinates.factor;
    x_radius *= coordinates.factor;
    y_radius *= coordinates.factor;

    finite(x1);
    finite(y1);
    finite(x2);
    finite(y2);
    finite(x_radius);
    finite(y_radius);

    if (std::max({std::fabs(x1), std::fabs(y1), std::fabs(x2), std::fabs(y2), x_radius, y_radius}) > 1e9) {
        throw Error(ErrorKind::unsupported, "rounded rectangle is outside supported limits");
    }

    const double left_edge = std::min(x1, x2), right_edge = std::max(x1, x2);
    const double top_edge = std::min(y1, y2), bottom_edge = std::max(y1, y2);
    const double half_w = (right_edge - left_edge) * 0.5;
    const double half_h = (bottom_edge - top_edge) * 0.5;

    if (half_w <= 0.0 || half_h <= 0.0) {
        return;
    }

    x_radius = std::clamp(x_radius, 0.0, half_w);
    y_radius = std::clamp(y_radius, 0.0, half_h);

    if (x_radius == 0.0 || y_radius == 0.0) {
        rectangle(x1, y1, x2, y2, outline);
        return;
    }

    ensure_render_target();
    pixels_cache_dirty_ = true;

    const double cx0 = left_edge + x_radius;
    const double cx1 = right_edge - x_radius;
    const double cy0 = top_edge + y_radius;
    const double cy1 = bottom_edge - y_radius;

    const int corner_segs = std::clamp(static_cast<int>(std::max(x_radius, y_radius)), 4, 16);
    constexpr double pi_2 = 0.5 * std::numbers::pi;
    constexpr double pi = std::numbers::pi;

    if (outline) {
        line(cx0, top_edge, cx1, top_edge, 1.0);
        line(right_edge, cy0, right_edge, cy1, 1.0);
        line(cx1, bottom_edge, cx0, bottom_edge, 1.0);
        line(left_edge, cy1, left_edge, cy0, 1.0);

        auto draw_arc = [&](double cx, double cy, double start_angle) {
            double px = cx + x_radius * std::cos(start_angle);
            double py = cy + y_radius * std::sin(start_angle);
            for (int i = 1; i <= corner_segs; ++i) {
                const double a = start_angle + (static_cast<double>(i) / corner_segs) * pi_2;
                const double x = cx + x_radius * std::cos(a);
                const double y = cy + y_radius * std::sin(a);
                line(px, py, x, y, 1.0);
                px = x;
                py = y;
            }
        };

        draw_arc(cx1, cy0, -pi_2);
        draw_arc(cx1, cy1, 0.0);
        draw_arc(cx0, cy1, pi_2);
        draw_arc(cx0, cy0, pi);
    } else {
        batcher_.draw_quad(0, static_cast<float>(cx0), static_cast<float>(cy0),
                           static_cast<float>(cx1 - cx0), static_cast<float>(cy1 - cy0),
                           0.0f, 0.0f, 0.0f, 0.0f, current_rgba_);
        batcher_.draw_quad(0, static_cast<float>(cx0), static_cast<float>(top_edge),
                           static_cast<float>(cx1 - cx0), static_cast<float>(y_radius),
                           0.0f, 0.0f, 0.0f, 0.0f, current_rgba_);
        batcher_.draw_quad(0, static_cast<float>(cx0), static_cast<float>(cy1),
                           static_cast<float>(cx1 - cx0), static_cast<float>(y_radius),
                           0.0f, 0.0f, 0.0f, 0.0f, current_rgba_);
        batcher_.draw_quad(0, static_cast<float>(left_edge), static_cast<float>(cy0),
                           static_cast<float>(x_radius), static_cast<float>(cy1 - cy0),
                           0.0f, 0.0f, 0.0f, 0.0f, current_rgba_);
        batcher_.draw_quad(0, static_cast<float>(cx1), static_cast<float>(cy0),
                           static_cast<float>(x_radius), static_cast<float>(cy1 - cy0),
                           0.0f, 0.0f, 0.0f, 0.0f, current_rgba_);

        auto fill_arc = [&](double cx, double cy, double start_angle) {
            const Vertex cv{static_cast<float>(cx), static_cast<float>(cy), 0.0f, 0.0f, current_rgba_};
            for (int i = 0; i < corner_segs; ++i) {
                const double a0 = start_angle + (static_cast<double>(i) / corner_segs) * pi_2;
                const double a1 = start_angle + (static_cast<double>(i + 1) / corner_segs) * pi_2;
                const Vertex v0{static_cast<float>(cx + x_radius * std::cos(a0)),
                               static_cast<float>(cy + y_radius * std::sin(a0)),
                               0.0f, 0.0f, current_rgba_};
                const Vertex v1{static_cast<float>(cx + x_radius * std::cos(a1)),
                               static_cast<float>(cy + y_radius * std::sin(a1)),
                               0.0f, 0.0f, current_rgba_};
                batcher_.draw_quad_corners(0, {cv, v0, v1, v1});
            }
        };

        fill_arc(cx1, cy0, -pi_2);
        fill_arc(cx1, cy1, 0.0);
        fill_arc(cx0, cy1, pi_2);
        fill_arc(cx0, cy0, pi);
    }
}

void GLModernCanvas::draw_surface(const GLModernCanvas &source,
                                  double x,
                                  double y,
                                  double sx,
                                  double sy,
                                  double angle,
                                  std::uint32_t tint,
                                  double opacity) {
    x *= render_scale_;
    y *= render_scale_;
    sx *= render_scale_;
    sy *= render_scale_;

    finite(x);
    finite(y);
    finite(sx);
    finite(sy);
    finite(angle);
    finite(opacity);

    if (sx == 0.0 || sy == 0.0 || source.width_ <= 0 || source.height_ <= 0 || opacity <= 0.0) {
        return;
    }

    const_cast<GLModernCanvas &>(source).flush();

    ensure_render_target();
    pixels_cache_dirty_ = true;

    const auto quad_w = static_cast<float>(source.width_ * sx);
    const auto quad_h = static_cast<float>(source.height_ * sy);
    const auto col = gml_to_rgba(tint, opacity);

    // OpenGL FBO texture coordinate v=1.0 is top row, v=0.0 is bottom row
    batcher_.draw_quad(source.texture_id(),
                       static_cast<float>(x), static_cast<float>(y),
                       quad_w, quad_h,
                       0.0f, 1.0f, 1.0f, 0.0f,
                       col, static_cast<float>(angle));
}

void GLModernCanvas::draw_surface_part(const GLModernCanvas &source,
                                       double left,
                                       double top,
                                       double width,
                                       double height,
                                       double x,
                                       double y,
                                       double sx,
                                       double sy,
                                       std::uint32_t tint,
                                       double opacity) {
    finite(left);
    finite(top);
    finite(width);
    finite(height);
    finite(x);
    finite(y);
    finite(sx);
    finite(sy);
    finite(opacity);

    if (width <= 0.0 || height <= 0.0 || sx == 0.0 || sy == 0.0 || opacity <= 0.0 ||
        source.width_ <= 0 || source.height_ <= 0) {
        return;
    }

    const double clipped_left = std::max(0.0, left);
    const double clipped_top = std::max(0.0, top);
    const double clipped_right = std::min(static_cast<double>(source.logical_width_), left + width);
    const double clipped_bottom = std::min(static_cast<double>(source.logical_height_), top + height);
    if (clipped_right <= clipped_left || clipped_bottom <= clipped_top) {
        return;
    }

    const double draw_x = (x + (clipped_left - left) * sx) * render_scale_;
    const double draw_y = (y + (clipped_top - top) * sy) * render_scale_;
    const double draw_sx = sx * render_scale_;
    const double draw_sy = sy * render_scale_;
    const double draw_width = (clipped_right - clipped_left) * draw_sx;
    const double draw_height = (clipped_bottom - clipped_top) * draw_sy;

    const double source_scale = source.render_scale_;
    const double source_left = clipped_left * source_scale;
    const double source_top = clipped_top * source_scale;
    const double source_right = clipped_right * source_scale;
    const double source_bottom = clipped_bottom * source_scale;

    const_cast<GLModernCanvas &>(source).flush();

    ensure_render_target();
    pixels_cache_dirty_ = true;

    const double sw = source.width_;
    const double sh = source.height_;

    const float u0 = static_cast<float>(source_left / sw);
    const float u1 = static_cast<float>(source_right / sw);
    const float v0 = static_cast<float>((sh - source_top) / sh);
    const float v1 = static_cast<float>((sh - source_bottom) / sh);

    const auto col = gml_to_rgba(tint, opacity);

    batcher_.draw_quad(source.texture_id(),
                       static_cast<float>(draw_x), static_cast<float>(draw_y),
                       static_cast<float>(draw_width), static_cast<float>(draw_height),
                       u0, v0, u1, v1,
                       col, 0.0f);
}

std::uint64_t GLModernCanvas::hash() const noexcept {
    const_cast<GLModernCanvas *>(this)->flush();
    const auto px = pixels();
    std::uint64_t h = 14695981039346656037ULL;
    for (const auto val : px) {
        h ^= val;
        h *= 1099511628211ULL;
    }
    return h;
}

} // namespace regml::glmodern
