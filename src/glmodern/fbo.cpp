#include "fbo.hpp"

#include <algorithm>
#include <utility>

namespace regml::glmodern {

GLFramebuffer::GLFramebuffer(int width, int height, bool smooth) {
    init(width, height, smooth);
}

GLFramebuffer::~GLFramebuffer() {
    destroy();
}

GLFramebuffer::GLFramebuffer(GLFramebuffer &&other) noexcept
    : fbo_id_(other.fbo_id_),
      texture_id_(other.texture_id_),
      width_(other.width_),
      height_(other.height_),
      smooth_(other.smooth_) {
    other.fbo_id_ = 0;
    other.texture_id_ = 0;
    other.width_ = 0;
    other.height_ = 0;
    other.smooth_ = false;
}

GLFramebuffer &GLFramebuffer::operator=(GLFramebuffer &&other) noexcept {
    if (this != &other) {
        destroy();
        fbo_id_ = other.fbo_id_;
        texture_id_ = other.texture_id_;
        width_ = other.width_;
        height_ = other.height_;
        smooth_ = other.smooth_;

        other.fbo_id_ = 0;
        other.texture_id_ = 0;
        other.width_ = 0;
        other.height_ = 0;
        other.smooth_ = false;
    }
    return *this;
}

bool GLFramebuffer::init(int width, int height, bool smooth) {
    destroy();

    if (width <= 0 || height <= 0) {
        return false;
    }

    load_gl_core_3_3();
    if (!glGenFramebuffers || !glGenTextures || !glBindFramebuffer || !glBindTexture ||
        !glTexImage2D || !glTexParameteri || !glFramebufferTexture2D || !glCheckFramebufferStatus) {
        return false;
    }

    width_ = width;
    height_ = height;
    smooth_ = smooth;

    glGenTextures(1, &texture_id_);
    if (!texture_id_) {
        destroy();
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, texture_id_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, smooth_ ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, smooth_ ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &fbo_id_);
    if (!fbo_id_) {
        destroy();
        return false;
    }

    GLint prev_fbo = 0;
    if (glGetIntegerv) {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_id_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_id_, 0);

    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        destroy();
        return false;
    }

    return true;
}

void GLFramebuffer::destroy() noexcept {
    if (texture_id_ != 0) {
        if (glDeleteTextures) {
            glDeleteTextures(1, &texture_id_);
        }
        texture_id_ = 0;
    }

    if (fbo_id_ != 0) {
        if (glDeleteFramebuffers) {
            glDeleteFramebuffers(1, &fbo_id_);
        }
        fbo_id_ = 0;
    }

    width_ = 0;
    height_ = 0;
    smooth_ = false;
}

void GLFramebuffer::bind(bool set_viewport) const noexcept {
    if (fbo_id_ != 0 && glBindFramebuffer) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_id_);
        if (set_viewport && glViewport && width_ > 0 && height_ > 0) {
            glViewport(0, 0, width_, height_);
        }
    }
}

void GLFramebuffer::unbind() noexcept {
    if (glBindFramebuffer) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}

void GLFramebuffer::set_viewport() const noexcept {
    if (glViewport && width_ > 0 && height_ > 0) {
        glViewport(0, 0, width_, height_);
    }
}

bool GLFramebuffer::is_complete() const noexcept {
    if (fbo_id_ == 0 || !glCheckFramebufferStatus || !glBindFramebuffer) {
        return false;
    }

    GLint prev_fbo = 0;
    if (glGetIntegerv) {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_id_);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));

    return status == GL_FRAMEBUFFER_COMPLETE;
}

bool GLFramebuffer::readback(std::span<std::uint32_t> out_pixels) const noexcept {
    if (!valid() || out_pixels.size() < static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_)) {
        return false;
    }

    if (!glReadPixels || !glBindFramebuffer) {
        return false;
    }

    GLint prev_fbo = 0;
    if (glGetIntegerv) {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_id_);

    if (glPixelStorei) {
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
    }

    // glReadPixels is bottom-up; flip rows to match top-down Canvas layout
    std::vector<std::uint32_t> temp(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_));
    glReadPixels(0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, temp.data());

    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));

    for (int y = 0; y < height_; ++y) {
        const auto *src_row = &temp[static_cast<std::size_t>(height_ - 1 - y) * width_];
        auto *dst_row = &out_pixels[static_cast<std::size_t>(y) * width_];
        std::copy_n(src_row, width_, dst_row);
    }

    return true;
}

std::uint32_t GLFramebuffer::pixel(int x, int y) const noexcept {
    if (!valid() || x < 0 || y < 0 || x >= width_ || y >= height_ || !glReadPixels || !glBindFramebuffer) {
        return 0;
    }

    GLint prev_fbo = 0;
    if (glGetIntegerv) {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_id_);

    if (glPixelStorei) {
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
    }

    std::uint32_t result = 0;
    glReadPixels(x, height_ - 1 - y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &result);

    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
    return result;
}

} // namespace regml::glmodern
