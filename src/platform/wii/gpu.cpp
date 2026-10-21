#include "gpu.hpp"
#if REGML_WII
#include "error.hpp"
#include "lookup.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <gccore.h>
#include <malloc.h>
#include <memory>
#include <unordered_map>
namespace regml {
namespace {
struct Free {
    void operator()(void *p) const { std::free(p); }
};
struct Texture {
    std::unique_ptr<void, Free> data;
    GXTexObj object{};
    std::size_t bytes = 0, used = 0;
    int width = 0, height = 0;
    float inverse_width = 0, inverse_height = 0;
};
struct ImageVertex {
    float x = 0, y = 0;
    GXColor color{};
    float u = 0, v = 0;
};
struct Box {
    float x, y, width, height;
    GXColor color;
};
constexpr std::size_t image_batch_quads = 128;
std::array<ImageVertex, image_batch_quads * 4> image_batch{};
std::size_t image_batch_size = 0;
const Texture *image_batch_texture = nullptr;
std::array<Box, 4096> box_batch{};
std::size_t box_batch_size = 0;
bool box_batch_active = false;
std::unordered_map<std::uint64_t, Texture> textures;
std::size_t cache_bytes = 0, clock = 0;
bool dirty = false;
bool presented = false;
int current_pipeline = -1;
const void *bound_texture = nullptr;
constexpr std::size_t budget = 2 * 1024 * 1024;
std::uint64_t child_key(std::uint64_t key, int x, int y) {
    return key ? ((key ^ static_cast<unsigned>(x)) * 1099511628211ULL ^ static_cast<unsigned>(y)) : 0;
}
void flush_image_batch() {
    if (!image_batch_size)
        return;
    GX_Begin(GX_QUADS, GX_VTXFMT0, static_cast<u16>(image_batch_size));
    for (std::size_t i = 0; i < image_batch_size; ++i) {
        const auto &vertex = image_batch[i];
        GX_Position2f32(vertex.x, vertex.y);
        GX_Color4u8(vertex.color.r, vertex.color.g, vertex.color.b, vertex.color.a);
        GX_TexCoord2f32(vertex.u, vertex.v);
    }
    GX_End();
    image_batch_size = 0;
    image_batch_texture = nullptr;
}
bool cached(int width, int height, std::uint64_t key) {
    if (width <= 1024 && height <= 1024)
        return textures.contains(key);
    for (int y = 0; y < height; y += 1024)
        for (int x = 0; x < width; x += 1024)
            if (!cached(std::min(1024, width - x), std::min(1024, height - y), child_key(key, x, y)))
                return false;
    return true;
}
GXColor color(std::uint32_t rgba) {
    return {static_cast<u8>(rgba >> 16), static_cast<u8>(rgba >> 8), static_cast<u8>(rgba),
            static_cast<u8>(rgba >> 24)};
}
void pipeline(bool textured) {
    const int mode = textured ? 1 : 0;
    if (current_pipeline == mode)
        return;
    flush_image_batch();
    current_pipeline = mode;
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetNumTexGens(textured ? 1 : 0);
    GX_SetTevOrder(GX_TEVSTAGE0, textured ? GX_TEXCOORD0 : GX_TEXCOORDNULL,
                   textured ? GX_TEXMAP0 : GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, textured ? GX_MODULATE : GX_PASSCLR);
    if (textured) {
        GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    }
    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_COPY);
    GX_SetAlphaCompare(GX_GREATER, 0, GX_AOP_AND, GX_ALWAYS, 0);
}
void draw_image(Texture &texture,
                int width,
                int height,
                double x,
                double y,
                double dw,
                double dh,
                double angle,
                unsigned tint,
                double opacity) {
    texture.used = ++clock;
    pipeline(true);
    if (image_batch_size && image_batch_texture != &texture)
        flush_image_batch();
    if (bound_texture != texture.data.get()) {
        flush_image_batch();
        GX_LoadTexObj(&texture.object, GX_TEXMAP0);
        bound_texture = texture.data.get();
    }
    const auto c = color((static_cast<unsigned>(std::clamp(opacity, 0.0, 1.0) * 255 + 0.5) << 24) | tint);
    const float tu = static_cast<float>(width) * texture.inverse_width;
    const float tv = static_cast<float>(height) * texture.inverse_height;
    const auto vertex = [&](double px, double py, float u, float v) {
        image_batch[image_batch_size++] = {static_cast<float>(px), static_cast<float>(py), c, u, v};
    };
    if (image_batch_size + 4 > image_batch.size())
        flush_image_batch();
    image_batch_texture = &texture;
    if (angle == 0) {
        vertex(x, y, 0, 0);
        vertex(x + dw, y, tu, 0);
        vertex(x + dw, y + dh, tu, tv);
        vertex(x, y + dh, 0, tv);
    } else {
        const double rad = angle * 3.141592653589793 / 180;
        const double cs = std::cos(rad), sn = std::sin(rad);
        const double wx = dw * cs, wy = -dw * sn, hx = dh * sn, hy = dh * cs;
        vertex(x, y, 0, 0);
        vertex(x + wx, y + wy, tu, 0);
        vertex(x + wx + hx, y + wy + hy, tu, tv);
        vertex(x + hx, y + hy, 0, tv);
    }
    dirty = true;
    if (image_batch_size == image_batch.size())
        flush_image_batch();
}
Texture upload(std::span<const std::uint32_t> pixels, int stride, int left, int top, int width, int height) {
    Texture t;
    t.width = (width + 3) & ~3;
    t.height = (height + 3) & ~3;
    if (t.width > 1024 || t.height > 1024)
        throw Error(ErrorKind::unsupported, "GX texture region exceeds 1024 pixels");
    t.bytes = static_cast<std::size_t>(t.width) * t.height * 4;
    t.data.reset(memalign(32, t.bytes));
    if (!t.data)
        throw Error(ErrorKind::io, "GX texture allocation failed");
    auto *out = static_cast<u32 *>(t.data.get());
    for (int ty = 0; ty < t.height; ty += 4)
        for (int tx = 0; tx < t.width; tx += 4) {
            const bool full = tx + 4 <= width;
            for (int yy = 0; yy < 4; ++yy) {
                const auto *row = pixels.data() +
                                  static_cast<std::size_t>(top + std::min(ty + yy, height - 1)) * stride +
                                  left;
                for (int pair = 0; pair < 2; ++pair) {
                    const int sx = tx + pair * 2;
                    const u32 a = row[full ? sx : std::min(sx, width - 1)];
                    const u32 b = row[full ? sx + 1 : std::min(sx + 1, width - 1)];
                    // the Wii is big endian; each word packs two AR or GB pairs
                    out[yy * 2 + pair] = (a & 0xffff0000U) | (b >> 16);
                    out[8 + yy * 2 + pair] = (a << 16) | (b & 0xffffU);
                }
            }
            out += 16;
        }
    t.inverse_width = 1.0f / t.width;
    t.inverse_height = 1.0f / t.height;
    DCFlushRange(t.data.get(), t.bytes);
    GX_InvalidateTexAll();
    bound_texture = nullptr;
    GX_InitTexObj(&t.object, t.data.get(), static_cast<u16>(t.width), static_cast<u16>(t.height), GX_TF_RGBA8,
                  GX_CLAMP, GX_CLAMP, GX_FALSE);
#if REGML_240P
    GX_InitTexObjLOD(&t.object, GX_LINEAR, GX_LINEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
#else
    GX_InitTexObjLOD(&t.object, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
#endif
    return t;
}
} // namespace
bool wii_gpu_cached_image(int width,
                          int height,
                          double x,
                          double y,
                          double dw,
                          double dh,
                          double angle,
                          unsigned tint,
                          double opacity,
                          std::uint64_t key) {
    if (!key)
        return false;
    if (width <= 1024 && height <= 1024) {
        const auto found = textures.find(key);
        if (found == textures.end())
            return false;
        draw_image(found->second, width, height, x, y, dw, dh, angle, tint, opacity);
        return true;
    }
    if (!cached(width, height, key))
        return false;
    // on a cache hit, we can skip decoding the PNG and copying the CPU region
    wii_gpu_image({}, 0, 0, 0, width, height, x, y, dw, dh, angle, tint, opacity, key);
    return true;
}
void wii_gpu_begin() {
    flush_image_batch();
    presented = false;
    current_pipeline = -1;
    bound_texture = nullptr;
    GX_SetViewport(0, 0, render_width, render_height, 0, 1);
    GX_SetScissor(0, 0, render_width, render_height);
    Mtx44 projection;
    guOrtho(projection, 0, render_height, 0, render_width, 0, 300);
    GX_LoadProjectionMtx(projection, GX_ORTHOGRAPHIC);
    GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
}
void wii_gpu_flush() {
    flush_image_batch();
}
void wii_gpu_presented() {
    presented = true;
    dirty = false;
}
void wii_gpu_box(double x, double y, double width, double height, std::uint32_t rgba) {
    if (!(rgba >> 24) || width <= 0 || height <= 0)
        return;
    if (box_batch_active) {
        if (box_batch_size < box_batch.size())
            box_batch[box_batch_size++] = {static_cast<float>(x), static_cast<float>(y),
                                           static_cast<float>(width), static_cast<float>(height),
                                           color(rgba)};
        return;
    }
    pipeline(false);
    const auto c = color(rgba);
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    for (int i : {0, 1, 3, 2}) {
        GX_Position2f32(static_cast<float>(x + (i & 1 ? width : 0)),
                        static_cast<float>(y + (i & 2 ? height : 0)));
        GX_Color4u8(c.r, c.g, c.b, c.a);
    }
    GX_End();
    dirty = true;
}
void wii_gpu_box_batch_begin() {
    box_batch_size = 0;
    box_batch_active = true;
}
void wii_gpu_box_batch_end() {
    box_batch_active = false;
    if (!box_batch_size)
        return;
    pipeline(false);
    GX_Begin(GX_QUADS, GX_VTXFMT0, static_cast<u16>(box_batch_size * 4));
    for (std::size_t i = 0; i < box_batch_size; ++i) {
        const auto &box = box_batch[i];
        for (int corner : {0, 1, 3, 2}) {
            GX_Position2f32(box.x + (corner & 1 ? box.width : 0), box.y + (corner & 2 ? box.height : 0));
            GX_Color4u8(box.color.r, box.color.g, box.color.b, box.color.a);
        }
    }
    GX_End();
    dirty = true;
    box_batch_size = 0;
}
std::uint32_t wii_gpu_pixel(int x, int y) {
    if (dirty) {
        flush_image_batch();
        GX_DrawDone();
        dirty = false;
    }
    GXColor rgba{};
#if REGML_240P
    if (presented) {
        x *= 2;
        y *= 2;
    }
#endif
    GX_PeekARGB(static_cast<u16>(x), static_cast<u16>(y), &rgba);
    return 0xff000000 | (static_cast<u32>(rgba.r) << 16) | (static_cast<u32>(rgba.g) << 8) | rgba.b;
}
void wii_gpu_triangle(double x1, double y1, double x2, double y2, double x3, double y3, std::uint32_t rgba) {
    pipeline(false);
    const auto c = color(rgba);
    GX_Begin(GX_TRIANGLES, GX_VTXFMT0, 3);
    GX_Position2f32(static_cast<float>(x1), static_cast<float>(y1));
    GX_Color4u8(c.r, c.g, c.b, c.a);
    GX_Position2f32(static_cast<float>(x2), static_cast<float>(y2));
    GX_Color4u8(c.r, c.g, c.b, c.a);
    GX_Position2f32(static_cast<float>(x3), static_cast<float>(y3));
    GX_Color4u8(c.r, c.g, c.b, c.a);
    GX_End();
    dirty = true;
}
void wii_gpu_circle(double x, double y, double radius, bool outline, std::uint32_t rgba) {
    constexpr int segments = 96;
    static const auto unit = [] {
        std::array<std::array<float, 2>, segments + 1> points{};
        for (int i = 0; i < segments; ++i) {
            const double a = i * 6.283185307179586 / segments;
            points[i] = {static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))};
        }
        points[segments] = points[0];
        return points;
    }();
    pipeline(false);
    const auto c = color(rgba);
    const auto vertex = [&](double px, double py) {
        GX_Position2f32(static_cast<float>(px), static_cast<float>(py));
        GX_Color4u8(c.r, c.g, c.b, c.a);
    };
    if (outline) {
        const double inner = std::max(0.0, radius - 1);
        GX_Begin(GX_TRIANGLESTRIP, GX_VTXFMT0, 2 * (segments + 1));
        for (const auto &p : unit) {
            vertex(x + radius * p[0], y + radius * p[1]);
            vertex(x + inner * p[0], y + inner * p[1]);
        }
    } else {
        GX_Begin(GX_TRIANGLEFAN, GX_VTXFMT0, segments + 2);
        vertex(x, y);
        for (const auto &p : unit)
            vertex(x + radius * p[0], y + radius * p[1]);
    }
    GX_End();
    dirty = true;
}
void wii_gpu_image(std::span<const std::uint32_t> pixels,
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
    if (width <= 0 || height <= 0 || opacity <= 0)
        return;
    if (width > 1024 || height > 1024) {
        const double rad = angle * 3.141592653589793 / 180;
        const double cs = angle == 0 ? 1 : std::cos(rad), sn = angle == 0 ? 0 : std::sin(rad);
        for (int yy = 0; yy < height; yy += 1024)
            for (int xx = 0; xx < width; xx += 1024) {
                const int w = std::min(1024, width - xx), h = std::min(1024, height - yy);
                const double u = dw * xx / width, v = dh * yy / height;
                const auto child = child_key(key, xx, yy);
                wii_gpu_image(pixels, stride, left + xx, top + yy, w, h, x + u * cs + v * sn,
                              y - u * sn + v * cs, dw * w / width, dh * h / height, angle, tint, opacity,
                              child);
            }
        return;
    }
    Texture temporary;
    Texture *texture;
    if (key) {
        auto it = textures.find(key);
        if (it == textures.end()) {
            const auto texture_width = static_cast<std::size_t>((width + 3) & ~3);
            const auto texture_height = static_cast<std::size_t>((height + 3) & ~3);
            const auto requested_bytes = texture_width * texture_height * 4;
            bool waited = false;
            while (cache_bytes + requested_bytes > budget && !textures.empty()) {
                auto oldest =
                    std::min_element(textures.begin(), textures.end(), [](const auto &a, const auto &b) {
                        return a.second.used < b.second.used;
                    });
                if (!waited) {
                    flush_image_batch();
                    GX_DrawDone();
                    waited = true;
                }
                cache_bytes -= oldest->second.bytes;
                bound_texture = nullptr;
                textures.erase(oldest);
            }
            Texture t;
            try {
                t = upload(pixels, stride, left, top, width, height);
            } catch (const Error &) {
                if (textures.empty())
                    throw;
                flush_image_batch();
                GX_DrawDone();
                textures.clear();
                cache_bytes = 0;
                bound_texture = nullptr;
                t = upload(pixels, stride, left, top, width, height);
            }
            cache_bytes += t.bytes;
            it = textures.emplace(key, std::move(t)).first;
        }
        texture = &it->second;
    } else {
        temporary = upload(pixels, stride, left, top, width, height);
        texture = &temporary;
    }
    draw_image(*texture, width, height, x, y, dw, dh, angle, tint, opacity);
    if (!key) {
        flush_image_batch();
        GX_DrawDone();
        bound_texture = nullptr;
    }
}
} // namespace regml
#endif
