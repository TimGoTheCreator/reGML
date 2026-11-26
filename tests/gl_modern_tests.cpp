#include "glmodern/gl_common.hpp"
#include "glmodern/gl_core.hpp"
#include "glmodern/shader.hpp"
#include "glmodern/texture.hpp"
#include "glmodern/batcher.hpp"
#include "glmodern/fbo.hpp"
#include "glmodern/canvas.hpp"
#include "glmodern/assets.hpp"
#include "glmodern/gl_gpu.hpp"

#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

void check(bool condition, const char* msg) {
    if (!condition) {
        std::cerr << "Assertion failed: " << msg << std::endl;
        std::exit(1);
    }
}

// Mock OpenGL state for end-to-end testing of Shader class
static GLuint s_next_id = 1;
static std::vector<std::string> s_shader_sources;
static std::vector<std::pair<std::string, GLint>> s_bound_attribs;
static GLuint s_bound_program = 0;
static GLint s_last_uniform_int_loc = -1;
static GLint s_last_uniform_int_val = -1;
static GLint s_last_uniform_float_loc = -1;
static GLfloat s_last_uniform_float_val = 0.0f;
static GLfloat s_last_uniform_vec4[4] = {};
static GLfloat s_last_uniform_mat4[16] = {};

static GLuint GLAPIENTRY mock_glCreateShader(GLenum type) {
    (void)type;
    return s_next_id++;
}

static void GLAPIENTRY mock_glShaderSource(GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length) {
    (void)shader;
    (void)count;
    (void)length;
    if (string && string[0]) {
        s_shader_sources.push_back(string[0]);
    }
}

static void GLAPIENTRY mock_glCompileShader(GLuint shader) {
    (void)shader;
}

static void GLAPIENTRY mock_glGetShaderiv(GLuint shader, GLenum pname, GLint *params) {
    (void)shader;
    if (pname == GL_COMPILE_STATUS && params) {
        *params = GL_TRUE;
    }
}

static void GLAPIENTRY mock_glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog) {
    (void)shader;
    (void)bufSize;
    if (length) *length = 0;
    if (infoLog) infoLog[0] = '\0';
}

static GLuint GLAPIENTRY mock_glCreateProgram() {
    return s_next_id++;
}

static void GLAPIENTRY mock_glAttachShader(GLuint program, GLuint shader) {
    (void)program;
    (void)shader;
}

static void GLAPIENTRY mock_glDetachShader(GLuint program, GLuint shader) {
    (void)program;
    (void)shader;
}

static void GLAPIENTRY mock_glBindAttribLocation(GLuint program, GLuint index, const GLchar *name) {
    (void)program;
    if (name) {
        s_bound_attribs.emplace_back(name, index);
    }
}

static void GLAPIENTRY mock_glLinkProgram(GLuint program) {
    (void)program;
}

static void GLAPIENTRY mock_glGetProgramiv(GLuint program, GLenum pname, GLint *params) {
    (void)program;
    if (pname == GL_LINK_STATUS && params) {
        *params = GL_TRUE;
    }
}

static void GLAPIENTRY mock_glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog) {
    (void)program;
    (void)bufSize;
    if (length) *length = 0;
    if (infoLog) infoLog[0] = '\0';
}

static void GLAPIENTRY mock_glUseProgram(GLuint program) {
    s_bound_program = program;
}

static void GLAPIENTRY mock_glDeleteShader(GLuint shader) {
    (void)shader;
}

static void GLAPIENTRY mock_glDeleteProgram(GLuint program) {
    if (s_bound_program == program) {
        s_bound_program = 0;
    }
}

static GLint GLAPIENTRY mock_glGetUniformLocation(GLuint program, const GLchar *name) {
    (void)program;
    if (!name) return -1;
    if (std::strcmp(name, "u_projection") == 0) return 10;
    if (std::strcmp(name, "u_texture") == 0) return 11;
    if (std::strcmp(name, "u_use_texture") == 0) return 12;
    if (std::strcmp(name, "u_alpha_ref") == 0) return 13;
    if (std::strcmp(name, "u_alpha_test") == 0) return 14;
    return 100;
}

static void GLAPIENTRY mock_glUniform1i(GLint location, GLint v0) {
    s_last_uniform_int_loc = location;
    s_last_uniform_int_val = v0;
}

static void GLAPIENTRY mock_glUniform1f(GLint location, GLfloat v0) {
    s_last_uniform_float_loc = location;
    s_last_uniform_float_val = v0;
}

static void GLAPIENTRY mock_glUniform4f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3) {
    (void)location;
    s_last_uniform_vec4[0] = v0;
    s_last_uniform_vec4[1] = v1;
    s_last_uniform_vec4[2] = v2;
    s_last_uniform_vec4[3] = v3;
}

static void GLAPIENTRY mock_glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
    (void)location;
    (void)count;
    (void)transpose;
    if (value) {
        std::memcpy(s_last_uniform_mat4, value, sizeof(s_last_uniform_mat4));
    }
}

static GLuint s_bound_texture = 0;
static GLuint s_bound_vao = 0;
static GLuint s_bound_vbo = 0;
static GLuint s_bound_ebo = 0;
static GLsizei s_draw_elements_count = 0;
static bool s_blend_enabled = false;
static GLenum s_blend_src_rgb = 0, s_blend_dst_rgb = 0;
static GLenum s_mock_gl_error = GL_NO_ERROR;
static bool s_mock_tex_image_fail = false;

static GLenum GLAPIENTRY mock_glGetError() {
    GLenum err = s_mock_gl_error;
    s_mock_gl_error = GL_NO_ERROR;
    return err;
}

static void GLAPIENTRY mock_glGenTextures(GLsizei n, GLuint *textures) {
    for (GLsizei i = 0; i < n; ++i) textures[i] = s_next_id++;
}
static void GLAPIENTRY mock_glDeleteTextures(GLsizei n, const GLuint *textures) { (void)n; (void)textures; }
static void GLAPIENTRY mock_glBindTexture(GLenum target, GLuint texture) { (void)target; s_bound_texture = texture; }
static void GLAPIENTRY mock_glActiveTexture(GLenum texture) { (void)texture; }
static void GLAPIENTRY mock_glTexParameteri(GLenum target, GLenum pname, GLint param) { (void)target; (void)pname; (void)param; }
static void GLAPIENTRY mock_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels) {
    (void)target; (void)level; (void)internalformat; (void)width; (void)height; (void)border; (void)format; (void)type; (void)pixels;
    if (s_mock_tex_image_fail) {
        s_mock_gl_error = GL_OUT_OF_MEMORY;
    }
}
static void GLAPIENTRY mock_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels) {
    (void)target; (void)level; (void)xoffset; (void)yoffset; (void)width; (void)height; (void)format; (void)type; (void)pixels;
}
static void GLAPIENTRY mock_glPixelStorei(GLenum pname, GLint param) { (void)pname; (void)param; }
static void GLAPIENTRY mock_glGenVertexArrays(GLsizei n, GLuint *arrays) {
    for (GLsizei i = 0; i < n; ++i) arrays[i] = s_next_id++;
}
static void GLAPIENTRY mock_glDeleteVertexArrays(GLsizei n, const GLuint *arrays) { (void)n; (void)arrays; }
static void GLAPIENTRY mock_glBindVertexArray(GLuint array) { s_bound_vao = array; }
static void GLAPIENTRY mock_glGenBuffers(GLsizei n, GLuint *buffers) {
    for (GLsizei i = 0; i < n; ++i) buffers[i] = s_next_id++;
}
static void GLAPIENTRY mock_glDeleteBuffers(GLsizei n, const GLuint *buffers) { (void)n; (void)buffers; }
static void GLAPIENTRY mock_glBindBuffer(GLenum target, GLuint buffer) {
    if (target == GL_ARRAY_BUFFER) s_bound_vbo = buffer;
    if (target == GL_ELEMENT_ARRAY_BUFFER) s_bound_ebo = buffer;
}
static void GLAPIENTRY mock_glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
    (void)target; (void)size; (void)data; (void)usage;
}
static void GLAPIENTRY mock_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data) {
    (void)target; (void)offset; (void)size; (void)data;
}
static void GLAPIENTRY mock_glEnableVertexAttribArray(GLuint index) { (void)index; }
static void GLAPIENTRY mock_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer) {
    (void)index; (void)size; (void)type; (void)normalized; (void)stride; (void)pointer;
}
static void GLAPIENTRY mock_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
    (void)mode; (void)type; (void)indices; s_draw_elements_count += count;
}
static void GLAPIENTRY mock_glEnable(GLenum cap) { if (cap == GL_BLEND) s_blend_enabled = true; }
static void GLAPIENTRY mock_glDisable(GLenum cap) { if (cap == GL_BLEND) s_blend_enabled = false; }
static void GLAPIENTRY mock_glBlendFuncSeparate(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha) {
    s_blend_src_rgb = sfactorRGB; s_blend_dst_rgb = dfactorRGB;
    (void)sfactorAlpha; (void)dfactorAlpha;
}
static void GLAPIENTRY mock_glBlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha) { (void)modeRGB; (void)modeAlpha; }

static GLuint s_bound_fbo = 0;
static GLuint s_fbo_attached_tex = 0;
static GLuint s_last_read_pixels_call = 0;
static GLint s_last_viewport[4] = {};
static GLfloat s_last_clear_color[4] = {};
static GLboolean s_last_color_mask[4] = {};
static GLbitfield s_last_clear_mask = 0;

static void GLAPIENTRY mock_glGenFramebuffers(GLsizei n, GLuint *framebuffers) {
    for (GLsizei i = 0; i < n; ++i) framebuffers[i] = s_next_id++;
}
static void GLAPIENTRY mock_glDeleteFramebuffers(GLsizei n, const GLuint *framebuffers) {
    (void)n;
    if (framebuffers && framebuffers[0] == s_bound_fbo) s_bound_fbo = 0;
}
static void GLAPIENTRY mock_glBindFramebuffer(GLenum target, GLuint framebuffer) {
    (void)target;
    s_bound_fbo = framebuffer;
}
static void GLAPIENTRY mock_glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
    (void)target; (void)attachment; (void)textarget; (void)level;
    s_fbo_attached_tex = texture;
}
static GLenum GLAPIENTRY mock_glCheckFramebufferStatus(GLenum target) {
    (void)target;
    return GL_FRAMEBUFFER_COMPLETE;
}
static void GLAPIENTRY mock_glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    s_last_viewport[0] = x; s_last_viewport[1] = y; s_last_viewport[2] = width; s_last_viewport[3] = height;
}
static void GLAPIENTRY mock_glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) {
    s_last_clear_color[0] = red; s_last_clear_color[1] = green; s_last_clear_color[2] = blue; s_last_clear_color[3] = alpha;
}
static void GLAPIENTRY mock_glClear(GLbitfield mask) {
    s_last_clear_mask = mask;
}
static void GLAPIENTRY mock_glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha) {
    s_last_color_mask[0] = red; s_last_color_mask[1] = green; s_last_color_mask[2] = blue; s_last_color_mask[3] = alpha;
}
static void GLAPIENTRY mock_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels) {
    (void)x; (void)y; (void)format; (void)type;
    s_last_read_pixels_call++;
    if (pixels) {
        std::uint32_t *px = reinterpret_cast<std::uint32_t *>(pixels);
        for (GLsizei i = 0; i < width * height; ++i) {
            px[i] = 0xff123456U;
        }
    }
}
static void GLAPIENTRY mock_glGetIntegerv(GLenum pname, GLint *data) {
    if (pname == GL_FRAMEBUFFER_BINDING && data) {
        *data = static_cast<GLint>(s_bound_fbo);
    }
}

void* mock_gl_loader(const char* name) {
    if (std::strcmp(name, "glGenFramebuffers") == 0) return (void*)&mock_glGenFramebuffers;
    if (std::strcmp(name, "glDeleteFramebuffers") == 0) return (void*)&mock_glDeleteFramebuffers;
    if (std::strcmp(name, "glBindFramebuffer") == 0) return (void*)&mock_glBindFramebuffer;
    if (std::strcmp(name, "glFramebufferTexture2D") == 0) return (void*)&mock_glFramebufferTexture2D;
    if (std::strcmp(name, "glCheckFramebufferStatus") == 0) return (void*)&mock_glCheckFramebufferStatus;
    if (std::strcmp(name, "glViewport") == 0) return (void*)&mock_glViewport;
    if (std::strcmp(name, "glClearColor") == 0) return (void*)&mock_glClearColor;
    if (std::strcmp(name, "glClear") == 0) return (void*)&mock_glClear;
    if (std::strcmp(name, "glColorMask") == 0) return (void*)&mock_glColorMask;
    if (std::strcmp(name, "glGetError") == 0) return (void*)&mock_glGetError;
    if (std::strcmp(name, "glReadPixels") == 0) return (void*)&mock_glReadPixels;
    if (std::strcmp(name, "glGetIntegerv") == 0) return (void*)&mock_glGetIntegerv;
    if (std::strcmp(name, "glGenTextures") == 0) return (void*)&mock_glGenTextures;
    if (std::strcmp(name, "glDeleteTextures") == 0) return (void*)&mock_glDeleteTextures;
    if (std::strcmp(name, "glBindTexture") == 0) return (void*)&mock_glBindTexture;
    if (std::strcmp(name, "glActiveTexture") == 0) return (void*)&mock_glActiveTexture;
    if (std::strcmp(name, "glTexParameteri") == 0) return (void*)&mock_glTexParameteri;
    if (std::strcmp(name, "glTexImage2D") == 0) return (void*)&mock_glTexImage2D;
    if (std::strcmp(name, "glTexSubImage2D") == 0) return (void*)&mock_glTexSubImage2D;
    if (std::strcmp(name, "glPixelStorei") == 0) return (void*)&mock_glPixelStorei;
    if (std::strcmp(name, "glGenVertexArrays") == 0) return (void*)&mock_glGenVertexArrays;
    if (std::strcmp(name, "glDeleteVertexArrays") == 0) return (void*)&mock_glDeleteVertexArrays;
    if (std::strcmp(name, "glBindVertexArray") == 0) return (void*)&mock_glBindVertexArray;
    if (std::strcmp(name, "glGenBuffers") == 0) return (void*)&mock_glGenBuffers;
    if (std::strcmp(name, "glDeleteBuffers") == 0) return (void*)&mock_glDeleteBuffers;
    if (std::strcmp(name, "glBindBuffer") == 0) return (void*)&mock_glBindBuffer;
    if (std::strcmp(name, "glBufferData") == 0) return (void*)&mock_glBufferData;
    if (std::strcmp(name, "glBufferSubData") == 0) return (void*)&mock_glBufferSubData;
    if (std::strcmp(name, "glEnableVertexAttribArray") == 0) return (void*)&mock_glEnableVertexAttribArray;
    if (std::strcmp(name, "glVertexAttribPointer") == 0) return (void*)&mock_glVertexAttribPointer;
    if (std::strcmp(name, "glDrawElements") == 0) return (void*)&mock_glDrawElements;
    if (std::strcmp(name, "glEnable") == 0) return (void*)&mock_glEnable;
    if (std::strcmp(name, "glDisable") == 0) return (void*)&mock_glDisable;
    if (std::strcmp(name, "glBlendFuncSeparate") == 0) return (void*)&mock_glBlendFuncSeparate;
    if (std::strcmp(name, "glBlendEquationSeparate") == 0) return (void*)&mock_glBlendEquationSeparate;
    if (std::strcmp(name, "glCreateShader") == 0) return (void*)&mock_glCreateShader;
    if (std::strcmp(name, "glShaderSource") == 0) return (void*)&mock_glShaderSource;
    if (std::strcmp(name, "glCompileShader") == 0) return (void*)&mock_glCompileShader;
    if (std::strcmp(name, "glGetShaderiv") == 0) return (void*)&mock_glGetShaderiv;
    if (std::strcmp(name, "glGetShaderInfoLog") == 0) return (void*)&mock_glGetShaderInfoLog;
    if (std::strcmp(name, "glCreateProgram") == 0) return (void*)&mock_glCreateProgram;
    if (std::strcmp(name, "glAttachShader") == 0) return (void*)&mock_glAttachShader;
    if (std::strcmp(name, "glDetachShader") == 0) return (void*)&mock_glDetachShader;
    if (std::strcmp(name, "glBindAttribLocation") == 0) return (void*)&mock_glBindAttribLocation;
    if (std::strcmp(name, "glLinkProgram") == 0) return (void*)&mock_glLinkProgram;
    if (std::strcmp(name, "glGetProgramiv") == 0) return (void*)&mock_glGetProgramiv;
    if (std::strcmp(name, "glGetProgramInfoLog") == 0) return (void*)&mock_glGetProgramInfoLog;
    if (std::strcmp(name, "glUseProgram") == 0) return (void*)&mock_glUseProgram;
    if (std::strcmp(name, "glDeleteShader") == 0) return (void*)&mock_glDeleteShader;
    if (std::strcmp(name, "glDeleteProgram") == 0) return (void*)&mock_glDeleteProgram;
    if (std::strcmp(name, "glGetUniformLocation") == 0) return (void*)&mock_glGetUniformLocation;
    if (std::strcmp(name, "glUniform1i") == 0) return (void*)&mock_glUniform1i;
    if (std::strcmp(name, "glUniform1f") == 0) return (void*)&mock_glUniform1f;
    if (std::strcmp(name, "glUniform4f") == 0) return (void*)&mock_glUniform4f;
    if (std::strcmp(name, "glUniformMatrix4fv") == 0) return (void*)&mock_glUniformMatrix4fv;
    return nullptr;
}

} // namespace

int main() {
    using namespace regml::glmodern;

    std::cout << "[Test] Verifying OpenGL 3.3 Core profile constants...\n";
    check(GL_TRIANGLES == 0x0004, "GL_TRIANGLES == 4");
    check(GL_COLOR_BUFFER_BIT == 0x4000, "GL_COLOR_BUFFER_BIT == 0x4000");
    check(GL_SRC_ALPHA == 0x0302, "GL_SRC_ALPHA == 0x0302");
    check(GL_ONE_MINUS_SRC_ALPHA == 0x0303, "GL_ONE_MINUS_SRC_ALPHA == 0x0303");
    check(GL_TEXTURE_2D == 0x0DE1, "GL_TEXTURE_2D == 0x0DE1");
    check(GL_ARRAY_BUFFER == 0x8892, "GL_ARRAY_BUFFER == 0x8892");
    check(GL_DYNAMIC_DRAW == 0x88E8, "GL_DYNAMIC_DRAW == 0x88E8");
    check(GL_ELEMENT_ARRAY_BUFFER == 0x8893, "GL_ELEMENT_ARRAY_BUFFER == 0x8893");

    std::cout << "[Test] Verifying stub execution without crash...\n";
    glViewport(0, 0, 800, 600);
    glClearColor(1.0f, 0.5f, 0.25f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    std::cout << "[Test] Verifying shader GLSL 330 core source code...\n";
    check(SPRITE_VERTEX_SHADER.find("#version 330 core") != std::string_view::npos, "vertex version 330 core");
    check(SPRITE_VERTEX_SHADER.find("in vec2 in_pos") != std::string_view::npos, "vertex in_pos");
    check(SPRITE_VERTEX_SHADER.find("in vec2 in_uv") != std::string_view::npos, "vertex in_uv");
    check(SPRITE_VERTEX_SHADER.find("in vec4 in_color") != std::string_view::npos, "vertex in_color");
    check(SPRITE_VERTEX_SHADER.find("uniform mat4 u_projection") != std::string_view::npos, "vertex u_projection");

    check(SPRITE_FRAGMENT_SHADER.find("#version 330 core") != std::string_view::npos, "fragment version 330 core");
    check(SPRITE_FRAGMENT_SHADER.find("uniform sampler2D u_texture") != std::string_view::npos, "fragment u_texture");
    check(SPRITE_FRAGMENT_SHADER.find("uniform int u_use_texture") != std::string_view::npos, "fragment u_use_texture");
    check(SPRITE_FRAGMENT_SHADER.find("uniform float u_alpha_ref") != std::string_view::npos, "fragment u_alpha_ref");
    check(SPRITE_FRAGMENT_SHADER.find("uniform int u_alpha_test") != std::string_view::npos, "fragment u_alpha_test");
    check(SPRITE_FRAGMENT_SHADER.find("discard") != std::string_view::npos, "fragment discard on alpha test");

    std::cout << "[Test] Loading OpenGL functions via mock loader...\n";
    load_gl_core_3_3(mock_gl_loader);

    std::cout << "[Test] Compiling 2D sprite shader with Shader class...\n";
    Shader shader;
    check(!shader.is_valid(), "initially invalid");
    check(shader.init_sprite_shader(), "compile sprite shader successfully");
    check(shader.is_valid(), "shader is valid after compilation");
    check(shader.program() != 0, "program id is non-zero");

    std::cout << "[Test] Testing Shader binding and uniform setting...\n";
    shader.bind();
    check(s_bound_program == shader.program(), "bind program active");

    shader.set_int("u_use_texture", 1);
    check(s_last_uniform_int_loc == 12, "u_use_texture location matched");
    check(s_last_uniform_int_val == 1, "u_use_texture value set to 1");

    shader.set_float("u_alpha_ref", 0.5f);
    check(s_last_uniform_float_loc == 13, "u_alpha_ref location matched");
    check(std::abs(s_last_uniform_float_val - 0.5f) < 1e-5f, "u_alpha_ref value set to 0.5");

    shader.set_vec4("u_color", 1.0f, 0.2f, 0.3f, 0.9f);
    check(std::abs(s_last_uniform_vec4[0] - 1.0f) < 1e-5f, "vec4 x");
    check(std::abs(s_last_uniform_vec4[1] - 0.2f) < 1e-5f, "vec4 y");
    check(std::abs(s_last_uniform_vec4[2] - 0.3f) < 1e-5f, "vec4 z");
    check(std::abs(s_last_uniform_vec4[3] - 0.9f) < 1e-5f, "vec4 w");

    std::cout << "[Test] Verifying Mat4 orthographic projection matrices...\n";
    {
        Mat4 m2d = Mat4::ortho_2d(640.0f, 480.0f);
        check(std::abs(m2d.m[0] - (2.0f / 640.0f)) < 1e-6f, "ortho_2d m[0] == 2/w");
        check(std::abs(m2d.m[5] - (-2.0f / 480.0f)) < 1e-6f, "ortho_2d m[5] == -2/h");
        check(m2d.m[10] == -1.0f, "ortho_2d m[10] == -1");
        check(m2d.m[12] == -1.0f, "ortho_2d m[12] == -1");
        check(m2d.m[13] == 1.0f, "ortho_2d m[13] == 1");
        check(m2d.m[15] == 1.0f, "ortho_2d m[15] == 1");

        // Test NDC mapping: y = 0 (top) must map to +1.0f, y = height (bottom) must map to -1.0f
        float y_top_ndc = 0.0f * m2d.m[5] + m2d.m[13];
        float y_bottom_ndc = 480.0f * m2d.m[5] + m2d.m[13];
        float x_left_ndc = 0.0f * m2d.m[0] + m2d.m[12];
        float x_right_ndc = 640.0f * m2d.m[0] + m2d.m[12];
        check(std::abs(y_top_ndc - 1.0f) < 1e-6f, "y = 0 maps to NDC +1 (top)");
        check(std::abs(y_bottom_ndc - (-1.0f)) < 1e-6f, "y = 480 maps to NDC -1 (bottom)");
        check(std::abs(x_left_ndc - (-1.0f)) < 1e-6f, "x = 0 maps to NDC -1 (left)");
        check(std::abs(x_right_ndc - 1.0f) < 1e-6f, "x = 640 maps to NDC +1 (right)");

        // Mat4::ortho(0, w, h, 0) should match Mat4::ortho_2d(w, h)
        Mat4 mo = Mat4::ortho(0.0f, 640.0f, 480.0f, 0.0f);
        for (std::size_t i = 0; i < 16; ++i) {
            check(std::abs(mo.m[i] - m2d.m[i]) < 1e-6f, "ortho matches ortho_2d");
        }
    }

    Mat4 test_mat = Mat4::ortho_2d(640.0f, 480.0f);
    shader.set_mat4("u_projection", test_mat);
    check(std::abs(s_last_uniform_mat4[0] - test_mat.m[0]) < 1e-6f, "mat4 projection set");

    std::cout << "[Test] Testing Shader move semantics...\n";
    GLuint prog_id = shader.program();
    Shader moved(std::move(shader));
    check(!shader.is_valid(), "original shader now invalid");
    check(moved.is_valid(), "moved shader is valid");
    check(moved.program() == prog_id, "moved shader retained program id");

    Shader::unbind();
    check(s_bound_program == 0, "unbind program active");

    std::cout << "[Test] Testing GLTexture...\n";
    {
        std::vector<std::uint32_t> test_pixels(16 * 16, 0xff00ff00U);
        GLTexture tex;
        check(!tex.valid(), "texture initially invalid");
        tex.upload(16, 16, test_pixels, false);
        check(tex.valid(), "texture valid after upload");
        check(tex.width() == 16, "width == 16");
        check(tex.height() == 16, "height == 16");
        check(tex.bytes() == 16 * 16 * 4, "bytes == 1024");
        check(!tex.smooth(), "not smooth");

        tex.bind(0);
        check(s_bound_texture == tex.id(), "bound texture matches id");

        GLTexture::unbind(0);
        check(s_bound_texture == 0, "unbind texture resets bound texture");

        tex.set_filter(true);
        check(tex.smooth(), "smooth set to true");

        // Sub image upload
        std::vector<std::uint32_t> sub_pixels(4 * 4, 0xffff0000U);
        tex.upload_sub(2, 2, 4, 4, sub_pixels);

        // Move semantics
        GLuint saved_id = tex.id();
        GLTexture moved_tex(std::move(tex));
        check(!tex.valid(), "moved texture is invalid");
        check(moved_tex.valid(), "new texture is valid");
        check(moved_tex.id() == saved_id, "retained id");

        // Error handling: if glGetError reports an error (e.g. GL_OUT_OF_MEMORY), texture is destroyed and invalid
        s_mock_tex_image_fail = true;
        GLTexture failed_tex(16, 16, test_pixels);
        s_mock_tex_image_fail = false;
        check(!failed_tex.valid(), "failed_tex is invalid when glGetError reports error");
        check(failed_tex.id() == 0, "failed_tex id is 0");
    }

    std::cout << "[Test] Testing TextureCache LRU eviction and budgeting...\n";
    {
        // 1024 bytes budget: exactly four 8x8 RGBA textures (256 bytes each)
        TextureCache cache(1024);
        check(cache.budget() == 1024, "budget matches");
        check(cache.cache_bytes() == 0, "cache bytes initially 0");
        check(cache.size() == 0, "cache size initially 0");

        std::vector<std::uint32_t> px(64, 0xffffffffU);
        GLuint id1 = cache.get_or_upload(101, px, 8, 8);
        check(cache.size() == 1, "size 1");
        check(cache.cache_bytes() == 256, "cache bytes 256");
        check(cache.contains(101), "contains 101");
        check(cache.get(101) == id1, "get(101) == id1");

        cache.get_or_upload(102, px, 8, 8);
        check(cache.size() == 2, "size 2");
        check(cache.cache_bytes() == 512, "cache bytes 512");

        cache.get_or_upload(103, px, 8, 8);
        check(cache.size() == 3, "size 3");
        check(cache.cache_bytes() == 768, "cache bytes 768");

        cache.get_or_upload(104, px, 8, 8);
        check(cache.size() == 4, "size 4");
        check(cache.cache_bytes() == 1024, "cache bytes 1024");

        // Touch 101 so 102 becomes the oldest
        check(cache.touch(101), "touch 101");

        // 5th upload: should evict 102
        cache.get_or_upload(105, px, 8, 8);
        check(cache.size() == 4, "size 4 after eviction");
        check(cache.cache_bytes() == 1024, "cache bytes remains 1024");
        check(cache.contains(101), "101 still present");
        check(!cache.contains(102), "102 was evicted");
        check(cache.contains(103), "103 still present");
        check(cache.contains(104), "104 still present");
        check(cache.contains(105), "105 present");

        cache.clear();
        check(cache.size() == 0, "size 0 after clear");
        check(cache.cache_bytes() == 0, "bytes 0 after clear");

        // When upload fails, cache should not retain entry and should return 0
        s_mock_tex_image_fail = true;
        GLuint failed_id = cache.get_or_upload(999, px, 8, 8);
        s_mock_tex_image_fail = false;
        check(failed_id == 0, "failed upload returns 0 id");
        check(!cache.contains(999), "failed upload not inserted into cache");
        check(cache.size() == 0, "size stays 0 after failed upload");
    }

    std::cout << "[Test] Testing SpriteBatcher quad streaming, state flush, and buffer capacity...\n";
    {
        SpriteBatcher batcher;
        check(batcher.init(), "batcher init");

        Mat4 proj = Mat4::ortho(0.0f, 640.0f, 480.0f, 0.0f);
        batcher.begin(proj);
        check(batcher.is_active(), "batcher is active");
        check(batcher.quad_count() == 0, "quad count is 0");

        // Draw quads with texture 1
        batcher.draw_quad(1, 10.0f, 10.0f, 32.0f, 32.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0xffffffffU, 0.0f);
        check(batcher.quad_count() == 1, "quad count 1");

        batcher.draw_quad(1, 50.0f, 50.0f, 32.0f, 32.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0xff00ffffU, 45.0f);
        check(batcher.quad_count() == 2, "quad count 2");

        // Texture change triggers flush
        s_draw_elements_count = 0;
        batcher.draw_quad(2, 100.0f, 100.0f, 64.0f, 64.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0xffffffffU);
        check(s_draw_elements_count == 12, "flushed 2 quads = 12 indices on texture change");
        check(batcher.quad_count() == 1, "quad count 1 after texture switch");

        // Blend mode change triggers flush
        s_draw_elements_count = 0;
        batcher.set_blend_mode(5, 2, 5, 2); // additive
        check(s_draw_elements_count == 6, "flushed 1 quad on blend mode change");
        check(batcher.quad_count() == 0, "quad count 0 after blend change");
        check(s_blend_src_rgb == GL_SRC_ALPHA && s_blend_dst_rgb == GL_ONE, "blend factors updated");

        // Buffer full triggers flush
        s_draw_elements_count = 0;
        for (std::size_t i = 0; i < SpriteBatcher::MAX_QUADS; ++i) {
            batcher.draw_quad(3, (float)i, (float)i, 4.0f, 4.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0xffffffffU);
        }
        check(batcher.quad_count() == SpriteBatcher::MAX_QUADS, "buffer filled to MAX_QUADS");
        check(s_draw_elements_count == 0, "no draw yet until flush");

        // Adding one more quad flushes the 2048 quads and buffers the new one
        batcher.draw_quad(3, 999.0f, 999.0f, 4.0f, 4.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0xffffffffU);
        check(s_draw_elements_count == SpriteBatcher::MAX_INDICES, "flushed 2048 quads = 12288 indices");
        check(batcher.quad_count() == 1, "quad count 1 after overflow flush");

        batcher.end();
        check(!batcher.is_active(), "batcher inactive after end()");
        check(batcher.quad_count() == 0, "quad count 0 after end()");
    }

    std::cout << "[Test] Testing GLFramebuffer (FBO wrapper)...\n";
    {
        GLFramebuffer fbo;
        check(!fbo.valid(), "fbo initially invalid");
        check(fbo.init(320, 240, false), "fbo init 320x240");
        check(fbo.valid(), "fbo valid after init");
        check(fbo.is_complete(), "fbo is complete");
        check(fbo.width() == 320, "width 320");
        check(fbo.height() == 240, "height 240");
        check(fbo.texture_id() != 0, "texture id non-zero");
        check(fbo.fbo_id() != 0, "fbo id non-zero");

        fbo.bind(true);
        check(s_bound_fbo == fbo.fbo_id(), "fbo is bound");
        check(s_last_viewport[2] == 320 && s_last_viewport[3] == 240, "viewport updated to fbo size");

        GLFramebuffer::unbind();
        check(s_bound_fbo == 0, "fbo unbound to default");

        std::vector<std::uint32_t> out_px(320 * 240);
        check(fbo.readback(out_px), "readback succeeded");
        check(out_px[0] == 0xff123456U, "readback pixel value matched");

        std::uint32_t p = fbo.pixel(10, 10);
        check(p == 0xff123456U, "single pixel readback");

        // Move semantics
        GLuint saved_fbo = fbo.fbo_id();
        GLuint saved_tex = fbo.texture_id();
        GLFramebuffer moved_fbo(std::move(fbo));
        check(!fbo.valid(), "original fbo now invalid");
        check(moved_fbo.valid(), "moved fbo is valid");
        check(moved_fbo.fbo_id() == saved_fbo, "fbo id preserved");
        check(moved_fbo.texture_id() == saved_tex, "texture id preserved");
    }

    std::cout << "[Test] Testing GLModernCanvas (Canvas methods & rendering)...\n";
    {
        GLModernCanvas canvas(640, 480);
        check(canvas.width() == 640, "width 640");
        check(canvas.height() == 480, "height 480");
        check(canvas.logical_width() == 640, "logical_width 640");
        check(canvas.logical_height() == 480, "logical_height 480");
        check(canvas.render_scale() == 1.0, "render_scale 1.0");
        check(canvas.gpu_target(), "gpu_target is true");
        check(canvas.texture_id() != 0, "texture_id valid");

        // Clear tests
        canvas.clear(0x00ff00); // GML green
        check(s_last_clear_mask == GL_COLOR_BUFFER_BIT, "clear called");
        check(std::abs(s_last_clear_color[1] - 1.0f) < 1e-4f, "clear green channel");

        canvas.clear_transparent();
        check(s_last_clear_color[3] == 0.0f, "clear transparent alpha == 0");

        // Color and alpha
        canvas.color(0x0000ff); // GML red
        canvas.alpha(0.8);

        // Blend mode
        canvas.blend_enable(false);
        canvas.blend_mode(5, 6, 5, 6);

        // Alpha test
        canvas.alpha_test_enable(true);
        canvas.alpha_test_ref(64);

        // Color write mask
        canvas.color_write_enable(true, false, true, false);
        check(s_last_color_mask[0] == GL_TRUE, "red write enabled");
        check(s_last_color_mask[1] == GL_FALSE, "green write disabled");
        check(s_last_color_mask[2] == GL_TRUE, "blue write enabled");
        check(s_last_color_mask[3] == GL_FALSE, "alpha write disabled");

        // Primitives: rectangle, line, triangle, circle, ellipse, round_rectangle
        s_draw_elements_count = 0;
        canvas.rectangle(10.0, 10.0, 50.0, 50.0, false);
        canvas.rectangle(10.0, 10.0, 50.0, 50.0, true);
        canvas.line(0.0, 0.0, 100.0, 100.0, 2.0);
        canvas.triangle(10.0, 10.0, 60.0, 10.0, 30.0, 50.0, false);
        canvas.triangle(10.0, 10.0, 60.0, 10.0, 30.0, 50.0, true);
        canvas.circle(50.0, 50.0, 20.0, false);
        canvas.circle(50.0, 50.0, 20.0, true);
        canvas.ellipse(20.0, 20.0, 80.0, 60.0, false);
        canvas.ellipse(20.0, 20.0, 80.0, 60.0, true);
        canvas.round_rectangle(15.0, 15.0, 65.0, 65.0, 5.0, 5.0, false);
        canvas.round_rectangle(15.0, 15.0, 65.0, 65.0, 5.0, 5.0, true);

        // Surfaces
        GLModernCanvas src_canvas(64, 64);
        canvas.draw_surface(src_canvas, 100.0, 100.0);
        canvas.draw_surface_part(src_canvas, 0.0, 0.0, 32.0, 32.0, 200.0, 200.0, 2.0, 2.0, 0xffffff, 1.0);

        canvas.flush();
        check(s_draw_elements_count > 0, "primitives and surfaces flushed to GPU");

        // Pixel readback
        std::uint32_t px = canvas.pixel(10, 10);
        check(px == 0xff123456U, "pixel readback matched");

        std::vector<std::uint32_t> buf(640 * 480);
        check(canvas.readback(buf), "canvas readback succeeded");

        auto span = canvas.pixels();
        check(span.size() == 640 * 480, "canvas pixels span size");
        check(canvas.hash() != 0, "canvas hash non-zero");

        // Move semantics
        GLModernCanvas moved_canvas(std::move(canvas));
        check(moved_canvas.width() == 640, "moved canvas width 640");
        check(canvas.width() == 0, "moved-from canvas width 0");
    }

    {
        std::cout << "[Test] Testing GLModernAssets (Sprites, Backgrounds, Tiles, UVs)...\n";
        GLModernCanvas canvas(640, 480);
        GLModernAssets assets;
        GLModernAssets::set_active(&assets);

        std::vector<std::uint32_t> page_pixels(64 * 64, 0xffffffffU);
        int p0 = assets.add_page(64, 64, page_pixels);
        check(p0 == 0, "first texture page index 0");
        check(assets.page_texture(p0) != 0, "page texture allocated");

        GLModernAssets::Region r0{
            .x = 0, .y = 0, .w = 32, .h = 32,
            .tx = 0, .ty = 0, .tw = 32, .th = 32,
            .page = p0
        };
        int reg0 = assets.add_region(r0);
        check(reg0 == 0, "first region index 0");

        GLModernAssets::Sprite spr0{
            .name = "spr_player",
            .width = 32,
            .height = 32,
            .ox = 16,
            .oy = 16,
            .left = 0, .right = 31, .top = 0, .bottom = 31
        };
        int spr_id = assets.add_sprite(spr0, {static_cast<std::uint32_t>(reg0)});
        check(spr_id == 0, "first sprite id 0");
        check(assets.sprite_exists(0), "sprite 0 exists");
        check(!assets.sprite_exists(1), "sprite 1 does not exist");
        check(assets.sprite_count() == 1, "sprite count == 1");
        check(assets.sprite_name(0) == "spr_player", "sprite name matches");

        // Sprite drawing functions
        assets.draw_sprite(canvas, spr_id, 0.0, 100.0, 100.0);
        assets.draw_sprite_ext(canvas, spr_id, 0.0, 120.0, 120.0, 1.5, 1.5, 45.0, 0x00ff00, 0.8);
        assets.draw_sprite_part(canvas, spr_id, 0.0, 0.0, 0.0, 16.0, 16.0, 140.0, 140.0, 1.0, 1.0, 0xffffff, 1.0);
        assets.draw_sprite_pos(canvas, spr_id, 0.0, {10.0, 10.0, 40.0, 10.0, 40.0, 40.0, 10.0, 40.0}, 1.0);
        assets.draw_sprite_general(canvas, spr_id, 0.0, 0.0, 0.0, 16.0, 16.0, 200.0, 200.0, 1.0, 1.0, 30.0,
                                   {0xffffff, 0xff0000, 0x00ff00, 0x0000ff}, 1.0);
        assets.draw_sprite_stretched(canvas, spr_id, 0.0, 50.0, 50.0, 64.0, 64.0);

        // Backgrounds and tiles
        int bg_id = assets.add_background(reg0);
        check(bg_id == 0, "first background id 0");
        assets.draw_background(canvas, bg_id, 0.0, 0.0);
        assets.draw_background_stretched(canvas, bg_id, 0.0, 0.0, 128.0, 128.0);
        assets.draw_background_part_ext(canvas, bg_id, 0.0, 0.0, 16.0, 16.0, 10.0, 10.0, 1.0, 1.0, 0xffffff, 1.0);
        assets.draw_tile(canvas, bg_id, 0, 0, 16, 16, 0.0, 0.0, 1.0, 1.0, 0xffffff, 0);
        assets.draw_tile(canvas, bg_id, 0, 0, 16, 16, 20.0, 20.0, 1.0, 1.0, 0xffffff, 0x10000000U | 0x20000000U);

        // Standalone free functions
        regml::glmodern::draw_sprite(canvas, spr_id, 0.0, 10.0, 10.0);
        regml::glmodern::draw_sprite_ext(canvas, spr_id, 0.0, 20.0, 20.0, 1.0, 1.0, 0.0, 0xffffff, 1.0);
        regml::glmodern::draw_sprite_part(canvas, spr_id, 0.0, 0.0, 0.0, 16.0, 16.0, 30.0, 30.0, 1.0, 1.0, 0xffffff, 1.0);
        regml::glmodern::draw_sprite_pos(canvas, spr_id, 0.0, {0.0, 0.0, 32.0, 0.0, 32.0, 32.0, 0.0, 32.0}, 1.0);
        regml::glmodern::draw_sprite_general(canvas, spr_id, 0.0, 0.0, 0.0, 16.0, 16.0, 40.0, 40.0, 1.0, 1.0, 0.0,
                                             {0xffffff, 0xffffff, 0xffffff, 0xffffff}, 1.0);
        regml::glmodern::draw_sprite_stretched(canvas, spr_id, 0.0, 0.0, 0.0, 32.0, 32.0);
        regml::glmodern::draw_background(canvas, bg_id, 0.0, 0.0);
        regml::glmodern::draw_background_stretched(canvas, bg_id, 0.0, 0.0, 64.0, 64.0);
        regml::glmodern::draw_background_part_ext(canvas, bg_id, 0.0, 0.0, 16.0, 16.0, 0.0, 0.0, 1.0, 1.0, 0xffffff, 1.0);
        regml::glmodern::draw_tile(canvas, bg_id, 0, 0, 16, 16, 0.0, 0.0, 1.0, 1.0, 0xffffff, 0);

        // View transformation
        assets.view(10.0, 20.0, 2.0, 2.0, 5.0, 5.0);
        auto tp = assets.transform_point(15.0, 25.0);
        check(tp[0] == (15.0 - 10.0) * 2.0 + 5.0, "transform_point x");
        check(tp[1] == (25.0 - 20.0) * 2.0 + 5.0, "transform_point y");

        canvas.flush();
    }

    {
        std::cout << "[Test] Testing gl_gpu interface (Hardware backend layer)...\n";
        using namespace regml::glmodern;
        gl_gpu_begin();
        gl_gpu_set_viewport(640, 480);
        gl_gpu_box(10.0, 10.0, 50.0, 50.0, 0xff0000ffU);
        gl_gpu_circle(100.0, 100.0, 25.0, false, 0x00ff00ffU);
        gl_gpu_circle(100.0, 100.0, 25.0, true, 0x0000ffffU);
        gl_gpu_triangle(0.0, 0.0, 50.0, 50.0, 0.0, 50.0, 0xffffffffU);

        gl_gpu_box_batch_begin();
        gl_gpu_box(5.0, 5.0, 10.0, 10.0, 0x12345678U);
        gl_gpu_box_batch_end();

        std::vector<std::uint32_t> img(16 * 16, 0xaabbccddU);
        gl_gpu_image(img, 16, 0, 0, 16, 16, 10.0, 10.0, 16.0, 16.0, 0.0, 0xffffff, 1.0, 999ULL);
        gl_gpu_cached_image(16, 16, 30.0, 30.0, 16.0, 16.0, 45.0, 0xffffff, 0.7, 999ULL);
        gl_gpu_cached_sub_image(50.0, 50.0, 16.0, 16.0, 0.0f, 0.0f, 1.0f, 1.0f, 0xffffff, 1.0, 999ULL);

        // Test >1024 recursive tiling logic
        gl_gpu_image({}, 0, 0, 0, 2048, 2048, 0.0, 0.0, 2048.0, 2048.0, 0.0, 0xffffff, 1.0, 12345ULL);
        gl_gpu_image({}, 0, 0, 0, 2048, 2048, 0.0, 0.0, 0.0, 0.0, 0.0, 0xffffff, 1.0, 12345ULL);

        gl_gpu_flush();
        gl_gpu_presented();

        uint32_t px = gl_gpu_pixel(5, 5);
        (void)px;

        std::vector<std::uint32_t> rb(32 * 32);
        check(gl_gpu_readback(32, 32, rb), "gl_gpu_readback succeeded");
    }

    std::cout << "ALL MODERN OPENGL 3.3 TESTS PASSED!\n";
    return 0;
}
