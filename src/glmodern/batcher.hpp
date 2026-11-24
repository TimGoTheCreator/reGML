#pragma once

#include "gl_common.hpp"
#include "gl_core.hpp"
#include "shader.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace regml::glmodern {

class SpriteBatcher {
public:
    static constexpr std::size_t MAX_QUADS = 2048;
    static constexpr std::size_t MAX_VERTICES = MAX_QUADS * 4;
    static constexpr std::size_t MAX_INDICES = MAX_QUADS * 6;

    // 4x batch capacity to stream VBO slices without stalling on in-flight draws
    static constexpr std::size_t VBO_CAPACITY_QUADS = 8192;
    static constexpr std::size_t VBO_CAPACITY_VERTICES = VBO_CAPACITY_QUADS * 4;
    static constexpr std::size_t VBO_CAPACITY_INDICES = VBO_CAPACITY_QUADS * 6;

    SpriteBatcher();
    ~SpriteBatcher();

    SpriteBatcher(const SpriteBatcher &) = delete;
    SpriteBatcher &operator=(const SpriteBatcher &) = delete;

    SpriteBatcher(SpriteBatcher &&other) noexcept;
    SpriteBatcher &operator=(SpriteBatcher &&other) noexcept;

    bool init();
    void destroy() noexcept;

    void begin(const Mat4 &projection);
    void draw_quad(std::uint32_t texture_id,
                   float x,
                   float y,
                   float w,
                   float h,
                   float u0,
                   float v0,
                   float u1,
                   float v1,
                   std::uint32_t color,
                   float angle_degrees = 0.0f);
    void draw_quad_corners(std::uint32_t texture_id, const std::array<Vertex, 4> &vertices);
    void flush();
    void end();

    void set_blend_enable(bool enabled);
    void set_blend_mode(int src_rgb, int dst_rgb, int src_a, int dst_a, int eq_rgb = 0, int eq_a = 0);
    void set_alpha_test(bool enabled, float ref = 0.0f);
    void set_color_mask(bool r, bool g, bool b, bool a);

    void set_shader(Shader *shader) noexcept { custom_shader_ = shader; }
    [[nodiscard]] Shader *shader() const noexcept { return custom_shader_ ? custom_shader_ : default_shader_.get(); }

    [[nodiscard]] std::size_t quad_count() const noexcept { return quad_count_; }
    [[nodiscard]] bool is_active() const noexcept { return in_begin_; }
    [[nodiscard]] std::size_t draw_call_count() const noexcept { return draw_call_count_; }
    void reset_draw_call_count() noexcept { draw_call_count_ = 0; }
    [[nodiscard]] std::uint32_t current_texture() const noexcept { return current_texture_; }

private:
    struct BlendState {
        bool enabled = true;
        int src_rgb = 5;  // bm_src_alpha
        int dst_rgb = 6;  // bm_inv_src_alpha
        int src_a = 5;
        int dst_a = 6;
        int eq_rgb = 0;   // add
        int eq_a = 0;     // add

        bool operator==(const BlendState &) const = default;
    };

    struct ColorMask {
        bool r = true;
        bool g = true;
        bool b = true;
        bool a = true;

        bool operator==(const ColorMask &) const = default;
    };

    void apply_blend_state();
    void apply_color_mask();
    Shader &active_shader();

    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint ebo_ = 0;
    GLuint dummy_white_tex_ = 0;

    std::array<Vertex, MAX_VERTICES> vertex_buffer_{};
    std::size_t quad_count_ = 0;
    std::size_t vbo_quad_offset_ = 0;
    std::size_t draw_call_count_ = 0;
    std::uint32_t current_texture_ = 0;
    int last_use_texture_ = -1;

    Mat4 projection_{};
    BlendState blend_state_{};
    ColorMask color_mask_{};
    bool alpha_test_enabled_ = false;
    float alpha_test_ref_ = 0.0f;
    bool in_begin_ = false;

    Shader *custom_shader_ = nullptr;
    std::unique_ptr<Shader> default_shader_;
};

} // namespace regml::glmodern
