#include "texture.hpp"

#include <algorithm>
#include <utility>

namespace regml::glmodern {

GLTexture::GLTexture(int width, int height, std::span<const std::uint32_t> pixels, bool smooth) {
    upload(width, height, pixels, smooth);
}

GLTexture::~GLTexture() {
    destroy();
}

GLTexture::GLTexture(GLTexture &&other) noexcept
    : id_(other.id_),
      width_(other.width_),
      height_(other.height_),
      smooth_(other.smooth_) {
    other.id_ = 0;
    other.width_ = 0;
    other.height_ = 0;
    other.smooth_ = false;
}

GLTexture &GLTexture::operator=(GLTexture &&other) noexcept {
    if (this != &other) {
        destroy();
        id_ = other.id_;
        width_ = other.width_;
        height_ = other.height_;
        smooth_ = other.smooth_;
        other.id_ = 0;
        other.width_ = 0;
        other.height_ = 0;
        other.smooth_ = false;
    }
    return *this;
}

void GLTexture::create() {
    if (id_ == 0 && glGenTextures) {
        glGenTextures(1, &id_);
    }
}

void GLTexture::destroy() noexcept {
    if (id_ != 0) {
        if (glDeleteTextures) {
            glDeleteTextures(1, &id_);
        }
        id_ = 0;
        width_ = 0;
        height_ = 0;
        smooth_ = false;
    }
}

void GLTexture::bind(unsigned int slot) const noexcept {
    if (glActiveTexture) {
        glActiveTexture(GL_TEXTURE0 + slot);
    }
    if (glBindTexture) {
        glBindTexture(GL_TEXTURE_2D, id_);
    }
}

void GLTexture::unbind(unsigned int slot) noexcept {
    if (glActiveTexture) {
        glActiveTexture(GL_TEXTURE0 + slot);
    }
    if (glBindTexture) {
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

void GLTexture::upload(int width, int height, std::span<const std::uint32_t> pixels, bool smooth) {
    if (width <= 0 || height <= 0) {
        destroy();
        return;
    }

    create();
    bind(0);

    set_wrap(true);
    set_filter(smooth);

    if (glPixelStorei) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    }

    if (glTexImage2D) {
        const void *data = pixels.empty() ? nullptr : pixels.data();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    }

    if (glGetError && glGetError() != GL_NO_ERROR) {
        destroy();
        return;
    }

    width_ = width;
    height_ = height;
    smooth_ = smooth;
}

void GLTexture::upload_sub(int x, int y, int width, int height, std::span<const std::uint32_t> pixels) {
    if (id_ == 0 || pixels.empty() || width <= 0 || height <= 0) {
        return;
    }

    bind(0);

    if (glPixelStorei) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    }

    if (glTexSubImage2D) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    }
}

void GLTexture::set_filter(bool smooth) {
    smooth_ = smooth;
    if (id_ != 0 && glTexParameteri) {
        bind(0);
        const GLint filter = smooth ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    }
}

void GLTexture::set_wrap(bool clamp_to_edge) {
    if (id_ != 0 && glTexParameteri) {
        bind(0);
        const GLint wrap = clamp_to_edge ? GL_CLAMP_TO_EDGE : GL_REPEAT;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    }
}

void TextureCache::evict_to_budget(std::size_t needed_bytes) {
    while (cache_bytes_ + needed_bytes > budget_ && !cache_.empty()) {
        auto oldest = std::min_element(cache_.begin(), cache_.end(), [](const auto &a, const auto &b) {
            return a.second.last_used < b.second.last_used;
        });
        if (oldest != cache_.end()) {
            cache_bytes_ -= oldest->second.texture.bytes();
            cache_.erase(oldest);
        }
    }
}

GLuint TextureCache::get_or_upload(std::uint64_t key,
                                   std::span<const std::uint32_t> pixels,
                                   int width,
                                   int height,
                                   bool smooth) {
    const std::uint64_t actual_key = key ? key : (static_cast<std::uint64_t>(++clock_) | (1ULL << 63));

    auto it = cache_.find(actual_key);
    if (it != cache_.end()) {
        it->second.last_used = ++clock_;
        return it->second.texture.id();
    }

    const auto requested_bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
    evict_to_budget(requested_bytes);

    GLTexture tex;
    tex.upload(width, height, pixels, smooth);
    const GLuint id = tex.id();
    if (id == 0) {
        return 0;
    }
    cache_bytes_ += tex.bytes();
    cache_.emplace(actual_key, Entry{std::move(tex), ++clock_});

    return id;
}

GLuint TextureCache::get(std::uint64_t key) noexcept {
    auto it = cache_.find(key);
    if (it == cache_.end()) {
        return 0;
    }
    it->second.last_used = ++clock_;
    return it->second.texture.id();
}

GLTexture *TextureCache::find(std::uint64_t key) noexcept {
    auto it = cache_.find(key);
    if (it == cache_.end()) {
        return nullptr;
    }
    return &it->second.texture;
}

const GLTexture *TextureCache::find(std::uint64_t key) const noexcept {
    auto it = cache_.find(key);
    if (it == cache_.end()) {
        return nullptr;
    }
    return &it->second.texture;
}

bool TextureCache::touch(std::uint64_t key) noexcept {
    auto it = cache_.find(key);
    if (it == cache_.end()) {
        return false;
    }
    it->second.last_used = ++clock_;
    return true;
}

void TextureCache::clear() noexcept {
    cache_.clear();
    cache_bytes_ = 0;
    clock_ = 0;
}

} // namespace regml::glmodern
