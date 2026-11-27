#include "shader.hpp"
#include <fstream>
#if REGML_WIIU
#include "sprite_shader_gsh.hpp"
extern "C" void glWiiULoadShaderGroupGFD(unsigned int program, unsigned int index, const void *gfdData);
#endif

namespace regml::glmodern {

Shader::~Shader() {
    destroy();
}

Shader::Shader(Shader&& other) noexcept
    : program_(other.program_),
      uniform_cache_(std::move(other.uniform_cache_)),
      error_log_(std::move(other.error_log_)) {
    other.program_ = 0;
}

Shader& Shader::operator=(Shader&& other) noexcept {
    if (this != &other) {
        destroy();
        program_ = other.program_;
        uniform_cache_ = std::move(other.uniform_cache_);
        error_log_ = std::move(other.error_log_);
        other.program_ = 0;
    }
    return *this;
}

GLuint Shader::compile_stage(GLenum type, std::string_view source, std::string& error_log) {
    if (!glCreateShader) {
        error_log = "glCreateShader not loaded";
        return 0;
    }

    GLuint shader = glCreateShader(type);
    if (!shader) {
        error_log = "glCreateShader returned 0";
        return 0;
    }

    const GLchar* src = source.data();
    const GLint length = static_cast<GLint>(source.size());
#if REGML_WIIU
    {
        std::ofstream debug_log("/vol/external01/reGML/regml-init.log", std::ios::app);
        debug_log << "glCompileShader(type=" << type << " len=" << length << ")...\n";
        debug_log.flush();
    }
#endif
    glShaderSource(shader, 1, &src, &length);
    glCompileShader(shader);

    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
#if REGML_WIIU
    {
        std::ofstream debug_log("/vol/external01/reGML/regml-init.log", std::ios::app);
        debug_log << "glGetShaderiv GL_COMPILE_STATUS=" << compiled << "\n";
        debug_log.flush();
    }
#endif
    if (!compiled) {
        GLint log_len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_len);
        if (log_len > 0) {
            std::string log(static_cast<std::size_t>(log_len), '\0');
            glGetShaderInfoLog(shader, log_len, nullptr, log.data());
            error_log = std::move(log);
        } else {
            error_log = "Shader compile failed with empty info log";
        }
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

bool Shader::init(std::string_view vertex_src, std::string_view fragment_src) {
    destroy();

    std::string vert_err;
    GLuint vert = compile_stage(GL_VERTEX_SHADER, vertex_src, vert_err);
    if (!vert) {
        error_log_ = "Vertex compile error: " + vert_err;
        return false;
    }

    std::string frag_err;
    GLuint frag = compile_stage(GL_FRAGMENT_SHADER, fragment_src, frag_err);
    if (!frag) {
        error_log_ = "Fragment compile error: " + frag_err;
        glDeleteShader(vert);
        return false;
    }

    program_ = glCreateProgram();
    if (!program_) {
        error_log_ = "glCreateProgram failed";
        glDeleteShader(vert);
        glDeleteShader(frag);
        return false;
    }

    glAttachShader(program_, vert);
    glAttachShader(program_, frag);

    // Must match SpriteBatcher VAO attribute locations
    glBindAttribLocation(program_, 0, "in_pos");
    glBindAttribLocation(program_, 1, "in_uv");
    glBindAttribLocation(program_, 2, "in_color");

    glLinkProgram(program_);

    glDetachShader(program_, vert);
    glDetachShader(program_, frag);
    glDeleteShader(vert);
    glDeleteShader(frag);

    GLint linked = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint log_len = 0;
        glGetProgramiv(program_, GL_INFO_LOG_LENGTH, &log_len);
        if (log_len > 0) {
            std::string log(static_cast<std::size_t>(log_len), '\0');
            glGetProgramInfoLog(program_, log_len, nullptr, log.data());
            error_log_ = "Program link error: " + log;
        } else {
            error_log_ = "Program link failed with empty info log";
        }
        destroy();
        return false;
    }

    error_log_.clear();
#if REGML_WIIU
    {
        std::ofstream out("/vol/external01/reGML/regml-init.log", std::ios::app);
        GLint u_proj = glGetUniformLocation(program_, "u_projection");
        GLint u_tex = glGetUniformLocation(program_, "u_texture");
        GLint u_use = glGetUniformLocation(program_, "u_use_texture");
        GLint u_ref = glGetUniformLocation(program_, "u_alpha_ref");
        GLint u_test = glGetUniformLocation(program_, "u_alpha_test");
        out << "Sprite Shader loaded: u_proj=" << u_proj << " u_tex=" << u_tex
            << " u_use=" << u_use << " u_ref=" << u_ref << " u_test=" << u_test << "\n";
    }
#endif
    return true;
}

bool Shader::init_sprite_shader() {
#if REGML_WIIU
    destroy();
    program_ = glCreateProgram();
    if (!program_) {
        error_log_ = "glCreateProgram failed";
        return false;
    }
    glBindAttribLocation(program_, 0, "in_pos");
    glBindAttribLocation(program_, 1, "in_uv");
    glBindAttribLocation(program_, 2, "in_color");
    glWiiULoadShaderGroupGFD(program_, 0, SPRITE_SHADER_GSH);
    GLint linked = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint log_len = 0;
        glGetProgramiv(program_, GL_INFO_LOG_LENGTH, &log_len);
        if (log_len > 0) {
            std::string log(static_cast<std::size_t>(log_len), '\0');
            glGetProgramInfoLog(program_, log_len, nullptr, log.data());
            error_log_ = "GFD Program load error: " + log;
        } else {
            error_log_ = "GFD Program load failed with empty info log";
        }
        destroy();
        return false;
    }
    error_log_.clear();
    {
        std::ofstream out("/vol/external01/reGML/regml-init.log", std::ios::app);
        GLint u_proj = glGetUniformLocation(program_, "u_projection");
        GLint u_tex = glGetUniformLocation(program_, "u_texture");
        GLint u_use = glGetUniformLocation(program_, "u_use_texture");
        GLint u_ref = glGetUniformLocation(program_, "u_alpha_ref");
        GLint u_test = glGetUniformLocation(program_, "u_alpha_test");
        out << "Sprite Shader loaded: u_proj=" << u_proj << " u_tex=" << u_tex
            << " u_use=" << u_use << " u_ref=" << u_ref << " u_test=" << u_test << "\n";
    }
    return true;
#else
    return init(SPRITE_VERTEX_SHADER, SPRITE_FRAGMENT_SHADER);
#endif
}

std::unique_ptr<Shader> Shader::create_sprite_shader() {
    auto shader = std::make_unique<Shader>();
    if (!shader->init_sprite_shader()) {
        return nullptr;
    }
    return shader;
}

void Shader::destroy() {
    if (program_ != 0) {
        if (glDeleteProgram) {
            glDeleteProgram(program_);
        }
        program_ = 0;
    }
    uniform_cache_.clear();
}

void Shader::bind() const {
    if (program_ != 0 && glUseProgram) {
        glUseProgram(program_);
    }
}

void Shader::unbind() {
    if (glUseProgram) {
        glUseProgram(0);
    }
}

GLint Shader::uniform_location(std::string_view name) const {
    if (program_ == 0 || !glGetUniformLocation) return -1;
    std::string key(name);
    auto it = uniform_cache_.find(key);
    if (it != uniform_cache_.end()) {
        return it->second;
    }
    GLint loc = glGetUniformLocation(program_, key.c_str());
    uniform_cache_.emplace(std::move(key), loc);
    return loc;
}

GLint Shader::attrib_location(std::string_view name) const {
    if (program_ == 0 || !glGetAttribLocation) return -1;
    std::string key(name);
    return glGetAttribLocation(program_, key.c_str());
}

void Shader::set_mat4(std::string_view name, const Mat4& mat) const {
    set_mat4(uniform_location(name), mat);
}

void Shader::set_mat4(GLint location, const Mat4& mat) const {
    if (location != -1 && glUniformMatrix4fv) {
        glUniformMatrix4fv(location, 1, GL_FALSE, mat.m.data());
    }
}

void Shader::set_mat4(std::string_view name, const float* values) const {
    set_mat4(uniform_location(name), values);
}

void Shader::set_mat4(GLint location, const float* values) const {
    if (location != -1 && values && glUniformMatrix4fv) {
        glUniformMatrix4fv(location, 1, GL_FALSE, values);
    }
}

void Shader::set_int(std::string_view name, int value) const {
    set_int(uniform_location(name), value);
}

void Shader::set_int(GLint location, int value) const {
    if (location != -1 && glUniform1i) {
        glUniform1i(location, value);
    }
}

void Shader::set_float(std::string_view name, float value) const {
    set_float(uniform_location(name), value);
}

void Shader::set_float(GLint location, float value) const {
    if (location != -1 && glUniform1f) {
        glUniform1f(location, value);
    }
}

void Shader::set_vec4(std::string_view name, float x, float y, float z, float w) const {
    set_vec4(uniform_location(name), x, y, z, w);
}

void Shader::set_vec4(GLint location, float x, float y, float z, float w) const {
    if (location != -1 && glUniform4f) {
        glUniform4f(location, x, y, z, w);
    }
}

void Shader::set_vec4(std::string_view name, const float* values) const {
    set_vec4(uniform_location(name), values);
}

void Shader::set_vec4(GLint location, const float* values) const {
    if (location != -1 && values && glUniform4fv) {
        glUniform4fv(location, 1, values);
    }
}

} // namespace regml::glmodern
