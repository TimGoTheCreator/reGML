#pragma once

#include "gl_common.hpp"
#include "gl_core.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace regml::glmodern {

class GLFramebuffer {
public:
    GLFramebuffer() noexcept = default;
    GLFramebuffer(int width, int height, bool smooth = false);
    ~GLFramebuffer();

    GLFramebuffer(const GLFramebuffer &) = delete;
    GLFramebuffer &operator=(const GLFramebuffer &) = delete;

    GLFramebuffer(GLFramebuffer &&other) noexcept;
    GLFramebuffer &operator=(GLFramebuffer &&other) noexcept;

    bool init(int width, int height, bool smooth = false);
    void destroy() noexcept;

    void bind(bool set_viewport = true) const noexcept;
    static void unbind() noexcept;
    void set_viewport() const noexcept;

    bool readback(std::span<std::uint32_t> out_pixels) const noexcept;
    [[nodiscard]] std::uint32_t pixel(int x, int y) const noexcept;

    [[nodiscard]] GLuint fbo_id() const noexcept { return fbo_id_; }
    [[nodiscard]] GLuint id() const noexcept { return fbo_id_; }
    [[nodiscard]] GLuint texture_id() const noexcept { return texture_id_; }
    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }
    [[nodiscard]] bool valid() const noexcept { return fbo_id_ != 0 && texture_id_ != 0; }
    [[nodiscard]] bool is_complete() const noexcept;

private:
    GLuint fbo_id_ = 0;
    GLuint texture_id_ = 0;
    int width_ = 0;
    int height_ = 0;
    bool smooth_ = false;
};

} // namespace regml::glmodern
