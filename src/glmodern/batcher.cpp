#include "batcher.hpp"
#include <fstream>

#include <algorithm>
#include <cmath>
#include <utility>

namespace regml::glmodern {

namespace {

constexpr GLenum gm_factor_to_gl(int factor) noexcept {
    switch (factor) {
    case 1: return GL_ZERO;
    case 2: return GL_ONE;
    case 3: return GL_SRC_COLOR;
    case 4: return GL_ONE_MINUS_SRC_COLOR;
    case 5: return GL_SRC_ALPHA;
    case 6: return GL_ONE_MINUS_SRC_ALPHA;
    case 7: return GL_DST_ALPHA;
    case 8: return GL_ONE_MINUS_DST_ALPHA;
    case 9: return GL_DST_COLOR;
    case 10: return GL_ONE_MINUS_DST_COLOR;
    case 11: return GL_SRC_ALPHA_SATURATE;
    default: return GL_ZERO;
    }
}

constexpr GLenum gm_equation_to_gl(int eq) noexcept {
    switch (eq) {
    case 1: return GL_FUNC_REVERSE_SUBTRACT;
    case 2: return GL_FUNC_SUBTRACT;
    case 3: return GL_MIN;
    case 4: return GL_MAX;
    case 0:
    default: return GL_FUNC_ADD;
    }
}

} // namespace

SpriteBatcher::SpriteBatcher() {
    init();
}

SpriteBatcher::~SpriteBatcher() {
    destroy();
}

SpriteBatcher::SpriteBatcher(SpriteBatcher &&other) noexcept
    : vao_(other.vao_),
      vbo_(other.vbo_),
      ebo_(other.ebo_),
      dummy_white_tex_(other.dummy_white_tex_),
      vertex_buffer_(other.vertex_buffer_),
      quad_count_(other.quad_count_),
      vbo_quad_offset_(other.vbo_quad_offset_),
      draw_call_count_(other.draw_call_count_),
      current_texture_(other.current_texture_),
      projection_(other.projection_),
      blend_state_(other.blend_state_),
      color_mask_(other.color_mask_),
      alpha_test_enabled_(other.alpha_test_enabled_),
      alpha_test_ref_(other.alpha_test_ref_),
      in_begin_(other.in_begin_),
      custom_shader_(other.custom_shader_),
      default_shader_(std::move(other.default_shader_)) {
    other.vao_ = 0;
    other.vbo_ = 0;
    other.ebo_ = 0;
    other.dummy_white_tex_ = 0;
    other.quad_count_ = 0;
    other.vbo_quad_offset_ = 0;
    other.draw_call_count_ = 0;
    other.current_texture_ = 0;
    other.in_begin_ = false;
    other.custom_shader_ = nullptr;
}

SpriteBatcher &SpriteBatcher::operator=(SpriteBatcher &&other) noexcept {
    if (this != &other) {
        destroy();
        vao_ = other.vao_;
        vbo_ = other.vbo_;
        ebo_ = other.ebo_;
        dummy_white_tex_ = other.dummy_white_tex_;
        vertex_buffer_ = other.vertex_buffer_;
        quad_count_ = other.quad_count_;
        draw_call_count_ = other.draw_call_count_;
        current_texture_ = other.current_texture_;
        projection_ = other.projection_;
        blend_state_ = other.blend_state_;
        color_mask_ = other.color_mask_;
        alpha_test_enabled_ = other.alpha_test_enabled_;
        alpha_test_ref_ = other.alpha_test_ref_;
        in_begin_ = other.in_begin_;
        custom_shader_ = other.custom_shader_;
        default_shader_ = std::move(other.default_shader_);

        other.vao_ = 0;
        other.vbo_ = 0;
        other.ebo_ = 0;
        other.dummy_white_tex_ = 0;
        other.quad_count_ = 0;
        other.vbo_quad_offset_ = 0;
        other.current_texture_ = 0;
        other.in_begin_ = false;
        other.custom_shader_ = nullptr;
    }
    return *this;
}

bool SpriteBatcher::init() {
    if (vao_ != 0) {
        return true;
    }

    if (!glGenVertexArrays || !glGenBuffers) {
        return false;
    }

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glGenBuffers(1, &ebo_);

    if (!vao_ || !vbo_ || !ebo_) {
        destroy();
        return false;
    }

    glBindVertexArray(vao_);

    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(VBO_CAPACITY_VERTICES * sizeof(Vertex)), nullptr, GL_DYNAMIC_DRAW);

    std::vector<std::uint16_t> indices(VBO_CAPACITY_INDICES);
    for (std::size_t i = 0; i < VBO_CAPACITY_QUADS; ++i) {
        const auto base = static_cast<std::uint16_t>(i * 4);
        indices[i * 6 + 0] = base + 0;
        indices[i * 6 + 1] = base + 1;
        indices[i * 6 + 2] = base + 2;
        indices[i * 6 + 3] = base + 2;
        indices[i * 6 + 4] = base + 3;
        indices[i * 6 + 5] = base + 0;
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices.size() * sizeof(std::uint16_t)), indices.data(), GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void *>(offsetof(Vertex, x)));

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void *>(offsetof(Vertex, u)));

    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), reinterpret_cast<const void *>(offsetof(Vertex, color)));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    if (!default_shader_) {
        default_shader_ = std::make_unique<Shader>();
        default_shader_->init_sprite_shader();
    }

    if (dummy_white_tex_ == 0 && glGenTextures && glBindTexture && glTexImage2D) {
        glGenTextures(1, &dummy_white_tex_);
        glBindTexture(GL_TEXTURE_2D, dummy_white_tex_);
        const std::uint32_t white_pixel = 0xffffffffU;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white_pixel);
        if (glTexParameteri) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    return true;
}

void SpriteBatcher::destroy() noexcept {
    if (ebo_ != 0) {
        if (glDeleteBuffers) {
            glDeleteBuffers(1, &ebo_);
        }
        ebo_ = 0;
    }
    if (vbo_ != 0) {
        if (glDeleteBuffers) {
            glDeleteBuffers(1, &vbo_);
        }
        vbo_ = 0;
    }
    if (vao_ != 0) {
        if (glDeleteVertexArrays) {
            glDeleteVertexArrays(1, &vao_);
        }
        vao_ = 0;
    }
    if (dummy_white_tex_ != 0) {
        if (glDeleteTextures) {
            glDeleteTextures(1, &dummy_white_tex_);
        }
        dummy_white_tex_ = 0;
    }
    default_shader_.reset();
    custom_shader_ = nullptr;
    quad_count_ = 0;
    vbo_quad_offset_ = 0;
    draw_call_count_ = 0;
    current_texture_ = 0;
    in_begin_ = false;
}

Shader &SpriteBatcher::active_shader() {
    if (custom_shader_) {
        return *custom_shader_;
    }
    if (!default_shader_) {
        default_shader_ = std::make_unique<Shader>();
        default_shader_->init_sprite_shader();
    }
    return *default_shader_;
}

void SpriteBatcher::begin(const Mat4 &projection) {
    if (vao_ == 0) {
        init();
    }
    if (quad_count_ > 0) {
        flush();
    }

    projection_ = projection;
    current_texture_ = 0;
    last_use_texture_ = -1;
    in_begin_ = true;
    vbo_quad_offset_ = 0;

    if (glBindVertexArray) {
        glBindVertexArray(vao_);
    }

    Shader &sh = active_shader();
    sh.bind();
    sh.set_mat4("u_projection", projection_);
    sh.set_int("u_texture", 0);
    sh.set_int("u_alpha_test", alpha_test_enabled_ ? 1 : 0);
    sh.set_float("u_alpha_ref", alpha_test_ref_);

    apply_blend_state();
    apply_color_mask();
}

void SpriteBatcher::draw_quad(std::uint32_t texture_id,
                              float x,
                              float y,
                              float w,
                              float h,
                              float u0,
                              float v0,
                              float u1,
                              float v1,
                              std::uint32_t color,
                              float angle_degrees) {
    if (angle_degrees == 0.0f) {
        const std::array<Vertex, 4> v{{
            {x, y, u0, v0, color},
            {x + w, y, u1, v0, color},
            {x + w, y + h, u1, v1, color},
            {x, y + h, u0, v1, color}
        }};
        draw_quad_corners(texture_id, v);
    } else {
        const double rad = static_cast<double>(angle_degrees) * 3.14159265358979323846 / 180.0;
        const float cs = static_cast<float>(std::cos(rad));
        const float sn = static_cast<float>(std::sin(rad));
        const float wx = w * cs;
        const float wy = -w * sn;
        const float hx = h * sn;
        const float hy = h * cs;
        const std::array<Vertex, 4> v{{
            {x, y, u0, v0, color},
            {x + wx, y + wy, u1, v0, color},
            {x + wx + hx, y + wy + hy, u1, v1, color},
            {x + hx, y + hy, u0, v1, color}
        }};
        draw_quad_corners(texture_id, v);
    }
}

void SpriteBatcher::draw_quad_corners(std::uint32_t texture_id, const std::array<Vertex, 4> &vertices) {
    if (quad_count_ > 0 && texture_id != current_texture_) {
        flush();
    }
    if (quad_count_ >= MAX_QUADS) {
        flush();
    }

    current_texture_ = texture_id;

    const auto offset = quad_count_ * 4;
    vertex_buffer_[offset + 0] = vertices[0];
    vertex_buffer_[offset + 1] = vertices[1];
    vertex_buffer_[offset + 2] = vertices[2];
    vertex_buffer_[offset + 3] = vertices[3];
    ++quad_count_;
}

void SpriteBatcher::flush() {
    if (quad_count_ == 0) {
        return;
    }

    bool wrapped = false;
    if (vbo_quad_offset_ + quad_count_ > VBO_CAPACITY_QUADS) {
        vbo_quad_offset_ = 0;
        wrapped = true;
    }

    const std::size_t vbo_byte_offset = vbo_quad_offset_ * 4 * sizeof(Vertex);
    const std::size_t vbo_byte_size = quad_count_ * 4 * sizeof(Vertex);
    const std::size_t ebo_index_offset = vbo_quad_offset_ * 6;

    Shader &sh = active_shader();

    const GLuint target_tex = current_texture_ != 0 ? current_texture_ : dummy_white_tex_;
    const int use_tex = current_texture_ != 0 ? 1 : 0;

    if (target_tex != 0) {
        if (glActiveTexture) {
            glActiveTexture(GL_TEXTURE0);
        }
        if (glBindTexture) {
            glBindTexture(GL_TEXTURE_2D, target_tex);
        }
    }
    if (use_tex != last_use_texture_) {
        sh.set_int("u_use_texture", use_tex);
        last_use_texture_ = use_tex;
    }

    if (glBindBuffer && glBufferSubData) {
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        if (wrapped && glBufferData) {
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(VBO_CAPACITY_VERTICES * sizeof(Vertex)),
                         nullptr,
                         GL_DYNAMIC_DRAW);
        }
        glBufferSubData(GL_ARRAY_BUFFER,
                        static_cast<GLintptr>(vbo_byte_offset),
                        static_cast<GLsizeiptr>(vbo_byte_size),
                        vertex_buffer_.data());
    }

    if (glDrawElements) {
        const void *indices_ptr = reinterpret_cast<const void *>(ebo_index_offset * sizeof(std::uint16_t));
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(quad_count_ * 6), GL_UNSIGNED_SHORT, indices_ptr);
        ++draw_call_count_;
    }

    vbo_quad_offset_ += quad_count_;
    quad_count_ = 0;
}

void SpriteBatcher::end() {
    flush();
    if (glBindVertexArray) {
        glBindVertexArray(0);
    }
    in_begin_ = false;
}

void SpriteBatcher::set_blend_enable(bool enabled) {
    if (blend_state_.enabled != enabled) {
        flush();
        blend_state_.enabled = enabled;
        apply_blend_state();
    }
}

void SpriteBatcher::set_blend_mode(int src_rgb, int dst_rgb, int src_a, int dst_a, int eq_rgb, int eq_a) {
    BlendState next{blend_state_.enabled, src_rgb, dst_rgb, src_a, dst_a, eq_rgb, eq_a};
    if (next != blend_state_) {
        flush();
        blend_state_ = next;
        apply_blend_state();
    }
}

void SpriteBatcher::apply_blend_state() {
    if (!glEnable || !glDisable) {
        return;
    }

    if (blend_state_.enabled) {
        glEnable(GL_BLEND);
        if (glBlendFuncSeparate) {
            glBlendFuncSeparate(gm_factor_to_gl(blend_state_.src_rgb),
                                gm_factor_to_gl(blend_state_.dst_rgb),
                                gm_factor_to_gl(blend_state_.src_a),
                                gm_factor_to_gl(blend_state_.dst_a));
        } else if (glBlendFunc) {
            glBlendFunc(gm_factor_to_gl(blend_state_.src_rgb), gm_factor_to_gl(blend_state_.dst_rgb));
        }

        if (glBlendEquationSeparate) {
            glBlendEquationSeparate(gm_equation_to_gl(blend_state_.eq_rgb),
                                    gm_equation_to_gl(blend_state_.eq_a));
        } else if (glBlendEquation) {
            glBlendEquation(gm_equation_to_gl(blend_state_.eq_rgb));
        }
    } else {
        glDisable(GL_BLEND);
    }
}

void SpriteBatcher::set_alpha_test(bool enabled, float ref) {
    if (alpha_test_enabled_ != enabled || alpha_test_ref_ != ref) {
        flush();
        alpha_test_enabled_ = enabled;
        alpha_test_ref_ = ref;
        Shader &sh = active_shader();
        sh.set_int("u_alpha_test", enabled ? 1 : 0);
        sh.set_float("u_alpha_ref", ref);
    }
}

void SpriteBatcher::set_color_mask(bool r, bool g, bool b, bool a) {
    ColorMask next{r, g, b, a};
    if (next != color_mask_) {
        flush();
        color_mask_ = next;
        apply_color_mask();
    }
}

void SpriteBatcher::apply_color_mask() {
    if (glColorMask) {
        glColorMask(color_mask_.r ? GL_TRUE : GL_FALSE,
                    color_mask_.g ? GL_TRUE : GL_FALSE,
                    color_mask_.b ? GL_TRUE : GL_FALSE,
                    color_mask_.a ? GL_TRUE : GL_FALSE);
    }
}

} // namespace regml::glmodern
