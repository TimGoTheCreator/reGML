#include "gpu.hpp"

#if REGML_PS2
#include "error.hpp"
#include "rendersoftware/canvas.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <dmaKit.h>
#include <gsInline.h>
#include <kernel.h>
#include <malloc.h>

namespace regml {
namespace {
constexpr int kTileSize = 128;
constexpr std::size_t kScratchSlotCount = 2;
constexpr std::size_t kSlotCount = 40;
constexpr std::size_t kCachedImageTileLimit = 256;
constexpr std::uint64_t kHashPrime = 1099511628211ULL;
constexpr std::uint32_t kVif1Stat = 0x10003c00;
constexpr std::uint32_t kVif1Chcr = 0x10009000;
constexpr std::uint32_t kVif1Madr = 0x10009010;
constexpr std::uint32_t kVif1Qwc = 0x10009020;

struct TextureSlot {
    GSTEXTURE texture{};
    u32 *memory = nullptr;
    std::uint64_t key = 0;
    std::size_t used = 0;
    bool pending_draws = false;
};

struct CachedImageTile {
    TextureSlot *slot;
    int source_x, source_y, width, height, tile_x, tile_y;
};

GSGLOBAL *gs = nullptr;
std::array<TextureSlot, kSlotCount> slots{};
std::size_t slot_count = 0;
std::size_t use_clock = 0;
std::size_t scratch_slot = 0;
int canvas_width = 0, canvas_height = 0;
double viewport_scale = 1, viewport_x = 0, viewport_y = 0;
int frame_buffer = 0, last_presented_buffer = 0;
bool frame_active = false;
bool readback_valid = false;
bool font_atlas_ready = false;
std::uint16_t *raw_frame = nullptr;
std::size_t raw_frame_bytes = 0;
std::uint32_t *canvas_frame = nullptr;
bool profile_enabled = false;
Ps2GpuProfile frame_profile{};
std::array<CachedImageTile, kCachedImageTileLimit> cached_image_tiles{};

std::uint64_t tile_key(std::uint64_t key, int x, int y) noexcept {
    key ^= static_cast<std::uint32_t>(x);
    key *= kHashPrime;
    key ^= static_cast<std::uint32_t>(y);
    key *= kHashPrime;
    return key ? key : 1;
}

std::uint8_t ps2_alpha(unsigned alpha) noexcept {
    return static_cast<std::uint8_t>((std::min(alpha, 255U) * 128U + 127U) / 255U);
}

u64 gs_color(std::uint32_t rgba) noexcept {
    return GS_SETREG_RGBAQ((rgba >> 16) & 255, (rgba >> 8) & 255, rgba & 255, ps2_alpha(rgba >> 24), 0);
}

u64 gs_tint(unsigned tint, double opacity) noexcept {
    const auto alpha = static_cast<unsigned>(std::clamp(opacity, 0.0, 1.0) * 128.0 + 0.5);
    return GS_SETREG_RGBAQ((tint >> 16) & 255, (tint >> 8) & 255, tint & 255, alpha, 0);
}

double screen_x(double x) noexcept {
    return viewport_x + x * viewport_scale;
}
double screen_y(double y) noexcept {
    return viewport_y + y * viewport_scale;
}

void set_canvas_scissor() {
    const int left = std::clamp(static_cast<int>(std::ceil(viewport_x)), 0, gs->Width - 1);
    const int top = std::clamp(static_cast<int>(std::ceil(viewport_y)), 0, gs->Height - 1);
    const int right = std::clamp(static_cast<int>(std::ceil(viewport_x + canvas_width * viewport_scale)) - 1,
                                 left, gs->Width - 1);
    const int bottom = std::clamp(
        static_cast<int>(std::ceil(viewport_y + canvas_height * viewport_scale)) - 1, top, gs->Height - 1);
    gsKit_set_scissor(gs, GS_SETREG_SCISSOR(left, right, top, bottom));
}

TextureSlot *find_slot(std::uint64_t key) noexcept {
    for (std::size_t i = kScratchSlotCount; i < slot_count; ++i) {
        if (slots[i].key == key) {
            slots[i].used = ++use_clock;
            return &slots[i];
        }
    }
    return nullptr;
}

TextureSlot *upload_tile(std::span<const std::uint32_t> pixels,
                         int stride,
                         int source_tile_x,
                         int source_tile_y,
                         int cache_tile_x,
                         int cache_tile_y,
                         std::uint64_t key) {
    if (slot_count == 0 || stride <= 0 || pixels.empty())
        return nullptr;
    TextureSlot *slot = nullptr;
    std::uint64_t cache_key = 0;
    bool cache_slot = false;
    if (key) {
        cache_key = tile_key(key, cache_tile_x, cache_tile_y);
        if ((slot = find_slot(cache_key)))
            return slot;
        if (slot_count > kScratchSlotCount) {
            slot = &slots[kScratchSlotCount];
            for (std::size_t i = kScratchSlotCount + 1; i < slot_count; ++i)
                if (slots[i].used < slot->used)
                    slot = &slots[i];
            cache_slot = true;
        } else {
            slot = &slots[0];
        }
    } else {
        const auto scratch_start = font_atlas_ready ? std::size_t{1} : std::size_t{0};
        const auto scratch_count = std::min(kScratchSlotCount - scratch_start, slot_count - scratch_start);
        slot = &slots[scratch_start + scratch_slot % scratch_count];
        scratch_slot = (scratch_slot + 1) % scratch_count;
    }

    const auto page_height = static_cast<int>(pixels.size() / static_cast<std::size_t>(stride));
    if (page_height <= 0)
        return nullptr;

    // Wait only when queued draws still reference the texture being replaced.
    if (slot->pending_draws) {
        ps2_gpu_flush();
    }
    if (cache_slot) {
        slot->key = cache_key;
        slot->used = ++use_clock;
    }
    for (int y = 0; y < kTileSize; ++y) {
        const int source_y = std::clamp(source_tile_y + y, 0, page_height - 1);
        auto *destination = slot->memory + static_cast<std::size_t>(y) * kTileSize;
        const auto *source = pixels.data() + static_cast<std::size_t>(source_y) * stride;
        for (int x = 0; x < kTileSize; ++x) {
            const int source_x = std::clamp(source_tile_x + x, 0, stride - 1);
            const auto pixel = source[source_x];
            const auto red = (pixel >> 16) & 255;
            const auto green = (pixel >> 8) & 255;
            const auto blue = pixel & 255;
            const auto alpha = ps2_alpha(pixel >> 24);
            destination[x] = red | (green << 8) | (blue << 16) | (alpha << 24);
        }
    }
    if (profile_enabled)
        ++frame_profile.texture_uploads;
    gsKit_texture_upload(gs, &slot->texture);
    readback_valid = false;
    return slot;
}

void draw_tile(TextureSlot &slot,
               int source_x,
               int source_y,
               int width,
               int height,
               int tile_x,
               int tile_y,
               int image_x,
               int image_y,
               int image_width,
               int image_height,
               double x,
               double y,
               double draw_width,
               double draw_height,
               double angle,
               double cs,
               double sn,
               u64 color) {
    const int u0 = source_x - tile_x;
    const int v0 = source_y - tile_y;
    const int u1 = u0 + width;
    const int v1 = v0 + height;
    const double du0 = static_cast<double>(source_x - image_x) * draw_width / image_width;
    const double du1 = static_cast<double>(source_x + width - image_x) * draw_width / image_width;
    const double dv0 = static_cast<double>(source_y - image_y) * draw_height / image_height;
    const double dv1 = static_cast<double>(source_y + height - image_y) * draw_height / image_height;
    if (angle == 0 && draw_width > 0 && draw_height > 0) {
        if (profile_enabled)
            ++frame_profile.image_quads;
        gsKit_prim_sprite_texture(gs, &slot.texture, static_cast<float>(screen_x(x + du0)),
                                  static_cast<float>(screen_y(y + dv0)), static_cast<float>(u0),
                                  static_cast<float>(v0), static_cast<float>(screen_x(x + du1)),
                                  static_cast<float>(screen_y(y + dv1)), static_cast<float>(u1),
                                  static_cast<float>(v1), 1, color);
        return;
    }
    const auto point = [&](double u, double v) {
        return std::array<double, 2>{screen_x(x + u * cs + v * sn), screen_y(y - u * sn + v * cs)};
    };
    const auto p0 = point(du0, dv0);
    const auto p1 = point(du1, dv0);
    const auto p2 = point(du1, dv1);
    const auto p3 = point(du0, dv1);
    if (profile_enabled)
        ++frame_profile.image_quads;
    gsKit_prim_quad_texture_3d(gs, &slot.texture, static_cast<float>(p0[0]), static_cast<float>(p0[1]), 1,
                               static_cast<float>(u0), static_cast<float>(v0),
                               static_cast<float>(p1[0]), static_cast<float>(p1[1]), 1,
                               static_cast<float>(u1), static_cast<float>(v0),
                               static_cast<float>(p2[0]), static_cast<float>(p2[1]), 1,
                               static_cast<float>(u1), static_cast<float>(v1),
                               static_cast<float>(p3[0]), static_cast<float>(p3[1]), 1,
                               static_cast<float>(u0), static_cast<float>(v1), color);
}

bool tile_visible(int offset_x,
                  int offset_y,
                  int width,
                  int height,
                  int image_width,
                  int image_height,
                  double x,
                  double y,
                  double draw_width,
                  double draw_height,
                  double cs,
                  double sn,
                  bool rotated) noexcept {
    const double u0 = static_cast<double>(offset_x) * draw_width / image_width;
    const double u1 = static_cast<double>(offset_x + width) * draw_width / image_width;
    const double v0 = static_cast<double>(offset_y) * draw_height / image_height;
    const double v1 = static_cast<double>(offset_y + height) * draw_height / image_height;
    if (!rotated) {
        return std::max(x + u0, x + u1) > 0 && std::max(y + v0, y + v1) > 0 &&
               std::min(x + u0, x + u1) < canvas_width &&
               std::min(y + v0, y + v1) < canvas_height;
    }

    const double tile_xs[] = {x + u0 * cs + v0 * sn, x + u1 * cs + v0 * sn,
                              x + u1 * cs + v1 * sn, x + u0 * cs + v1 * sn};
    const double tile_ys[] = {y - u0 * sn + v0 * cs, y - u1 * sn + v0 * cs,
                              y - u1 * sn + v1 * cs, y - u0 * sn + v1 * cs};
    return *std::max_element(tile_xs, tile_xs + 4) > 0 &&
           *std::max_element(tile_ys, tile_ys + 4) > 0 &&
           *std::min_element(tile_xs, tile_xs + 4) < canvas_width &&
           *std::min_element(tile_ys, tile_ys + 4) < canvas_height;
}

void image_impl(std::span<const std::uint32_t> pixels,
                int stride,
                int left,
                int top,
                int width,
                int height,
                double x,
                double y,
                double draw_width,
                double draw_height,
                double angle,
                unsigned tint,
                double opacity,
                std::uint64_t key,
                double cs = 1,
                double sn = 0,
                bool transform_ready = false) {
    if (!gs || width <= 0 || height <= 0 || draw_width == 0 || draw_height == 0 || opacity <= 0)
        return;
    const auto profile_start = profile_enabled ? std::chrono::steady_clock::now()
                                               : std::chrono::steady_clock::time_point{};
    const auto color = gs_tint(tint, opacity);
    if (!transform_ready && angle != 0) {
        const double radians = angle * 3.14159265358979323846 / 180.0;
        cs = std::cos(radians);
        sn = std::sin(radians);
    }
    const int end_x = left + width, end_y = top + height;
    for (int offset_y = 0; offset_y < height; offset_y += kTileSize) {
        const int tile_y = top + offset_y;
        const int y0 = tile_y;
        const int y1 = std::min(end_y, tile_y + kTileSize);
        for (int offset_x = 0; offset_x < width; offset_x += kTileSize) {
            const int tile_x = left + offset_x;
            const int x0 = tile_x;
            const int x1 = std::min(end_x, tile_x + kTileSize);
            const int part_width = x1 - x0, part_height = y1 - y0;
            if (!tile_visible(offset_x, offset_y, part_width, part_height, width, height, x, y, draw_width,
                              draw_height, cs, sn, angle != 0))
                continue;
            TextureSlot *slot = key ? find_slot(tile_key(key, offset_x, offset_y)) : nullptr;
            if (!slot && !pixels.empty())
                slot = upload_tile(pixels, stride, tile_x, tile_y, offset_x, offset_y, key);
            if (slot)
                draw_tile(*slot, x0, y0, part_width, part_height, tile_x, tile_y, left, top, width, height, x,
                          y, draw_width, draw_height, angle, cs, sn, color);
            if (slot)
                slot->pending_draws = true;
        }
    }
    readback_valid = false;
    if (profile_enabled) {
        frame_profile.image_ms += std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - profile_start)
                                     .count();
        ++frame_profile.image_calls;
    }
}

bool read_framebuffer() noexcept {
    if (!gs || !raw_frame || !canvas_frame || canvas_width <= 0 || canvas_height <= 0)
        return false;
    const int buffer = frame_active ? frame_buffer : last_presented_buffer;
    const auto byte_count = static_cast<std::size_t>(gs->Width) * gs->Height * sizeof(std::uint16_t);
    if (byte_count > raw_frame_bytes || byte_count == 0 || (byte_count & 15) != 0)
        return false;

    ps2_gpu_flush();
    dmaKit_wait_fast();
    auto *packet = reinterpret_cast<u64 *>(gsKit_heap_alloc(gs, 5, 80, GIF_AD));
    if (!packet)
        return false;
    if (packet == gs->CurQueue->last_tag) {
        *packet++ = GIF_TAG_AD(5);
        *packet++ = GIF_AD;
    }
    *packet++ = GS_SETREG_BITBLTBUF(gs->ScreenBuffer[buffer] / 256, (gs->Width + 63) / 64, gs->PSM, 0, 0, 0);
    *packet++ = GS_BITBLTBUF;
    *packet++ = GS_SETREG_TRXPOS(0, 0, 0, 0, 0);
    *packet++ = GS_TRXPOS;
    *packet++ = GS_SETREG_TRXREG(gs->Width, gs->Height);
    *packet++ = GS_TRXREG;
    *packet++ = 0;
    *packet++ = GS_FINISH;
    *packet++ = GS_SETREG_TRXDIR(1);
    *packet++ = GS_TRXDIR;
    *GS_CSR = 1U << 1;
    gsKit_queue_exec(gs);
    dmaKit_wait_fast();

    while (!GS_CSR_FINISH) {
    }
    while (*reinterpret_cast<volatile std::uint32_t *>(kVif1Stat) & 0x1f000003U) {
    }

    *reinterpret_cast<volatile std::uint32_t *>(kVif1Stat) = 1U << 23;
    *GS_BUSDIR = 1;
    FlushCache(WRITEBACK_DCACHE);
    *reinterpret_cast<volatile std::uint32_t *>(kVif1Qwc) = static_cast<std::uint32_t>(byte_count / 16);
    *reinterpret_cast<volatile std::uint32_t *>(kVif1Madr) = reinterpret_cast<std::uint32_t>(raw_frame);
    *reinterpret_cast<volatile std::uint32_t *>(kVif1Chcr) = 0x100;
    while (*reinterpret_cast<volatile std::uint32_t *>(kVif1Chcr) & 0x100) {
    }
    *reinterpret_cast<volatile std::uint32_t *>(kVif1Stat) = 0;
    *GS_BUSDIR = 0;
    FlushCache(INVALIDATE_DCACHE);

    for (int y = 0; y < canvas_height; ++y) {
        const int source_y = std::clamp(static_cast<int>(viewport_y + (y + 0.5) * viewport_scale), 0,
                                        static_cast<int>(gs->Height) - 1);
        for (int x = 0; x < canvas_width; ++x) {
            const int source_x = std::clamp(static_cast<int>(viewport_x + (x + 0.5) * viewport_scale), 0,
                                            static_cast<int>(gs->Width) - 1);
            const auto packed = raw_frame[static_cast<std::size_t>(source_y) * gs->Width + source_x];
            const unsigned red = ((packed & 31) * 255 + 15) / 31;
            const unsigned green = (((packed >> 5) & 31) * 255 + 15) / 31;
            const unsigned blue = (((packed >> 10) & 31) * 255 + 15) / 31;
            canvas_frame[static_cast<std::size_t>(y) * canvas_width + x] =
                0xff000000 | (red << 16) | (green << 8) | blue;
        }
    }
    readback_valid = true;
    return true;
}
} // namespace

void ps2_gpu_initialize(GSGLOBAL *global, int width, int height) {
    ps2_gpu_shutdown();
    if (!global || width < 1 || height < 1)
        throw Error(ErrorKind::io, "PS2 GPU renderer received invalid framebuffer dimensions");
    gs = global;
    canvas_width = width;
    canvas_height = height;
    viewport_scale =
        std::min(static_cast<double>(gs->Width) / width, static_cast<double>(gs->Height) / height);
    viewport_x = (gs->Width - width * viewport_scale) * 0.5;
    viewport_y = (gs->Height - height * viewport_scale) * 0.5;
    canvas_frame = static_cast<std::uint32_t *>(memalign(64, static_cast<std::size_t>(width) * height * 4));
    raw_frame_bytes = (static_cast<std::size_t>(gs->Width) * gs->Height * 2 + 15) & ~std::size_t(15);
    raw_frame = static_cast<std::uint16_t *>(memalign(64, raw_frame_bytes));
    if (!canvas_frame || !raw_frame) {
        ps2_gpu_shutdown();
        throw Error(ErrorKind::io, "PS2 GPU readback buffer allocation failed");
    }
    std::memset(canvas_frame, 0, static_cast<std::size_t>(width) * height * 4);
    std::memset(raw_frame, 0, raw_frame_bytes);
    for (auto &slot : slots) {
        slot.memory = static_cast<u32 *>(memalign(64, kTileSize * kTileSize * 4));
        if (!slot.memory)
            break;
        slot.texture.Width = kTileSize;
        slot.texture.Height = kTileSize;
        slot.texture.PSM = GS_PSM_CT32;
        slot.texture.TBW = kTileSize / 64;
        slot.texture.Mem = slot.memory;
        slot.texture.Vram = gsKit_vram_alloc(gs, gsKit_texture_size(kTileSize, kTileSize, GS_PSM_CT32),
                                             GSKIT_ALLOC_USERBUFFER);
        slot.texture.Filter = GS_FILTER_NEAREST;
        if (slot.texture.Vram == GSKIT_ALLOC_ERROR) {
            std::free(slot.memory);
            slot.memory = nullptr;
            break;
        }
        ++slot_count;
    }
    if (slot_count == 0) {
        ps2_gpu_shutdown();
        throw Error(ErrorKind::io, "PS2 GS has no VRAM available for texture tiles");
    }
    if (slot_count > kScratchSlotCount) {
        auto &font = slots[0];
        std::fill_n(font.memory, kTileSize * kTileSize, 0U);
        for (unsigned character = 0; character < 256; ++character) {
            const auto bitmap = font_bitmap(static_cast<unsigned char>(character));
            const int cell_x = static_cast<int>(character % 16) * 8;
            const int cell_y = static_cast<int>(character / 16) * 8;
            for (int y = 0; y < 7; ++y)
                for (int x = 0; x < 5; ++x)
                    if (bitmap[static_cast<std::size_t>(y)] & (1U << static_cast<unsigned>(4 - x)))
                        font.memory[static_cast<std::size_t>(cell_y + y) * kTileSize + cell_x + x] =
                            0x80ffffff;
        }
        gsKit_texture_upload(gs, &font.texture);
        font_atlas_ready = true;
    }
    gs->PrimAlphaEnable = GS_SETTING_ON;
    gs->PrimAAEnable = GS_SETTING_OFF;
    gsKit_set_primalpha(gs, GS_SETREG_ALPHA(0, 1, 0, 1, 128), 0);
    gsKit_set_test(gs, GS_ZTEST_OFF);
    frame_buffer = gs->ActiveBuffer & 1;
    last_presented_buffer = frame_buffer;
}

void ps2_gpu_shutdown() noexcept {
    for (auto &slot : slots) {
        std::free(slot.memory);
        slot = {};
    }
    std::free(raw_frame);
    std::free(canvas_frame);
    raw_frame = nullptr;
    canvas_frame = nullptr;
    raw_frame_bytes = 0;
    slot_count = 0;
    use_clock = 0;
    scratch_slot = 0;
    frame_active = false;
    readback_valid = false;
    font_atlas_ready = false;
    frame_profile = {};
    gs = nullptr;
    canvas_width = canvas_height = 0;
}

void ps2_gpu_profile(bool enabled) noexcept {
    profile_enabled = enabled;
}

Ps2GpuProfile ps2_gpu_take_profile() noexcept {
    const auto result = frame_profile;
    frame_profile = {};
    return result;
}

void ps2_gpu_begin() {
    if (!gs)
        return;
    frame_profile = {};
    if (!frame_active)
        frame_buffer = gs->ActiveBuffer & 1;
    frame_active = true;
    readback_valid = false;
    gs->PrimAlphaEnable = GS_SETTING_ON;
    if (viewport_x > 0 || viewport_y > 0 || viewport_x + canvas_width * viewport_scale < gs->Width ||
        viewport_y + canvas_height * viewport_scale < gs->Height) {
        gsKit_set_scissor(gs, GS_SCISSOR_RESET);
        gsKit_clear(gs, GS_SETREG_RGBAQ(0, 0, 0, 128, 0));
    }
    set_canvas_scissor();
}

void ps2_gpu_flush() {
    if (!gs)
        return;
    gsKit_queue_exec(gs);
    dmaKit_wait_fast();
    for (std::size_t i = 0; i < slot_count; ++i)
        slots[i].pending_draws = false;
}

void ps2_gpu_presented() noexcept {
    if (!gs)
        return;
    last_presented_buffer = frame_buffer;
    frame_active = false;
    readback_valid = false;
}

void ps2_gpu_box(double x, double y, double width, double height, std::uint32_t rgba) {
    if (!gs || width <= 0 || height <= 0 || !(rgba >> 24))
        return;
    const auto color = gs_color(rgba);
    gsKit_prim_sprite(gs, static_cast<float>(screen_x(x)), static_cast<float>(screen_y(y)),
                      static_cast<float>(screen_x(x + width)), static_cast<float>(screen_y(y + height)), 1,
                      color);
    readback_valid = false;
}

bool ps2_gpu_font_glyph(unsigned char character,
                        const std::array<double, 8> &corners,
                        std::uint32_t rgba) {
    if (!gs || !font_atlas_ready || !(rgba >> 24))
        return false;
    auto &font = slots[0];
    const float u0 = static_cast<float>((character % 16) * 8);
    const float v0 = static_cast<float>((character / 16) * 8);
    const float u1 = u0 + 5;
    const float v1 = v0 + 7;
    gsKit_prim_quad_texture_3d(gs, &font.texture,
                               static_cast<float>(screen_x(corners[0])),
                               static_cast<float>(screen_y(corners[1])), 1, u0, v0,
                               static_cast<float>(screen_x(corners[2])),
                               static_cast<float>(screen_y(corners[3])), 1, u1, v0,
                               static_cast<float>(screen_x(corners[4])),
                               static_cast<float>(screen_y(corners[5])), 1, u1, v1,
                               static_cast<float>(screen_x(corners[6])),
                               static_cast<float>(screen_y(corners[7])), 1, u0, v1,
                               gs_color(rgba));
    font.pending_draws = true;
    if (profile_enabled)
        ++frame_profile.glyph_quads;
    readback_valid = false;
    return true;
}

void ps2_gpu_circle(double x, double y, double radius, bool outline, std::uint32_t rgba) {
    if (!gs || radius <= 0 || !(rgba >> 24))
        return;
    constexpr int segments = 48;
    std::array<float, (segments + 2) * 2> points{};
    if (outline) {
        for (int i = 0; i <= segments; ++i) {
            const double angle = i * 6.28318530717958647692 / segments;
            points[static_cast<std::size_t>(i) * 2] =
                static_cast<float>(screen_x(x + std::cos(angle) * radius));
            points[static_cast<std::size_t>(i) * 2 + 1] =
                static_cast<float>(screen_y(y + std::sin(angle) * radius));
        }
        gsKit_prim_line_strip(gs, points.data(), segments + 1, 1, gs_color(rgba));
    } else {
        points[0] = static_cast<float>(screen_x(x));
        points[1] = static_cast<float>(screen_y(y));
        for (int i = 0; i <= segments; ++i) {
            const double angle = i * 6.28318530717958647692 / segments;
            points[static_cast<std::size_t>(i + 1) * 2] =
                static_cast<float>(screen_x(x + std::cos(angle) * radius));
            points[static_cast<std::size_t>(i + 1) * 2 + 1] =
                static_cast<float>(screen_y(y + std::sin(angle) * radius));
        }
        gsKit_prim_triangle_fan(gs, points.data(), segments + 2, 1, gs_color(rgba));
    }
    readback_valid = false;
}

void ps2_gpu_triangle(double x1, double y1, double x2, double y2, double x3, double y3, std::uint32_t rgba) {
    if (!gs || !(rgba >> 24))
        return;
    gsKit_prim_triangle_3d(gs, static_cast<float>(screen_x(x1)), static_cast<float>(screen_y(y1)), 1,
                           static_cast<float>(screen_x(x2)), static_cast<float>(screen_y(y2)), 1,
                           static_cast<float>(screen_x(x3)), static_cast<float>(screen_y(y3)), 1,
                           gs_color(rgba));
    readback_valid = false;
}

void ps2_gpu_quad(double x1,
                  double y1,
                  double x2,
                  double y2,
                  double x3,
                  double y3,
                  double x4,
                  double y4,
                  std::uint32_t rgba) {
    if (!gs || !(rgba >> 24))
        return;
    gsKit_prim_quad_3d(gs,
                       static_cast<float>(screen_x(x1)), static_cast<float>(screen_y(y1)), 1,
                       static_cast<float>(screen_x(x2)), static_cast<float>(screen_y(y2)), 1,
                       static_cast<float>(screen_x(x3)), static_cast<float>(screen_y(y3)), 1,
                       static_cast<float>(screen_x(x4)), static_cast<float>(screen_y(y4)), 1,
                       gs_color(rgba));
    readback_valid = false;
}

bool ps2_gpu_cached_image(int width,
                          int height,
                          double x,
                          double y,
                          double draw_width,
                          double draw_height,
                          double angle,
                          unsigned tint,
                          double opacity,
                          std::uint64_t key) {
    if (!gs || !key || width <= 0 || height <= 0)
        return false;
    const auto profile_start = profile_enabled ? std::chrono::steady_clock::now()
                                               : std::chrono::steady_clock::time_point{};
    const auto record_profile = [&] {
        if (profile_enabled) {
            frame_profile.image_ms += std::chrono::duration<double, std::milli>(
                                          std::chrono::steady_clock::now() - profile_start)
                                          .count();
            ++frame_profile.image_calls;
        }
    };
    if (opacity <= 0 || draw_width == 0 || draw_height == 0) {
        record_profile();
        return true;
    }
    double cs = 1, sn = 0;
    if (angle != 0) {
        const double radians = angle * 3.14159265358979323846 / 180.0;
        cs = std::cos(radians);
        sn = std::sin(radians);
    }
    std::size_t cached_count = 0;
    for (int tile_y = 0; tile_y < height; tile_y += kTileSize) {
        for (int tile_x = 0; tile_x < width; tile_x += kTileSize) {
            const int part_width = std::min(kTileSize, width - tile_x);
            const int part_height = std::min(kTileSize, height - tile_y);
            if (!tile_visible(tile_x, tile_y, part_width, part_height, width, height, x, y, draw_width,
                              draw_height, cs, sn, angle != 0))
                continue;
            if (cached_count == cached_image_tiles.size()) {
                record_profile();
                return false;
            }
            auto *slot = find_slot(tile_key(key, tile_x, tile_y));
            if (!slot) {
                record_profile();
                return false;
            }
            cached_image_tiles[cached_count++] = {slot, tile_x, tile_y, part_width, part_height,
                                                  tile_x, tile_y};
        }
    }
    const auto color = gs_tint(tint, opacity);
    for (std::size_t i = 0; i < cached_count; ++i) {
        const auto &tile = cached_image_tiles[i];
        draw_tile(*tile.slot, tile.source_x, tile.source_y, tile.width, tile.height, tile.tile_x,
                  tile.tile_y, 0, 0, width, height, x, y, draw_width, draw_height, angle, cs, sn, color);
        tile.slot->pending_draws = true;
    }
    readback_valid = false;
    record_profile();
    return true;
}

void ps2_gpu_image(std::span<const std::uint32_t> pixels,
                   int stride,
                   int left,
                   int top,
                   int width,
                   int height,
                   double x,
                   double y,
                   double draw_width,
                   double draw_height,
                   double angle,
                   unsigned tint,
                   double opacity,
                   std::uint64_t key) {
    image_impl(pixels, stride, left, top, width, height, x, y, draw_width, draw_height, angle, tint, opacity,
               key);
}

std::uint32_t ps2_gpu_pixel(int x, int y) noexcept {
    if (!gs || x < 0 || y < 0 || x >= canvas_width || y >= canvas_height)
        return 0xff000000;
    if (!readback_valid && !read_framebuffer())
        return 0xff000000;
    return canvas_frame[static_cast<std::size_t>(y) * canvas_width + x];
}

bool ps2_gpu_readback(std::span<std::uint32_t> pixels) noexcept {
    const auto count = static_cast<std::size_t>(canvas_width) * canvas_height;
    if (pixels.size() < count || (!readback_valid && !read_framebuffer()))
        return false;
    std::copy_n(canvas_frame, count, pixels.begin());
    return true;
}
} // namespace regml
#endif
