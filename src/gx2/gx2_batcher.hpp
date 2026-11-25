#pragma once

#include <gx2/enum.h>
#include <gx2/sampler.h>
#include <gx2/texture.h>
#include <whb/gfx.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace regml::gx2 {

struct Gx2Vertex {
    float x = 0.0f;
    float y = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    std::uint32_t rgba = 0xffffffffU;
};

struct Gx2Mat4 {
    std::array<float, 16> m{};

    static Gx2Mat4 ortho(float left, float right, float bottom, float top, float near_z = -1.0f, float far_z = 1.0f) noexcept;
};

class Gx2Batcher {
public:
    static constexpr std::size_t MAX_BATCH_VERTICES = 2048 * 6;
    static constexpr std::size_t RING_CAPACITY_VERTICES = 8192 * 6;

    Gx2Batcher() = default;
    ~Gx2Batcher();

    Gx2Batcher(const Gx2Batcher &) = delete;
    Gx2Batcher &operator=(const Gx2Batcher &) = delete;

    bool init();
    void destroy() noexcept;

    void begin(const Gx2Mat4 &projection);
    void draw_quad(const GX2Texture *texture,
                   float x,
                   float y,
                   float w,
                   float h,
                   float u0,
                   float v0,
                   float u1,
                   float v1,
                   std::uint32_t rgba,
                   float angle_degrees = 0.0f);
    void draw_triangle(float x1,
                       float y1,
                       float x2,
                       float y2,
                       float x3,
                       float y3,
                       std::uint32_t rgba);
    void flush();
    void end();
    void set_projection(const Gx2Mat4 &projection);

    void set_blend_enable(bool enabled);
    void set_blend_mode(int src_rgb, int dst_rgb, int src_a, int dst_a, int eq_rgb = 0, int eq_a = 0);
    void set_alpha_test(bool enabled, float ref = 0.0f);
    void set_color_mask(bool r, bool g, bool b, bool a);

    [[nodiscard]] bool is_active() const noexcept { return in_begin_; }
    [[nodiscard]] bool blend_enabled() const noexcept { return blend_state_.enabled; }
    [[nodiscard]] bool alpha_test_enabled() const noexcept { return alpha_test_enabled_; }
    [[nodiscard]] float alpha_test_ref() const noexcept { return alpha_test_ref_; }
    [[nodiscard]] std::size_t draw_call_count() const noexcept { return draw_call_count_; }
    void reset_draw_call_count() noexcept { draw_call_count_ = 0; }

private:
    struct BlendState {
        bool enabled = true;
        int src_rgb = 5;
        int dst_rgb = 6;
        int src_a = 5;
        int dst_a = 6;
        int eq_rgb = 0;
        int eq_a = 0;

        bool operator==(const BlendState &) const = default;
    };

    struct ColorMask {
        bool r = true;
        bool g = true;
        bool b = true;
        bool a = true;

        bool operator==(const ColorMask &) const = default;
    };

    void ensure_space(std::size_t vertices_needed, const GX2Texture *texture);
    void apply_blend_state();
    void apply_color_mask();
    void apply_alpha_test();
    std::uint8_t *alloc_ubo_slot(std::size_t size) noexcept;
    void upload_vertex_projection();

    static constexpr std::size_t UBO_SLOT_SIZE = 512;
    static constexpr std::size_t UBO_SLOT_COUNT = 2048;

    WHBGfxShaderGroup shader_group_{};
    bool shader_loaded_ = false;

    GX2Sampler sampler_{};
    GX2Sampler linear_sampler_{};
    GX2Texture dummy_white_tex_{};
    std::uint8_t *dummy_white_pixels_ = nullptr;

    Gx2Vertex *ring_buffer_ = nullptr;
    std::size_t ring_offset_ = 0;
    std::uint8_t *ubo_ring_ = nullptr;
    std::size_t ubo_offset_ = 0;
    std::size_t batch_count_ = 0;
    std::array<Gx2Vertex, MAX_BATCH_VERTICES> staging_{};

    const GX2Texture *current_texture_ = nullptr;
    const GX2Texture *last_bound_texture_ = nullptr;
    int last_use_texture_ = -1;
    int last_ps_use_tex_ = -1;
    bool last_ps_alpha_test_ = false;
    float last_ps_alpha_ref_ = -1.0f;

    Gx2Mat4 projection_{};
    BlendState blend_state_{};
    BlendState applied_blend_{};
    bool has_applied_blend_ = false;
    bool blend_dirty_ = true;

    ColorMask color_mask_{};
    ColorMask applied_color_mask_{};
    bool has_applied_color_mask_ = false;
    bool color_mask_dirty_ = true;

    bool alpha_test_enabled_ = false;
    float alpha_test_ref_ = 0.0f;
    bool alpha_test_dirty_ = true;

    bool in_begin_ = false;
    std::size_t draw_call_count_ = 0;
};

} // namespace regml::gx2
