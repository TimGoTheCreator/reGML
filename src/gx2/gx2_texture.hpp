#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>

#ifdef __WIIU__
#include <coreinit/cache.h>
#include <gx2/mem.h>
#include <gx2/sampler.h>
#include <gx2/surface.h>
#include <gx2/texture.h>
#endif

namespace cooolio::gx2 {

#ifdef __WIIU__

struct TextureEntry {
    GX2Texture texture{};
    void* image_data = nullptr;
    std::size_t byte_size = 0;
};

struct ColorBufferTarget {
    GX2ColorBuffer color_buffer{};
    void* image_data = nullptr;
    std::size_t byte_size = 0;
};

// Upload RGBA8 pixel data into a GX2Texture (allocates aligned surface, copies rows, flushes & invalidates).
bool create_texture_rgba8(TextureEntry& out, uint32_t width, uint32_t height, const void* rgba_pixels);
void destroy_texture(TextureEntry& entry);

// 1x1 opaque white dummy texture for untextured draws.
bool create_dummy_white_texture(TextureEntry& out);

// Sampler setup for nearest/linear filtering and clamp/wrap addressing.
void init_sampler(GX2Sampler& sampler, bool linear_filter, bool clamp_edge = true);

// Offscreen GX2ColorBuffer management.
bool create_color_buffer(ColorBufferTarget& out, uint32_t width, uint32_t height,
                         GX2SurfaceFormat format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8);
void destroy_color_buffer(ColorBufferTarget& target);
void invalidate_color_buffer(const ColorBufferTarget& target);

class GX2TextureCache {
public:
    static constexpr std::size_t kDefaultBudgetBytes = 64u * 1024u * 1024u;

    explicit GX2TextureCache(std::size_t budget_bytes = kDefaultBudgetBytes);
    ~GX2TextureCache();

    GX2TextureCache(const GX2TextureCache&) = delete;
    GX2TextureCache& operator=(const GX2TextureCache&) = delete;

    // Returns cached texture if present (updating LRU), otherwise uploads and caches it.
    const GX2Texture* get_or_upload(uint64_t key, uint32_t width, uint32_t height, const void* rgba_pixels);

    // Returns cached texture if present (updating LRU), or nullptr.
    const GX2Texture* find(uint64_t key);

    void remove(uint64_t key);
    void clear();

    std::size_t used_bytes() const { return used_bytes_; }
    std::size_t budget_bytes() const { return budget_bytes_; }

private:
    struct CacheNode {
        uint64_t key = 0;
        TextureEntry entry{};
    };

    void evict_until_fits(std::size_t incoming_bytes);

    std::size_t budget_bytes_ = kDefaultBudgetBytes;
    std::size_t used_bytes_ = 0;
    std::list<CacheNode> lru_;
    std::unordered_map<uint64_t, std::list<CacheNode>::iterator> index_;
};

#endif // __WIIU__

} // namespace cooolio::gx2
