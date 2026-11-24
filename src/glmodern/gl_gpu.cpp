#include "gl_gpu.hpp"
#include "batcher.hpp"
#include "fbo.hpp"
#include "gl_core.hpp"
#include "texture.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <memory>
#include <unordered_map>
#include <vector>

namespace regml::glmodern {
namespace {

struct Box {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    std::uint32_t color = 0xffffffffU;
};

SpriteBatcher s_batcher;
TextureCache s_texture_cache;

std::size_t s_current_frame = 0;
std::size_t s_flush_draw_calls = 0;
std::size_t s_frame_draw_calls = 0;
std::uint32_t s_last_texture_id = 0;
std::vector<std::uint32_t> s_textures_used;

bool s_initialized = false;
bool s_dirty = false;
bool s_presented = false;

int s_viewport_width = 640;
int s_viewport_height = 480;
GLint s_main_vp[4] = {0, 0, 0, 0};
bool s_main_vp_saved = false;
std::unordered_map<std::uint32_t, std::unique_ptr<GLFramebuffer>> s_surfaces;
std::uint32_t s_next_surface_id = 1;
std::uint32_t s_bound_surface_id = 0;

bool s_blend_enabled = true;
int s_blend_src_rgb = 5;
int s_blend_dst_rgb = 6;
int s_blend_src_a = 5;
int s_blend_dst_a = 6;
int s_blend_eq_rgb = 0;
int s_blend_eq_a = 0;

bool s_alpha_test_enabled = false;
float s_alpha_test_ref = 0.0f;

bool s_color_mask_r = true;
bool s_color_mask_g = true;
bool s_color_mask_b = true;
bool s_color_mask_a = true;

std::array<Box, 4096> s_box_batch{};
std::size_t s_box_batch_size = 0;
bool s_box_batch_active = false;

constexpr int CIRCLE_SEGMENTS = 96;

const auto &unit_circle() {
    static const auto unit = [] {
        std::array<std::array<float, 2>, CIRCLE_SEGMENTS + 1> points{};
        for (int i = 0; i < CIRCLE_SEGMENTS; ++i) {
            const double a = i * 6.28318530717958647692 / CIRCLE_SEGMENTS;
            points[i] = {static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))};
        }
        points[CIRCLE_SEGMENTS] = points[0];
        return points;
    }();
    return unit;
}

[[nodiscard]] constexpr std::uint32_t unpack_rgba(std::uint32_t rgba) noexcept {
    const auto r = static_cast<std::uint8_t>((rgba >> 16) & 0xffU);
    const auto g = static_cast<std::uint8_t>((rgba >> 8) & 0xffU);
    const auto b = static_cast<std::uint8_t>(rgba & 0xffU);
    const auto a = static_cast<std::uint8_t>((rgba >> 24) & 0xffU);
    return make_rgba(r, g, b, a);
}

[[nodiscard]] constexpr std::uint32_t tint_opacity_to_rgba(unsigned tint, double opacity) noexcept {
    const auto a = static_cast<std::uint8_t>(std::clamp(opacity, 0.0, 1.0) * 255.0 + 0.5);
    const auto r = static_cast<std::uint8_t>((tint >> 16) & 0xffU);
    const auto g = static_cast<std::uint8_t>((tint >> 8) & 0xffU);
    const auto b = static_cast<std::uint8_t>(tint & 0xffU);
    return make_rgba(r, g, b, a);
}

bool ensure_initialized() {
    if (s_initialized) return true;
    if (!is_gl_core_loaded() && !load_gl_core_3_3()) {
        return false;
    }
    s_batcher.init();
    s_initialized = true;
    return true;
}

bool is_cached_recursive(int width, int height, std::uint64_t key) {
    if (width <= 1024 && height <= 1024) {
        return s_texture_cache.contains(key);
    }
    for (int y = 0; y < height; y += 1024) {
        for (int x = 0; x < width; x += 1024) {
            const int w = std::min(1024, width - x);
            const int h = std::min(1024, height - y);
            if (!is_cached_recursive(w, h, hash_combine_key(key, x, y))) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

void gl_gpu_set_viewport(int width, int height) {
    if (width > 0 && height > 0) {
        s_viewport_width = width;
        s_viewport_height = height;
    }
}

void gl_gpu_begin() {
    ensure_initialized();
    s_batcher.reset_draw_call_count();
    s_textures_used.clear();
    s_flush_draw_calls = 0;

    if (s_viewport_width <= 0 || s_viewport_height <= 0) {
        GLint vp[4]{};
        if (glGetIntegerv) {
            glGetIntegerv(GL_VIEWPORT, vp);
            if (vp[2] > 0 && vp[3] > 0) {
                s_viewport_width = vp[2];
                s_viewport_height = vp[3];
            }
        }
    }

    const auto proj = Mat4::ortho_2d(static_cast<float>(s_viewport_width > 0 ? s_viewport_width : 640),
                                     static_cast<float>(s_viewport_height > 0 ? s_viewport_height : 480));
    s_batcher.begin(proj);
    s_batcher.set_blend_enable(s_blend_enabled);
    s_batcher.set_blend_mode(s_blend_src_rgb, s_blend_dst_rgb, s_blend_src_a, s_blend_dst_a, s_blend_eq_rgb, s_blend_eq_a);
    s_batcher.set_alpha_test(s_alpha_test_enabled, s_alpha_test_ref);
    s_batcher.set_color_mask(s_color_mask_r, s_color_mask_g, s_color_mask_b, s_color_mask_a);

    s_presented = false;
    s_dirty = false;
}

void gl_gpu_set_blend_enable(bool enabled) {
    s_blend_enabled = enabled;
    if (s_batcher.is_active()) {
        s_batcher.set_blend_enable(enabled);
    }
}

void gl_gpu_set_blend_mode(int src_rgb, int dst_rgb, int src_a, int dst_a, int eq_rgb, int eq_a) {
    s_blend_src_rgb = src_rgb;
    s_blend_dst_rgb = dst_rgb;
    s_blend_src_a = src_a;
    s_blend_dst_a = dst_a;
    s_blend_eq_rgb = eq_rgb;
    s_blend_eq_a = eq_a;
    if (s_batcher.is_active()) {
        s_batcher.set_blend_mode(src_rgb, dst_rgb, src_a, dst_a, eq_rgb, eq_a);
    }
}

void gl_gpu_set_alpha_test(bool enabled, float ref) {
    s_alpha_test_enabled = enabled;
    s_alpha_test_ref = ref;
    if (s_batcher.is_active()) {
        s_batcher.set_alpha_test(enabled, ref);
    }
}

void gl_gpu_set_color_mask(bool r, bool g, bool b, bool a) {
    s_color_mask_r = r;
    s_color_mask_g = g;
    s_color_mask_b = b;
    s_color_mask_a = a;
    if (s_batcher.is_active()) {
        s_batcher.set_color_mask(r, g, b, a);
    }
}

void gl_gpu_flush() {
    const std::size_t before = s_batcher.draw_call_count();
    if (s_batcher.is_active()) {
        s_batcher.flush();
    }
    s_dirty = false;
    s_flush_draw_calls = s_batcher.draw_call_count() - before;
    s_frame_draw_calls = s_batcher.draw_call_count();
    s_last_texture_id = s_batcher.current_texture();
}

void gl_gpu_presented() {
    s_presented = true;
    s_dirty = false;
}

void gl_gpu_box(double x, double y, double w, double h, std::uint32_t rgba) {
    if (w <= 0.0 || h <= 0.0) return;

    if (s_blend_enabled && (rgba >> 24) == 0 &&
        s_blend_src_rgb == 5 && s_blend_dst_rgb == 6 &&
        s_blend_src_a == 5 && s_blend_dst_a == 6) {
        return;
    }

    const std::uint32_t c = unpack_rgba(rgba);

    if (s_box_batch_active) {
        if (s_box_batch_size < s_box_batch.size()) {
            s_box_batch[s_box_batch_size++] = {
                static_cast<float>(x), static_cast<float>(y),
                static_cast<float>(w), static_cast<float>(h), c
            };
        }
        return;
    }

    s_batcher.draw_quad(0, static_cast<float>(x), static_cast<float>(y),
                        static_cast<float>(w), static_cast<float>(h),
                        0.0f, 0.0f, 0.0f, 0.0f, c, 0.0f);
    s_dirty = true;
}

void gl_gpu_box_batch_begin() {
    s_box_batch_size = 0;
    s_box_batch_active = true;
}

void gl_gpu_box_batch_end() {
    s_box_batch_active = false;
    if (s_box_batch_size == 0) return;

    for (std::size_t i = 0; i < s_box_batch_size; ++i) {
        const auto &b = s_box_batch[i];
        s_batcher.draw_quad(0, b.x, b.y, b.width, b.height,
                            0.0f, 0.0f, 0.0f, 0.0f, b.color, 0.0f);
    }
    s_box_batch_size = 0;
    s_dirty = true;
}

void gl_gpu_triangle(double x1, double y1, double x2, double y2, double x3, double y3, std::uint32_t rgba) {
    if (s_blend_enabled && (rgba >> 24) == 0 &&
        s_blend_src_rgb == 5 && s_blend_dst_rgb == 6 &&
        s_blend_src_a == 5 && s_blend_dst_a == 6) {
        return;
    }

    const std::uint32_t c = unpack_rgba(rgba);
    const Vertex v1{static_cast<float>(x1), static_cast<float>(y1), 0.0f, 0.0f, c};
    const Vertex v2{static_cast<float>(x2), static_cast<float>(y2), 0.0f, 0.0f, c};
    const Vertex v3{static_cast<float>(x3), static_cast<float>(y3), 0.0f, 0.0f, c};

    s_batcher.draw_quad_corners(0, {v1, v2, v3, v1});
    s_dirty = true;
}

void gl_gpu_circle(double x, double y, double r, bool outline, std::uint32_t rgba) {
    if (r <= 0.0) return;
    if (s_blend_enabled && (rgba >> 24) == 0 &&
        s_blend_src_rgb == 5 && s_blend_dst_rgb == 6 &&
        s_blend_src_a == 5 && s_blend_dst_a == 6) {
        return;
    }

    const std::uint32_t c = unpack_rgba(rgba);
    const auto &unit = unit_circle();

    if (outline) {
        const double inner = std::max(0.0, r - 1.0);
        for (int i = 0; i < CIRCLE_SEGMENTS; ++i) {
            const Vertex p0{static_cast<float>(x + r * unit[i][0]), static_cast<float>(y + r * unit[i][1]), 0.0f, 0.0f, c};
            const Vertex p1{static_cast<float>(x + r * unit[i + 1][0]), static_cast<float>(y + r * unit[i + 1][1]), 0.0f, 0.0f, c};
            const Vertex p2{static_cast<float>(x + inner * unit[i + 1][0]), static_cast<float>(y + inner * unit[i + 1][1]), 0.0f, 0.0f, c};
            const Vertex p3{static_cast<float>(x + inner * unit[i][0]), static_cast<float>(y + inner * unit[i][1]), 0.0f, 0.0f, c};
            s_batcher.draw_quad_corners(0, {p0, p1, p2, p3});
        }
    } else {
        const Vertex center{static_cast<float>(x), static_cast<float>(y), 0.0f, 0.0f, c};
        for (int i = 0; i < CIRCLE_SEGMENTS; ++i) {
            const Vertex p0{static_cast<float>(x + r * unit[i][0]), static_cast<float>(y + r * unit[i][1]), 0.0f, 0.0f, c};
            const Vertex p1{static_cast<float>(x + r * unit[i + 1][0]), static_cast<float>(y + r * unit[i + 1][1]), 0.0f, 0.0f, c};
            s_batcher.draw_quad_corners(0, {center, p0, p1, center});
        }
    }
    s_dirty = true;
}

bool gl_gpu_cached_image(int width,
                        int height,
                        double x,
                        double y,
                        double dw,
                        double dh,
                        double angle,
                        unsigned tint,
                        double opacity,
                        std::uint64_t key) {
    if (!key) return false;

    if (width <= 1024 && height <= 1024) {
        const GLuint tex_id = s_texture_cache.get(key);
        if (tex_id == 0) return false;

        const std::uint32_t c = tint_opacity_to_rgba(tint, opacity);
        s_batcher.draw_quad(tex_id, static_cast<float>(x), static_cast<float>(y),
                            static_cast<float>(dw), static_cast<float>(dh),
                            0.0f, 0.0f, 1.0f, 1.0f, c, static_cast<float>(angle));
        s_dirty = true;
        return true;
    }

    if (!is_cached_recursive(width, height, key)) {
        return false;
    }

    gl_gpu_image({}, 0, 0, 0, width, height, x, y, dw, dh, angle, tint, opacity, key);
    return true;
}

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
                 std::uint64_t key) {
    if (width <= 0 || height <= 0 || opacity <= 0.0) return;

    if (width > 1024 || height > 1024) {
        const double rad = angle * 3.14159265358979323846 / 180.0;
        const double cs = angle == 0.0 ? 1.0 : std::cos(rad);
        const double sn = angle == 0.0 ? 0.0 : std::sin(rad);

        for (int yy = 0; yy < height; yy += 1024) {
            for (int xx = 0; xx < width; xx += 1024) {
                const int w = std::min(1024, width - xx);
                const int h = std::min(1024, height - yy);
                const double u = dw * xx / width;
                const double v = dh * yy / height;
                const auto child = hash_combine_key(key, xx, yy);

                gl_gpu_image(pixels, stride, left + xx, top + yy, w, h,
                            x + u * cs + v * sn, y - u * sn + v * cs,
                            dw * w / width, dh * h / height, angle, tint, opacity, child);
            }
        }
        return;
    }

     auto extract_rgba = [&]() -> std::span<const std::uint32_t> {
        static std::vector<std::uint32_t> s_rgba_scratch;
        s_rgba_scratch.resize(static_cast<std::size_t>(width) * height);
        for (int r = 0; r < height; ++r) {
            const auto *src_row = pixels.data() + static_cast<std::size_t>(top + r) * stride + left;
            auto *dst_row = s_rgba_scratch.data() + static_cast<std::size_t>(r) * width;
            for (int col = 0; col < width; ++col) {
                const std::uint32_t p = src_row[col];
                const auto a = static_cast<std::uint8_t>((p >> 24) & 0xffU);
                const auto red = static_cast<std::uint8_t>((p >> 16) & 0xffU);
                const auto green = static_cast<std::uint8_t>((p >> 8) & 0xffU);
                const auto blue = static_cast<std::uint8_t>(p & 0xffU);
                dst_row[col] = make_rgba(red, green, blue, a);
            }
        }
        return s_rgba_scratch;
    };

    if (dw == 0.0 && dh == 0.0) {
        if (key != 0 && s_texture_cache.get(key) == 0 && !pixels.empty()) {
            const auto converted = extract_rgba();
            s_texture_cache.get_or_upload(key, converted, width, height, false);
        }
        return;
    }

    if (angle == 0.0) {
        const double min_x = dw >= 0.0 ? x : x + dw;
        const double max_x = dw >= 0.0 ? x + dw : x;
        const double min_y = dh >= 0.0 ? y : y + dh;
        const double max_y = dh >= 0.0 ? y + dh : y;
        if (max_x <= 0.0 || min_x >= s_viewport_width || max_y <= 0.0 || min_y >= s_viewport_height) {
            return;
        }
    } else {
        const double rad = angle * 3.14159265358979323846 / 180.0;
        const double cs = std::cos(rad);
        const double sn = std::sin(rad);
        const double x0 = x;
        const double y0 = y;
        const double x1 = x + dw * cs;
        const double y1 = y - dw * sn;
        const double x2 = x + dh * sn;
        const double y2 = y + dh * cs;
        const double x3 = x + dw * cs + dh * sn;
        const double y3 = y - dw * sn + dh * cs;
        const double min_x = std::min({x0, x1, x2, x3});
        const double max_x = std::max({x0, x1, x2, x3});
        const double min_y = std::min({y0, y1, y2, y3});
        const double max_y = std::max({y0, y1, y2, y3});
        if (max_x <= 0.0 || min_x >= s_viewport_width || max_y <= 0.0 || min_y >= s_viewport_height) {
            return;
        }
    }

    auto record_texture_used = [](std::uint32_t tex_id) {
        if (s_last_texture_id != tex_id || s_textures_used.empty()) {
            s_last_texture_id = tex_id;
            if (std::find(s_textures_used.begin(), s_textures_used.end(), tex_id) == s_textures_used.end()) {
                s_textures_used.push_back(tex_id);
            }
        }
    };

    const std::uint32_t c = tint_opacity_to_rgba(tint, opacity);

    if (key != 0) {
        GLuint tex_id = s_texture_cache.get(key);
        if (tex_id == 0 && !pixels.empty()) {
            const auto converted = extract_rgba();
            tex_id = s_texture_cache.get_or_upload(key, converted, width, height, false);
        }
        if (tex_id != 0) {
            record_texture_used(tex_id);
            s_batcher.draw_quad(tex_id, static_cast<float>(x), static_cast<float>(y),
                                static_cast<float>(dw), static_cast<float>(dh),
                                0.0f, 0.0f, 1.0f, 1.0f, c, static_cast<float>(angle));
            s_dirty = true;
        }
    } else if (!pixels.empty()) {
        const auto converted = extract_rgba();
        static GLTexture s_transient_tex;
        if (s_batcher.current_texture() == s_transient_tex.id() && s_batcher.quad_count() > 0) {
            s_batcher.flush();
        }
        if (s_transient_tex.valid() && s_transient_tex.width() == width && s_transient_tex.height() == height) {
            s_transient_tex.upload_sub(0, 0, width, height, converted);
        } else {
            s_transient_tex.destroy();
            s_transient_tex.upload(width, height, converted, false);
        }
        if (s_transient_tex.id() != 0) {
            record_texture_used(s_transient_tex.id());
            s_batcher.draw_quad(s_transient_tex.id(), static_cast<float>(x), static_cast<float>(y),
                                static_cast<float>(dw), static_cast<float>(dh),
                                0.0f, 0.0f, 1.0f, 1.0f, c, static_cast<float>(angle));
            s_dirty = true;
        }
    }
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
                             std::uint64_t key) {
    if (!key || opacity <= 0.0 || dw == 0.0 || dh == 0.0) return false;

    const GLuint tex_id = s_texture_cache.get(key);
    if (tex_id == 0) return false;

    const double min_x = dw >= 0.0 ? x : x + dw;
    const double max_x = dw >= 0.0 ? x + dw : x;
    const double min_y = dh >= 0.0 ? y : y + dh;
    const double max_y = dh >= 0.0 ? y + dh : y;
    if (max_x <= 0.0 || min_x >= s_viewport_width || max_y <= 0.0 || min_y >= s_viewport_height) {
        return true;
    }

    const std::uint32_t c = tint_opacity_to_rgba(tint, opacity);
    if (s_last_texture_id != tex_id || s_textures_used.empty()) {
        s_last_texture_id = tex_id;
        if (std::find(s_textures_used.begin(), s_textures_used.end(), tex_id) == s_textures_used.end()) {
            s_textures_used.push_back(tex_id);
        }
    }
    s_batcher.draw_quad(tex_id, static_cast<float>(x), static_cast<float>(y),
                        static_cast<float>(dw), static_cast<float>(dh),
                        u0, v0, u1, v1, c, 0.0f);
    s_dirty = true;
    return true;
}

std::uint32_t gl_gpu_pixel(int x, int y) {
    if (s_dirty) {
        s_batcher.flush();
        s_dirty = false;
    }

    GLint vp[4]{};
    if (glGetIntegerv) {
        glGetIntegerv(GL_VIEWPORT, vp);
    }
    const int vp_h = vp[3] > 0 ? vp[3] : s_viewport_height;
    const int gl_y = vp_h - 1 - y;

    std::uint8_t rgba[4]{0, 0, 0, 255};
    if (glReadPixels) {
        glReadPixels(x, gl_y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }

    return 0xff000000U | (static_cast<std::uint32_t>(rgba[0]) << 16) |
           (static_cast<std::uint32_t>(rgba[1]) << 8) | rgba[2];
}

bool gl_gpu_readback(int width, int height, std::span<std::uint32_t> pixels) noexcept {
    if (width <= 0 || height <= 0 ||
        pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {
        return false;
    }

    if (s_dirty) {
        s_batcher.flush();
        s_dirty = false;
    }

    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(width) * height * 4);
    if (!glReadPixels) return false;

    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, buffer.data());

    for (int y = 0; y < height; ++y) {
        const int src_y = height - 1 - y;
        for (int x = 0; x < width; ++x) {
            const auto *p = &buffer[(static_cast<std::size_t>(src_y) * width + x) * 4];
            pixels[static_cast<std::size_t>(y) * width + x] =
                0xff000000U | (static_cast<std::uint32_t>(p[0]) << 16) |
                (static_cast<std::uint32_t>(p[1]) << 8) | p[2];
        }
    }
    return true;
}

void gl_gpu_set_frame_number(std::size_t frame) {
    s_current_frame = frame;
}

std::size_t gl_gpu_flush_draw_calls() {
    return s_flush_draw_calls;
}

std::size_t gl_gpu_frame_draw_calls() {
    return s_frame_draw_calls;
}

std::uint32_t gl_gpu_last_texture_id() {
    return s_last_texture_id;
}

std::vector<std::uint32_t> gl_gpu_textures_used() {
    return s_textures_used;
}

std::uint32_t gl_gpu_surface_create(int width, int height) {
    ensure_initialized();
    if (width <= 0 || height <= 0) {
        return 0;
    }
    auto fbo = std::make_unique<GLFramebuffer>();
    if (!fbo->init(width, height, false)) {
        return 0;
    }
    const std::uint32_t id = s_next_surface_id++;
    s_surfaces.emplace(id, std::move(fbo));
    return id;
}

void gl_gpu_surface_destroy(std::uint32_t surface_id) {
    if (surface_id == 0) {
        return;
    }
    if (s_bound_surface_id == surface_id) {
        gl_gpu_surface_bind(0, s_viewport_width, s_viewport_height);
    }
    s_surfaces.erase(surface_id);
}

void gl_gpu_surface_bind(std::uint32_t surface_id, int width, int height) {
    ensure_initialized();
    if (surface_id != 0 && s_surfaces.find(surface_id) == s_surfaces.end()) {
        surface_id = 0;
    }
    if (s_bound_surface_id == 0 && surface_id != 0 && !s_main_vp_saved && glGetIntegerv) {
        glGetIntegerv(GL_VIEWPORT, s_main_vp);
        if (s_main_vp[2] > 0 && s_main_vp[3] > 0) {
            s_main_vp_saved = true;
        }
    }
    int target_w = width > 0 ? width : 640;
    int target_h = height > 0 ? height : 480;
    if (surface_id != 0) {
        const auto &fbo = s_surfaces.at(surface_id);
        target_w = fbo->width();
        target_h = fbo->height();
    }
    if (s_bound_surface_id == surface_id && s_viewport_width == target_w && s_viewport_height == target_h) {
        return;
    }
    if (s_batcher.is_active()) {
        s_batcher.end();
    }
    if (surface_id == 0) {
        GLFramebuffer::unbind();
        if (s_main_vp_saved && glViewport) {
            glViewport(s_main_vp[0], s_main_vp[1], s_main_vp[2], s_main_vp[3]);
        }
    } else {
        s_surfaces.at(surface_id)->bind(true);
    }
    s_bound_surface_id = surface_id;
    s_viewport_width = target_w;
    s_viewport_height = target_h;
    s_batcher.begin(Mat4::ortho_2d(static_cast<float>(target_w), static_cast<float>(target_h)));
    s_batcher.set_blend_enable(s_blend_enabled);
    s_batcher.set_blend_mode(s_blend_src_rgb, s_blend_dst_rgb, s_blend_src_a, s_blend_dst_a, s_blend_eq_rgb, s_blend_eq_a);
    s_batcher.set_alpha_test(s_alpha_test_enabled, s_alpha_test_ref);
    s_batcher.set_color_mask(s_color_mask_r, s_color_mask_g, s_color_mask_b, s_color_mask_a);
    s_dirty = false;
}

void gl_gpu_surface_clear(std::uint32_t argb) {
    ensure_initialized();
    if (s_batcher.is_active()) {
        s_batcher.flush();
    }
    if (glClearColor) {
        const float r = static_cast<float>((argb >> 16) & 0xffU) / 255.0f;
        const float g = static_cast<float>((argb >> 8) & 0xffU) / 255.0f;
        const float b = static_cast<float>(argb & 0xffU) / 255.0f;
        const float a = static_cast<float>((argb >> 24) & 0xffU) / 255.0f;
        glClearColor(r, g, b, a);
    }
    if (glClear) {
        glClear(GL_COLOR_BUFFER_BIT);
    }
    s_dirty = false;
}

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
                         double opacity) {
    if (surface_id == 0 || opacity <= 0.0 || width <= 0.0 || height <= 0.0 || dw == 0.0 || dh == 0.0) {
        return false;
    }
    const auto it = s_surfaces.find(surface_id);
    if (it == s_surfaces.end() || !it->second->valid()) {
        return false;
    }
    const GLuint tex_id = it->second->texture_id();
    const float fw = static_cast<float>(it->second->width());
    const float fh = static_cast<float>(it->second->height());
    const float u0 = static_cast<float>(left) / fw;
    const float u1 = static_cast<float>(left + width) / fw;
    const float v0 = 1.0f - static_cast<float>(top) / fh;
    const float v1 = 1.0f - static_cast<float>(top + height) / fh;
    const std::uint32_t c = tint_opacity_to_rgba(tint, opacity);
    if (s_last_texture_id != tex_id || s_textures_used.empty()) {
        s_last_texture_id = tex_id;
        if (std::find(s_textures_used.begin(), s_textures_used.end(), tex_id) == s_textures_used.end()) {
            s_textures_used.push_back(tex_id);
        }
    }
    s_batcher.draw_quad(tex_id, static_cast<float>(x), static_cast<float>(y),
                        static_cast<float>(dw), static_cast<float>(dh),
                        u0, v0, u1, v1, c, static_cast<float>(angle));
    s_dirty = true;
    return true;
}

} // namespace regml::glmodern
