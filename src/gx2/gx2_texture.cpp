#include "gx2_texture.hpp"

#ifdef __WIIU__

#include <cstdlib>
#include <cstring>
#include <malloc.h>

namespace cooolio::gx2 {

namespace {

void copy_rgba8_to_surface(const GX2Surface& surface, const void* src_pixels, uint32_t width, uint32_t height) {
    const uint32_t src_stride = width * 4u;
    const uint32_t dst_stride = surface.pitch * 4u;
    const auto* src = static_cast<const uint8_t*>(src_pixels);
    auto* dst = static_cast<uint8_t*>(surface.image);

    if (dst_stride == src_stride) {
        std::memcpy(dst, src, static_cast<std::size_t>(src_stride) * height);
    } else {
        for (uint32_t y = 0; y < height; ++y) {
            std::memcpy(dst + static_cast<std::size_t>(y) * dst_stride,
                        src + static_cast<std::size_t>(y) * src_stride,
                        src_stride);
        }
    }
}

} // namespace

bool create_texture_rgba8(TextureEntry& out, uint32_t width, uint32_t height, const void* rgba_pixels) {
    destroy_texture(out);
    if (width == 0 || height == 0 || !rgba_pixels) {
        return false;
    }

    GX2Texture& tex = out.texture;
    std::memset(&tex, 0, sizeof(tex));
    tex.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    tex.surface.width = width;
    tex.surface.height = height;
    tex.surface.depth = 1;
    tex.surface.mipLevels = 1;
    tex.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    tex.surface.aa = GX2_AA_MODE1X;
    tex.surface.use = GX2_SURFACE_USE_TEXTURE;
    tex.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    tex.viewFirstMip = 0;
    tex.viewNumMips = 1;
    tex.viewFirstSlice = 0;
    tex.viewNumSlices = 1;
    tex.compMap = 0x00010203u;

    GX2CalcSurfaceSizeAndAlignment(&tex.surface);
    GX2InitTextureRegs(&tex);

    if (tex.surface.imageSize == 0) {
        return false;
    }

    const std::size_t align = tex.surface.alignment ? tex.surface.alignment : 64u;
    void* mem = memalign(align, tex.surface.imageSize);
    if (!mem) {
        return false;
    }

    std::memset(mem, 0, tex.surface.imageSize);
    tex.surface.image = mem;
    copy_rgba8_to_surface(tex.surface, rgba_pixels, width, height);

    DCFlushRange(tex.surface.image, tex.surface.imageSize);
    GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, tex.surface.image, tex.surface.imageSize);

    out.image_data = mem;
    out.byte_size = tex.surface.imageSize;
    return true;
}

void destroy_texture(TextureEntry& entry) {
    if (entry.image_data) {
        std::free(entry.image_data);
        entry.image_data = nullptr;
    }
    std::memset(&entry.texture, 0, sizeof(entry.texture));
    entry.byte_size = 0;
}

bool create_dummy_white_texture(TextureEntry& out) {
    static constexpr uint8_t kWhitePixel[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    return create_texture_rgba8(out, 1, 1, kWhitePixel);
}

void init_sampler(GX2Sampler& sampler, bool linear_filter, bool clamp_edge) {
    std::memset(&sampler, 0, sizeof(sampler));
    GX2InitSampler(&sampler,
                   clamp_edge ? GX2_TEX_CLAMP_MODE_CLAMP : GX2_TEX_CLAMP_MODE_WRAP,
                   linear_filter ? GX2_TEX_XY_FILTER_MODE_LINEAR : GX2_TEX_XY_FILTER_MODE_POINT);
}

bool create_color_buffer(ColorBufferTarget& out, uint32_t width, uint32_t height, GX2SurfaceFormat format) {
    destroy_color_buffer(out);
    if (width == 0 || height == 0) {
        return false;
    }

    GX2ColorBuffer& cb = out.color_buffer;
    std::memset(&cb, 0, sizeof(cb));
    cb.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    cb.surface.width = width;
    cb.surface.height = height;
    cb.surface.depth = 1;
    cb.surface.mipLevels = 1;
    cb.surface.format = format;
    cb.surface.aa = GX2_AA_MODE1X;
    cb.surface.use = GX2_SURFACE_USE_TEXTURE_COLOR_BUFFER_TV;
    cb.surface.tileMode = GX2_TILE_MODE_DEFAULT;
    cb.viewMip = 0;
    cb.viewFirstSlice = 0;
    cb.viewNumSlices = 1;

    GX2CalcSurfaceSizeAndAlignment(&cb.surface);
    GX2InitColorBufferRegs(&cb);

    if (cb.surface.imageSize == 0) {
        return false;
    }

    const std::size_t align = cb.surface.alignment ? cb.surface.alignment : 64u;
    void* mem = memalign(align, cb.surface.imageSize);
    if (!mem) {
        return false;
    }

    std::memset(mem, 0, cb.surface.imageSize);
    cb.surface.image = mem;

    DCFlushRange(cb.surface.image, cb.surface.imageSize);
    GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, cb.surface.image, cb.surface.imageSize);

    out.image_data = mem;
    out.byte_size = cb.surface.imageSize;
    return true;
}

void destroy_color_buffer(ColorBufferTarget& target) {
    if (target.image_data) {
        std::free(target.image_data);
        target.image_data = nullptr;
    }
    std::memset(&target.color_buffer, 0, sizeof(target.color_buffer));
    target.byte_size = 0;
}

void invalidate_color_buffer(const ColorBufferTarget& target) {
    if (target.image_data && target.byte_size > 0) {
        GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, target.image_data, target.byte_size);
    }
}

GX2TextureCache::GX2TextureCache(std::size_t budget_bytes)
    : budget_bytes_(budget_bytes) {}

GX2TextureCache::~GX2TextureCache() {
    clear();
}

const GX2Texture* GX2TextureCache::find(uint64_t key) {
    auto it = index_.find(key);
    if (it == index_.end()) {
        return nullptr;
    }
    lru_.splice(lru_.begin(), lru_, it->second);
    return &it->second->entry.texture;
}

const GX2Texture* GX2TextureCache::get_or_upload(uint64_t key, uint32_t width, uint32_t height,
                                                 const void* rgba_pixels) {
    if (const GX2Texture* cached = find(key)) {
        return cached;
    }

    TextureEntry entry;
    if (!create_texture_rgba8(entry, width, height, rgba_pixels)) {
        return nullptr;
    }

    if (entry.byte_size > budget_bytes_) {
        destroy_texture(entry);
        return nullptr;
    }

    evict_until_fits(entry.byte_size);

    lru_.push_front(CacheNode{key, entry});
    // Ownership of image_data moved into lru_.front().entry.
    entry.image_data = nullptr;
    index_[key] = lru_.begin();
    used_bytes_ += lru_.front().entry.byte_size;

    return &lru_.front().entry.texture;
}

void GX2TextureCache::remove(uint64_t key) {
    auto it = index_.find(key);
    if (it == index_.end()) {
        return;
    }
    used_bytes_ -= it->second->entry.byte_size;
    destroy_texture(it->second->entry);
    lru_.erase(it->second);
    index_.erase(it);
}

void GX2TextureCache::clear() {
    for (auto& node : lru_) {
        destroy_texture(node.entry);
    }
    lru_.clear();
    index_.clear();
    used_bytes_ = 0;
}

void GX2TextureCache::evict_until_fits(std::size_t incoming_bytes) {
    while (!lru_.empty() && (used_bytes_ + incoming_bytes > budget_bytes_)) {
        auto& back = lru_.back();
        used_bytes_ -= back.entry.byte_size;
        index_.erase(back.key);
        destroy_texture(back.entry);
        lru_.pop_back();
    }
}

} // namespace cooolio::gx2

#endif // __WIIU__
