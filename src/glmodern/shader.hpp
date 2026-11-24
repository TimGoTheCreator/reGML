#pragma once

#include "gl_common.hpp"
#include "gl_core.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace regml::glmodern {

inline constexpr std::string_view SPRITE_VERTEX_SHADER =
    "#version 330 core\n"
    "layout (location = 0) in vec2 in_pos;\n"
    "layout (location = 1) in vec2 in_uv;\n"
    "layout (location = 2) in vec4 in_color;\n"
    "\n"
    "uniform mat4 u_projection;\n"
    "\n"
    "out vec2 v_uv;\n"
    "out vec4 v_color;\n"
    "\n"
    "void main() {\n"
    "    v_uv = in_uv;\n"
    "    v_color = in_color;\n"
    "    gl_Position = u_projection * vec4(in_pos, 0.0, 1.0);\n"
    "}\n";

inline constexpr std::string_view SPRITE_FRAGMENT_SHADER =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "in vec4 v_color;\n"
    "\n"
    "uniform sampler2D u_texture;\n"
    "uniform int u_use_texture;\n"
    "uniform float u_alpha_ref;\n"
    "uniform int u_alpha_test;\n"
    "\n"
    "layout (location = 0) out vec4 frag_color;\n"
    "\n"
    "void main() {\n"
    "    vec4 col = v_color;\n"
    "    if (u_use_texture != 0) {\n"
    "        col *= texture(u_texture, v_uv);\n"
    "    }\n"
    "    if (u_alpha_test != 0 && col.a < u_alpha_ref) {\n"
    "        discard;\n"
    "    }\n"
    "    frag_color = col;\n"
    "}\n";

class Shader {
public:
    Shader() = default;
    ~Shader();

    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;

    Shader(Shader&& other) noexcept;
    Shader& operator=(Shader&& other) noexcept;

    bool init(std::string_view vertex_src, std::string_view fragment_src);
    bool init_sprite_shader();
    static std::unique_ptr<Shader> create_sprite_shader();
    void destroy();

    void bind() const;
    static void unbind();

    GLint uniform_location(std::string_view name) const;
    GLint attrib_location(std::string_view name) const;

    void set_mat4(std::string_view name, const Mat4& mat) const;
    void set_mat4(GLint location, const Mat4& mat) const;
    void set_mat4(std::string_view name, const float* values) const;
    void set_mat4(GLint location, const float* values) const;

    void set_int(std::string_view name, int value) const;
    void set_int(GLint location, int value) const;

    void set_float(std::string_view name, float value) const;
    void set_float(GLint location, float value) const;

    void set_vec4(std::string_view name, float x, float y, float z, float w) const;
    void set_vec4(GLint location, float x, float y, float z, float w) const;
    void set_vec4(std::string_view name, const float* values) const;
    void set_vec4(GLint location, const float* values) const;

    [[nodiscard]] GLuint program() const noexcept { return program_; }
    [[nodiscard]] bool is_valid() const noexcept { return program_ != 0; }
    [[nodiscard]] const std::string& error_log() const noexcept { return error_log_; }

private:
    GLuint program_ = 0;
    mutable std::unordered_map<std::string, GLint> uniform_cache_;
    std::string error_log_;

    static GLuint compile_stage(GLenum type, std::string_view source, std::string& error_log);
};

} // namespace regml::glmodern
