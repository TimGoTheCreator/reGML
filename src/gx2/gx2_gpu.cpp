#include "gx2_gpu.hpp"
#include "gx2_batcher.hpp"

#include <coreinit/cache.h>
#include <gx2/clear.h>
#include <gx2/context.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/surface.h>
#include <gx2/texture.h>
#include <whb/gfx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <memory>
#include <unordered_map>
#include <vector>

namespace regml::gx2 {
namespace {

struct CachedTexture {
    GX2Texture tex{};
    std::uint8_t *pixels = nullptr;
    int width = 0;
    int height = 0;
    std::size_t last_frame = 0;
    std::uint32_t id = 0;

    CachedTexture() = default;
    ~CachedTexture() {
        if (pixels) std::free(pixels);
    }
    CachedTexture(const CachedTexture &) = delete;
    CachedTexture &operator=(const CachedTexture &) = delete;
};

struct Gx2Surface {
    GX2ColorBuffer color_buffer{};
    GX2Texture texture{};
    std::uint8_t *image = nullptr;
    int width = 0;
    int height = 0;
};

Gx2Batcher s_batcher;
std::unordered_map<std::uint64_t, std::unique_ptr<CachedTexture>> s_texture_cache;
std::vector<std::unique_ptr<CachedTexture>> s_transient_textures;
std::vector<std::unique_ptr<CachedTexture>> s_retired_transient_textures;
std::unordered_map<std::uint32_t, std::unique_ptr<Gx2Surface>> s_surfaces;

std::size_t s_current_frame = 0;
std::size_t s_flush_draw_calls = 0;
std::size_t s_frame_draw_calls = 0;
std::uint32_t s_last_texture_id = 0;
std::uint64_t s_last_lookup_key = 0;
CachedTexture *s_last_lookup_tex = nullptr;
std::uint32_t s_next_texture_id = 1;
std::uint32_t s_next_surface_id = 1;
std::uint32_t s_bound_surface_id = 0;
std::vector<std::uint32_t> s_textures_used;

bool s_whbgfx_initialized = false;
bool s_initialized = false;
bool s_in_render = false;
bool s_dirty = false;
bool s_presented = false;

int s_viewport_width = 1280;
int s_viewport_height = 720;
int s_main_width = 1280;
int s_main_height = 720;

bool s_box_batch_active = false;

constexpr int CIRCLE_SEGMENTS = 64;

const auto &unit_circle() {
    static const auto unit = [] {
        std::array<std::array<float, 2>, CIRCLE_SEGMENTS + 1> pts{};
        for (int i = 0; i < CIRCLE_SEGMENTS; ++i) {
            const double a = i * 6.28318530717958647692 / CIRCLE_SEGMENTS;
            pts[i] = {static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))};
        }
        pts[CIRCLE_SEGMENTS] = pts[0];
        return pts;
    }();
    return unit;
}

[[nodiscard]] constexpr std::uint32_t unpack_rgba(std::uint32_t rgba) noexcept {
    const auto r = static_cast<std::uint8_t>((rgba >> 16) & 0xffU);
    const auto g = static_cast<std::uint8_t>((rgba >> 8) & 0xffU);
    const auto b = static_cast<std::uint8_t>(rgba & 0xffU);
    const auto a = static_cast<std::uint8_t>((rgba >> 24) & 0xffU);
    return (static_cast<std::uint32_t>(r) << 24) |
           (static_cast<std::uint32_t>(g) << 16) |
           (static_cast<std::uint32_t>(b) << 8) |
           static_cast<std::uint32_t>(a);
}

[[nodiscard]] constexpr std::uint32_t tint_opacity_to_rgba(unsigned tint, double opacity) noexcept {
    const auto a = static_cast<std::uint8_t>(std::clamp(opacity, 0.0, 1.0) * 255.0 + 0.5);
    const auto r = static_cast<std::uint8_t>((tint >> 16) & 0xffU);
    const auto g = static_cast<std::uint8_t>((tint >> 8) & 0xffU);
    const auto b = static_cast<std::uint8_t>(tint & 0xffU);
    return (static_cast<std::uint32_t>(r) << 24) |
           (static_cast<std::uint32_t>(g) << 16) |
           (static_cast<std::uint32_t>(b) << 8) |
           static_cast<std::uint32_t>(a);
}

bool ensure_initialized() {
    if (s_initialized) return true;
    if (!s_whbgfx_initialized) {
        if (!WHBGfxInit()) {
            return false;
        }
        s_whbgfx_initialized = true;
    }
    s_initialized = s_batcher.init();
    if (s_initialized) {
        s_texture_cache.reserve(256);
    }
    return s_initialized;
}

void set_tv_viewport_and_scissor() {
    float tv_w = 1280.0f;
    float tv_h = 720.0f;
    if (GX2ColorBuffer *tv = WHBGfxGetTVColourBuffer()) {
        tv_w = static_cast<float>(tv->surface.width);
        tv_h = static_cast<float>(tv->surface.height);
    }
    const float logical_w = static_cast<float>(std::max(1, s_viewport_width));
    const float logical_h = static_cast<float>(std::max(1, s_viewport_height));
    const float scale = std::min(tv_w / logical_w, tv_h / logical_h);
    const float vp_w = logical_w * scale;
    const float vp_h = logical_h * scale;
    const float vp_x = (tv_w - vp_w) * 0.5f;
    const float vp_y = (tv_h - vp_h) * 0.5f;
    GX2SetViewport(vp_x, vp_y, vp_w, vp_h, 0.0f, 1.0f);
    GX2SetScissor(static_cast<std::uint32_t>(vp_x),
                  static_cast<std::uint32_t>(vp_y),
                  static_cast<std::uint32_t>(vp_w),
                  static_cast<std::uint32_t>(vp_h));
}

void ensure_batch_started() {
    if (!ensure_initialized()) return;
    if (!s_batcher.is_active()) {
        const auto proj = Gx2Mat4::ortho(0.0f,
                                         static_cast<float>(s_viewport_width),
                                         static_cast<float>(s_viewport_height),
                                         0.0f);
        s_batcher.begin(proj);
    }
}

CachedTexture *upload_texture(std::uint64_t key,
                              std::span<const std::uint32_t> pixels,
                              int stride,
                              int left,
                              int top,
                              int width,
                              int height) {
    if (width <= 0 || height <= 0 || pixels.empty()) return nullptr;

    auto it = s_texture_cache.find(key);
    if (it != s_texture_cache.end() && it->second->width == width && it->second->height == height) {
        it->second->last_frame = s_current_frame;
        return it->second.get();
    }

    auto entry = std::make_unique<CachedTexture>();
    entry->width = width;
    entry->height = height;
    entry->last_frame = s_current_frame;
    entry->id = s_next_texture_id++;

    auto &tex = entry->tex;
    std::memset(&tex, 0, sizeof(tex));
    tex.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    tex.surface.width = static_cast<std::uint32_t>(width);
    tex.surface.height = static_cast<std::uint32_t>(height);
    tex.surface.depth = 1;
    tex.surface.mipLevels = 1;
    tex.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    tex.surface.use = GX2_SURFACE_USE_TEXTURE;
    tex.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    tex.viewNumMips = 1;
    tex.viewNumSlices = 1;
    tex.compMap = 0x01020300u;
    GX2CalcSurfaceSizeAndAlignment(&tex.surface);
    GX2InitTextureRegs(&tex);
    if (tex.surface.imageSize == 0) return nullptr;

    const std::size_t align = tex.surface.alignment ? tex.surface.alignment : 64u;
    const std::size_t alloc_size = ((tex.surface.imageSize + align - 1u) / align) * align;
    entry->pixels = static_cast<std::uint8_t *>(memalign(align, alloc_size));
    if (!entry->pixels) return nullptr;

    const std::uint32_t pitch = tex.surface.pitch;
    auto *dst_words = reinterpret_cast<std::uint32_t *>(entry->pixels);
    if (left == 0 && top == 0 && width == stride && static_cast<std::uint32_t>(width) == pitch &&
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) <= pixels.size()) {
        std::memcpy(dst_words, pixels.data(),
                    static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * sizeof(std::uint32_t));
    } else {
        for (int row = 0; row < height; ++row) {
            const std::size_t src_off = static_cast<std::size_t>(top + row) * static_cast<std::size_t>(stride) +
                                        static_cast<std::size_t>(left);
            if (src_off + static_cast<std::size_t>(width) <= pixels.size()) {
                std::memcpy(dst_words + static_cast<std::size_t>(row) * pitch,
                            pixels.data() + src_off,
                            static_cast<std::size_t>(width) * sizeof(std::uint32_t));
            }
        }
    }

    tex.surface.image = entry->pixels;
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, entry->pixels, tex.surface.imageSize);

    CachedTexture *ptr = entry.get();
    if (key != 0) {
        s_texture_cache[key] = std::move(entry);
    } else {
        s_transient_textures.push_back(std::move(entry));
    }
    return ptr;
}

} // namespace

void gx2_gpu_begin() {
    if (!ensure_initialized()) return;
    if (!s_in_render) {
        s_retired_transient_textures.clear();
        s_retired_transient_textures.swap(s_transient_textures);
        WHBGfxBeginRender();
        WHBGfxBeginRenderTV();
        WHBGfxClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        set_tv_viewport_and_scissor();
        s_in_render = true;
    }
    s_dirty = false;
    s_presented = false;
    s_flush_draw_calls = 0;
    s_frame_draw_calls = 0;
    s_textures_used.clear();
    s_batcher.reset_draw_call_count();
}

void gx2_gpu_flush() {
    if (!s_initialized) return;
    if (s_batcher.is_active()) {
        s_batcher.flush();
    }
    s_flush_draw_calls = s_batcher.draw_call_count();
    s_frame_draw_calls += s_flush_draw_calls;
    s_batcher.reset_draw_call_count();
}

void gx2_gpu_presented() {
    gx2_gpu_flush();
    if (s_batcher.is_active()) {
        s_batcher.end();
    }
    if (s_in_render) {
        WHBGfxFinishRenderTV();
        static bool s_drc_cleared = false;
        if (!s_drc_cleared) {
            WHBGfxBeginRenderDRC();
            WHBGfxClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            WHBGfxFinishRenderDRC();
            s_drc_cleared = true;
        }
        WHBGfxFinishRender();
        s_in_render = false;
    }
    s_presented = true;
}

void gx2_gpu_box(double x, double y, double w, double h, std::uint32_t rgba) {
    if (w <= 0.0 || h <= 0.0) return;
    ensure_batch_started();
    s_batcher.draw_quad(nullptr,
                        static_cast<float>(x),
                        static_cast<float>(y),
                        static_cast<float>(w),
                        static_cast<float>(h),
                        0.0f, 0.0f, 1.0f, 1.0f,
                        unpack_rgba(rgba));
    s_dirty = true;
}

void gx2_gpu_circle(double x, double y, double r, bool outline, std::uint32_t rgba) {
    if (r <= 0.0) return;
    ensure_batch_started();
    const std::uint32_t col = unpack_rgba(rgba);
    const auto &pts = unit_circle();
    const float cx = static_cast<float>(x);
    const float cy = static_cast<float>(y);
    const float rf = static_cast<float>(r);

    if (!outline) {
        for (int i = 0; i < CIRCLE_SEGMENTS; ++i) {
            s_batcher.draw_triangle(cx, cy,
                                    cx + pts[i][0] * rf, cy + pts[i][1] * rf,
                                    cx + pts[i + 1][0] * rf, cy + pts[i + 1][1] * rf,
                                    col);
        }
    } else {
        const float inner = std::max(0.0f, rf - 1.5f);
        for (int i = 0; i < CIRCLE_SEGMENTS; ++i) {
            const float x0o = cx + pts[i][0] * rf;
            const float y0o = cy + pts[i][1] * rf;
            const float x1o = cx + pts[i + 1][0] * rf;
            const float y1o = cy + pts[i + 1][1] * rf;
            const float x0i = cx + pts[i][0] * inner;
            const float y0i = cy + pts[i][1] * inner;
            const float x1i = cx + pts[i + 1][0] * inner;
            const float y1i = cy + pts[i + 1][1] * inner;
            s_batcher.draw_triangle(x0o, y0o, x1o, y1o, x1i, y1i, col);
            s_batcher.draw_triangle(x0o, y0o, x1i, y1i, x0i, y0i, col);
        }
    }
    s_dirty = true;
}

void gx2_gpu_triangle(double x1, double y1, double x2, double y2, double x3, double y3, std::uint32_t rgba) {
    ensure_batch_started();
    s_batcher.draw_triangle(static_cast<float>(x1), static_cast<float>(y1),
                            static_cast<float>(x2), static_cast<float>(y2),
                            static_cast<float>(x3), static_cast<float>(y3),
                            unpack_rgba(rgba));
    s_dirty = true;
}

void gx2_gpu_image(std::span<const std::uint32_t> pixels,
                   int stride,
                   int left,
                   int top,
                   int width,
                   int height,
                   double x,
                   double y,
                   double dw,
                   double dh,
                   double angle,
                   unsigned tint,
                   double opacity,
                   std::uint64_t key) {
    ensure_batch_started();
    CachedTexture *tex = upload_texture(key, pixels, stride, left, top, width, height);
    if (!tex) return;

    s_last_texture_id = tex->id;
    s_batcher.draw_quad(&tex->tex,
                        static_cast<float>(x),
                        static_cast<float>(y),
                        static_cast<float>(dw),
                        static_cast<float>(dh),
                        0.0f, 0.0f, 1.0f, 1.0f,
                        tint_opacity_to_rgba(tint, opacity),
                        static_cast<float>(angle));
    s_dirty = true;
}

bool gx2_gpu_cached_sub_image(double x,
                              double y,
                              double dw,
                              double dh,
                              float u0,
                              float v0,
                              float u1,
                              float v1,
                              unsigned tint,
                              double opacity,
                              std::uint64_t key,
                              double angle) {
    auto it = s_texture_cache.find(key);
    if (it == s_texture_cache.end()) return false;

    ensure_batch_started();
    it->second->last_frame = s_current_frame;
    s_last_texture_id = it->second->id;
    s_batcher.draw_quad(&it->second->tex,
                        static_cast<float>(x),
                        static_cast<float>(y),
                        static_cast<float>(dw),
                        static_cast<float>(dh),
                        u0, v0, u1, v1,
                        tint_opacity_to_rgba(tint, opacity),
                        static_cast<float>(angle));
    s_dirty = true;
    return true;
}

bool gx2_gpu_has_cached_image(std::uint64_t key) {
    return key != 0 && s_texture_cache.contains(key);
}

void gx2_gpu_preload_image(std::span<const std::uint32_t> pixels,
                           int stride,
                           int left,
                           int top,
                           int width,
                           int height,
                           std::uint64_t key) {
    if (key == 0 || !ensure_initialized()) return;
    (void)upload_texture(key, pixels, stride, left, top, width, height);
}

bool gx2_gpu_cached_image(int width,
                          int height,
                          double x,
                          double y,
                          double dw,
                          double dh,
                          double angle,
                          unsigned tint,
                          double opacity,
                          std::uint64_t key) {
    auto it = s_texture_cache.find(key);
    if (it == s_texture_cache.end()) return false;
    if (it->second->width != width || it->second->height != height) return false;

    ensure_batch_started();
    it->second->last_frame = s_current_frame;
    s_last_texture_id = it->second->id;
    s_batcher.draw_quad(&it->second->tex,
                        static_cast<float>(x),
                        static_cast<float>(y),
                        static_cast<float>(dw),
                        static_cast<float>(dh),
                        0.0f, 0.0f, 1.0f, 1.0f,
                        tint_opacity_to_rgba(tint, opacity),
                        static_cast<float>(angle));
    s_dirty = true;
    return true;
}

std::uint32_t gx2_gpu_pixel(int x, int y) {
    (void)x;
    (void)y;
    return 0;
}

bool gx2_gpu_readback(int width, int height, std::span<std::uint32_t> pixels) noexcept {
    (void)width;
    (void)height;
    (void)pixels;
    return false;
}

void gx2_gpu_box_batch_begin() {
    s_box_batch_active = true;
}

void gx2_gpu_box_batch_end() {
    s_box_batch_active = false;
}

void gx2_gpu_set_viewport(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (s_viewport_width == width && s_viewport_height == height) return;
    gx2_gpu_flush();
    s_viewport_width = width;
    s_viewport_height = height;
    if (s_bound_surface_id == 0) {
        s_main_width = width;
        s_main_height = height;
        set_tv_viewport_and_scissor();
    } else {
        GX2SetViewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f);
        GX2SetScissor(0, 0, static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
    }
    if (s_batcher.is_active()) {
        s_batcher.set_projection(Gx2Mat4::ortho(0.0f,
                                                static_cast<float>(width),
                                                static_cast<float>(height),
                                                0.0f));
    }
}

void gx2_gpu_set_blend_enable(bool enabled) {
    s_batcher.set_blend_enable(enabled);
}

void gx2_gpu_set_blend_mode(int src_rgb, int dst_rgb, int src_a, int dst_a, int eq_rgb, int eq_a) {
    s_batcher.set_blend_mode(src_rgb, dst_rgb, src_a, dst_a, eq_rgb, eq_a);
}

void gx2_gpu_set_alpha_test(bool enabled, float ref) {
    s_batcher.set_alpha_test(enabled, ref);
}

void gx2_gpu_set_color_mask(bool r, bool g, bool b, bool a) {
    s_batcher.set_color_mask(r, g, b, a);
}

void gx2_gpu_set_frame_number(std::size_t frame) {
    s_current_frame = frame;
}

std::size_t gx2_gpu_flush_draw_calls() {
    return s_flush_draw_calls;
}

std::size_t gx2_gpu_frame_draw_calls() {
    return s_frame_draw_calls;
}

std::uint32_t gx2_gpu_last_texture_id() {
    return s_last_texture_id;
}

std::vector<std::uint32_t> gx2_gpu_textures_used() {
    return s_textures_used;
}

std::uint32_t gx2_gpu_surface_create(int width, int height) {
    if (width <= 0 || height <= 0) return 0;

    auto surf = std::make_unique<Gx2Surface>();
    surf->width = width;
    surf->height = height;

    auto &cb = surf->color_buffer;
    std::memset(&cb, 0, sizeof(cb));
    cb.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    cb.surface.width = static_cast<std::uint32_t>(width);
    cb.surface.height = static_cast<std::uint32_t>(height);
    cb.surface.depth = 1;
    cb.surface.mipLevels = 1;
    cb.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    cb.surface.use = GX2_SURFACE_USE_TEXTURE_COLOR_BUFFER_TV;
    cb.surface.tileMode = GX2_TILE_MODE_DEFAULT;
    cb.viewNumSlices = 1;
    GX2CalcSurfaceSizeAndAlignment(&cb.surface);
    GX2InitColorBufferRegs(&cb);
    if (cb.surface.imageSize == 0) return 0;

    const std::size_t align = cb.surface.alignment ? cb.surface.alignment : 64u;
    const std::size_t alloc_size = ((cb.surface.imageSize + align - 1u) / align) * align;
    surf->image = static_cast<std::uint8_t *>(memalign(align, alloc_size));
    if (!surf->image) return 0;
    std::memset(surf->image, 0, cb.surface.imageSize);
    DCFlushRange(surf->image, cb.surface.imageSize);
    GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, surf->image, cb.surface.imageSize);
    cb.surface.image = surf->image;

    auto &tex = surf->texture;
    std::memset(&tex, 0, sizeof(tex));
    tex.surface = cb.surface;
    tex.surface.use = GX2_SURFACE_USE_TEXTURE;
    tex.viewNumMips = 1;
    tex.viewNumSlices = 1;
    tex.compMap = 0x00010203u;
    GX2InitTextureRegs(&tex);

    const std::uint32_t id = s_next_surface_id++;
    s_surfaces[id] = std::move(surf);
    return id;
}

void gx2_gpu_surface_destroy(std::uint32_t surface_id) {
    auto it = s_surfaces.find(surface_id);
    if (it == s_surfaces.end()) return;
    if (s_bound_surface_id == surface_id) {
        gx2_gpu_surface_bind(0, s_main_width, s_main_height);
    } else {
        gx2_gpu_flush();
    }
    GX2DrawDone();
    s_batcher.forget_texture(&it->second->texture);
    if (it->second->image) {
        std::free(it->second->image);
    }
    s_surfaces.erase(it);
}

void gx2_gpu_surface_bind(std::uint32_t surface_id, int width, int height) {
    if (s_bound_surface_id == surface_id) {
        if (surface_id == 0) {
            gx2_gpu_set_viewport(width, height);
        }
        return;
    }
    gx2_gpu_flush();
    if (s_bound_surface_id != 0 && s_dirty) {
        if (auto prev = s_surfaces.find(s_bound_surface_id); prev != s_surfaces.end() && prev->second->image) {
            GX2Invalidate(static_cast<GX2InvalidateMode>(GX2_INVALIDATE_MODE_COLOR_BUFFER |
                                                         GX2_INVALIDATE_MODE_TEXTURE),
                          prev->second->image,
                          prev->second->color_buffer.surface.imageSize);
        }
        s_dirty = false;
    }
    if (surface_id == 0) {
        s_bound_surface_id = 0;
        if (GX2ColorBuffer *tv = WHBGfxGetTVColourBuffer()) {
            GX2SetColorBuffer(tv, GX2_RENDER_TARGET_0);
        }
        s_main_width = width;
        s_main_height = height;
        s_viewport_width = width;
        s_viewport_height = height;
        set_tv_viewport_and_scissor();
        if (s_batcher.is_active()) {
            s_batcher.set_projection(Gx2Mat4::ortho(0.0f,
                                                    static_cast<float>(width),
                                                    static_cast<float>(height),
                                                    0.0f));
        }
        return;
    }
    auto it = s_surfaces.find(surface_id);
    if (it == s_surfaces.end()) return;
    s_bound_surface_id = surface_id;
    GX2SetColorBuffer(&it->second->color_buffer, GX2_RENDER_TARGET_0);
    s_viewport_width = it->second->width;
    s_viewport_height = it->second->height;
    GX2SetViewport(0.0f, 0.0f, static_cast<float>(s_viewport_width), static_cast<float>(s_viewport_height), 0.0f, 1.0f);
    GX2SetScissor(0, 0, static_cast<std::uint32_t>(s_viewport_width), static_cast<std::uint32_t>(s_viewport_height));
    if (s_batcher.is_active()) {
        s_batcher.set_projection(Gx2Mat4::ortho(0.0f,
                                                static_cast<float>(s_viewport_width),
                                                static_cast<float>(s_viewport_height),
                                                0.0f));
    }
}

void gx2_gpu_surface_clear(std::uint32_t argb) {
    if (s_bound_surface_id == 0) return;
    auto it = s_surfaces.find(s_bound_surface_id);
    if (it == s_surfaces.end()) return;
    ensure_batch_started();
    const bool prev_blend = s_batcher.blend_enabled();
    const bool prev_alpha_test = s_batcher.alpha_test_enabled();
    const float prev_alpha_ref = s_batcher.alpha_test_ref();
    s_batcher.set_blend_enable(false);
    s_batcher.set_alpha_test(false, 0.0f);
    const std::uint32_t rgba = ((argb & 0x00ffffffu) << 8) | ((argb >> 24) & 0xffu);
    s_batcher.draw_quad(nullptr, 0.0f, 0.0f,
                        static_cast<float>(it->second->width),
                        static_cast<float>(it->second->height),
                        0.0f, 0.0f, 0.0f, 0.0f, rgba, 0.0f);
    s_batcher.set_alpha_test(prev_alpha_test, prev_alpha_ref);
    s_batcher.set_blend_enable(prev_blend);
    s_dirty = true;
}

bool gx2_gpu_surface_draw(std::uint32_t surface_id,
                          double left,
                          double top,
                          double width,
                          double height,
                          double x,
                          double y,
                          double dw,
                          double dh,
                          double angle,
                          unsigned tint,
                          double opacity) {
    auto it = s_surfaces.find(surface_id);
    if (it == s_surfaces.end()) return false;

    ensure_batch_started();
    const float sw = static_cast<float>(it->second->width);
    const float sh = static_cast<float>(it->second->height);
    const float u0 = static_cast<float>(left) / sw;
    const float v0 = static_cast<float>(top) / sh;
    const float u1 = static_cast<float>(left + width) / sw;
    const float v1 = static_cast<float>(top + height) / sh;

    s_batcher.draw_quad(&it->second->texture,
                        static_cast<float>(x),
                        static_cast<float>(y),
                        static_cast<float>(dw),
                        static_cast<float>(dh),
                        u0, v0, u1, v1,
                        tint_opacity_to_rgba(tint, opacity),
                        static_cast<float>(angle));
    s_dirty = true;
    return true;
}

} // namespace regml::gx2
