#pragma once

#include "gl_common.hpp"
#include "gl_core.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace regml::glmodern {

class GLTexture {
public:
    GLTexture() noexcept = default;
    GLTexture(int width, int height, std::span<const std::uint32_t> pixels = {}, bool smooth = false);
    ~GLTexture();

    GLTexture(const GLTexture &) = delete;
    GLTexture &operator=(const GLTexture &) = delete;

    GLTexture(GLTexture &&other) noexcept;
    GLTexture &operator=(GLTexture &&other) noexcept;

    void create();
    void destroy() noexcept;

    void bind(unsigned int slot = 0) const noexcept;
    static void unbind(unsigned int slot = 0) noexcept;

    // Empty pixels span allocates uninitialized storage
    void upload(int width, int height, std::span<const std::uint32_t> pixels, bool smooth = false);
    void upload_sub(int x, int y, int width, int height, std::span<const std::uint32_t> pixels);

    void set_filter(bool smooth);
    void set_wrap(bool clamp_to_edge = true);

    [[nodiscard]] GLuint id() const noexcept { return id_; }
    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }
    [[nodiscard]] bool smooth() const noexcept { return smooth_; }
    [[nodiscard]] bool valid() const noexcept { return id_ != 0; }
    [[nodiscard]] std::size_t bytes() const noexcept {
        return static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) * 4U;
    }

private:
    GLuint id_ = 0;
    int width_ = 0;
    int height_ = 0;
    bool smooth_ = false;
};

class TextureCache {
public:
    // 24 MB cap on Wii U for MEM2 heap limits
#if REGML_WIIU
    static constexpr std::size_t DEFAULT_BUDGET = 64 * 1024 * 1024;
#else
    static constexpr std::size_t DEFAULT_BUDGET = 128 * 1024 * 1024;
#endif

    explicit TextureCache(std::size_t budget = DEFAULT_BUDGET) noexcept
        : budget_(budget) {}

    ~TextureCache() { clear(); }

    TextureCache(const TextureCache &) = delete;
    TextureCache &operator=(const TextureCache &) = delete;

    TextureCache(TextureCache &&other) noexcept = default;
    TextureCache &operator=(TextureCache &&other) noexcept = default;

    GLuint get_or_upload(std::uint64_t key,
                         std::span<const std::uint32_t> pixels,
                         int width,
                         int height,
                         bool smooth = false);

    [[nodiscard]] GLuint get(std::uint64_t key) noexcept;
    [[nodiscard]] bool contains(std::uint64_t key) const noexcept {
        return cache_.find(key) != cache_.end();
    }

    [[nodiscard]] GLTexture *find(std::uint64_t key) noexcept;
    [[nodiscard]] const GLTexture *find(std::uint64_t key) const noexcept;

    bool touch(std::uint64_t key) noexcept;
    void clear() noexcept;

    void set_budget(std::size_t bytes) noexcept { budget_ = bytes; }
    [[nodiscard]] std::size_t budget() const noexcept { return budget_; }
    [[nodiscard]] std::size_t cache_bytes() const noexcept { return cache_bytes_; }
    [[nodiscard]] std::size_t size() const noexcept { return cache_.size(); }

private:
    struct Entry {
        GLTexture texture;
        std::size_t last_used = 0;
    };

    void evict_to_budget(std::size_t needed_bytes);

    std::unordered_map<std::uint64_t, Entry> cache_;
    std::size_t budget_ = DEFAULT_BUDGET;
    std::size_t cache_bytes_ = 0;
    std::size_t clock_ = 0;
};

} // namespace regml::glmodern
