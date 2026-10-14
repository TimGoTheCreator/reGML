#include "renderer.hpp"
#include "gpu.hpp"
#include "lookup.hpp"

#if REGML_WII
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <gccore.h>
#include <malloc.h>
#include <ogc/pad.h>
#include <wiiuse/wpad.h>

#if defined(__GNUC__) || defined(__clang__)
#define REGML_ALWAYS_INLINE __attribute__((always_inline)) inline
#define REGML_HOT __attribute__((hot))
#define REGML_COLD __attribute__((cold))
#define REGML_LIKELY(x) __builtin_expect(!!(x), 1)
#define REGML_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define REGML_RESTRICT __restrict__
#else
#define REGML_ALWAYS_INLINE inline
#define REGML_HOT
#define REGML_COLD
#define REGML_LIKELY(x) (x)
#define REGML_UNLIKELY(x) (x)
#define REGML_RESTRICT
#endif

namespace regml {
namespace {

constexpr int kWidth = 640;
constexpr int kHeight = 480;
constexpr std::size_t kPixelCount = static_cast<std::size_t>(kWidth) * kHeight;
constexpr int kTilesX = kWidth / 4;
constexpr int kTilesY = kHeight / 4;
constexpr std::size_t kTotalTiles = static_cast<std::size_t>(kTilesX) * kTilesY;
constexpr std::size_t kTileBytes = 64;
constexpr std::size_t kTextureBytes = kPixelCount * 4;
constexpr std::size_t kFifoSize = 256 * 1024;
constexpr std::size_t kDispListSize = 128;

constexpr std::size_t kKeyLeft = 37;
constexpr std::size_t kKeyUp = 38;
constexpr std::size_t kKeyRight = 39;
constexpr std::size_t kKeyDown = 40;
constexpr std::size_t kKeyConfirm = 'Z';
constexpr std::size_t kKeyCancel = 'X';
constexpr std::size_t kKeyMenu = 'C';
constexpr std::size_t kKeyStart = 13;

constexpr std::size_t kMappedKeys[8] = {kKeyLeft,    kKeyUp,     kKeyRight, kKeyDown,
                                        kKeyConfirm, kKeyCancel, kKeyMenu,  kKeyStart};

REGML_ALWAYS_INLINE void ppc_dcbz(void *ptr) noexcept {
#ifdef __powerpc__
    __asm__ volatile("dcbz 0, %0" : : "r"(ptr) : "memory");
#else
    std::memset(ptr, 0, 32);
#endif
}

REGML_ALWAYS_INLINE void ppc_dcbst(const void *ptr) noexcept {
#ifdef __powerpc__
    __asm__ volatile("dcbst 0, %0" : : "r"(ptr) : "memory");
#else
    (void)ptr;
#endif
}

REGML_ALWAYS_INLINE void ppc_dcbt(const void *ptr) noexcept {
#ifdef __powerpc__
    __asm__ volatile("dcbt 0, %0" : : "r"(ptr));
#else
    (void)ptr;
#endif
}

REGML_ALWAYS_INLINE void ppc_sync() noexcept {
#ifdef __powerpc__
    __asm__ volatile("sync" : : : "memory");
#endif
}

REGML_ALWAYS_INLINE void ppc_eieio() noexcept {
#ifdef __powerpc__
    __asm__ volatile("eieio" : : : "memory");
#endif
}

REGML_ALWAYS_INLINE u32 pack_ar(u32 p0, u32 p1) noexcept {
#ifdef __powerpc__
    u32 out = p0;
    __asm__("rlwimi %0, %1, 16, 16, 31" : "+r"(out) : "r"(p1));
    return out;
#else
    return (p0 & 0xFFFF0000u) | (p1 >> 16);
#endif
}

REGML_ALWAYS_INLINE u32 pack_gb(u32 p0, u32 p1) noexcept {
#ifdef __powerpc__
    u32 out = p1;
    __asm__("rlwimi %0, %1, 16, 0, 15" : "+r"(out) : "r"(p0));
    return out;
#else
    return (p0 << 16) | (p1 & 0x0000FFFFu);
#endif
}

REGML_ALWAYS_INLINE bool is_aligned_32(const void *ptr) noexcept {
    return (reinterpret_cast<std::uintptr_t>(ptr) & 31u) == 0u;
}

REGML_ALWAYS_INLINE u64 compute_tile_signature(u32 r0_0, u32 r0_1, u32 r0_2, u32 r0_3, u32 r1_0, u32 r1_1, u32 r1_2,
                                               u32 r1_3, u32 r2_0, u32 r2_1, u32 r2_2, u32 r2_3, u32 r3_0, u32 r3_1,
                                               u32 r3_2, u32 r3_3) noexcept {
    u32 lo = r0_0;
    u32 hi = r0_1 ^ 0x9E3779B9u;

    lo = (lo * 1664525u) + r0_2;
    hi = (hi * 22695477u) ^ r0_3;
    lo = (lo ^ (hi >> 13)) + r1_0;
    hi = (hi ^ (lo << 7)) + r1_1;
    lo = (lo * 1664525u) + r1_2;
    hi = (hi * 22695477u) ^ r1_3;

    lo = (lo ^ (hi >> 11)) + r2_0;
    hi = (hi ^ (lo << 9)) + r2_1;
    lo = (lo * 1664525u) + r2_2;
    hi = (hi * 22695477u) ^ r2_3;
    lo = (lo ^ (hi >> 15)) + r3_0;
    hi = (hi ^ (lo << 5)) + r3_1;
    lo = (lo * 1664525u) + r3_2;
    hi = (hi * 22695477u) ^ r3_3;

    lo ^= (lo >> 16);
    hi ^= (hi >> 16);
    return (static_cast<u64>(hi) << 32) | static_cast<u64>(lo);
}

REGML_ALWAYS_INLINE bool tile_matches(const u32 *dst, u32 r0_0, u32 r0_1, u32 r0_2, u32 r0_3, u32 r1_0, u32 r1_1,
                                      u32 r1_2, u32 r1_3, u32 r2_0, u32 r2_1, u32 r2_2, u32 r2_3, u32 r3_0, u32 r3_1,
                                      u32 r3_2, u32 r3_3) noexcept {
    const u32 expected[16] = {pack_ar(r0_0, r0_1), pack_ar(r0_2, r0_3), pack_ar(r1_0, r1_1), pack_ar(r1_2, r1_3),
                              pack_ar(r2_0, r2_1), pack_ar(r2_2, r2_3), pack_ar(r3_0, r3_1), pack_ar(r3_2, r3_3),
                              pack_gb(r0_0, r0_1), pack_gb(r0_2, r0_3), pack_gb(r1_0, r1_1), pack_gb(r1_2, r1_3),
                              pack_gb(r2_0, r2_1), pack_gb(r2_2, r2_3), pack_gb(r3_0, r3_1), pack_gb(r3_2, r3_3)};
    return std::memcmp(dst, expected, sizeof(expected)) == 0;
}

struct WiiHardwareState {
    GXRModeObj *video_mode = nullptr;
    GXTexObj tex_obj{};
    u64 *tile_signatures = nullptr;
    bool signature_valid = false;

    alignas(32) u8 quad_disp_list[kDispListSize]{};
    u32 quad_disp_list_size = 0;

    Mtx44 projection{};
    Mtx model_view{};

    volatile u32 retrace_counter = 0;
    u32 last_presented_retrace = 0;
    bool input_initialized = false;
};

WiiHardwareState g_hw{};

void vi_post_retrace_callback(u32 retrace_count) {
    g_hw.retrace_counter = retrace_count;
}

struct AllocationRollbackGuard {
    void **fb0 = nullptr;
    void **fb1 = nullptr;
    void **fifo = nullptr;
    void **tex0 = nullptr;
    u64 **sig0 = nullptr;
    VIRetraceCallback previous_retrace_callback = nullptr;
    bool video_configured = false;
    bool callback_installed = false;
    bool committed = false;

    ~AllocationRollbackGuard() noexcept {
        if (committed)
            return;
        if (video_configured) {
            VIDEO_SetBlack(true);
            VIDEO_Flush();
            VIDEO_WaitVSync();
        }
        if (callback_installed)
            VIDEO_SetPostRetraceCallback(previous_retrace_callback);
        if (sig0 && *sig0) {
            std::free(*sig0);
            *sig0 = nullptr;
        }
        if (tex0 && *tex0) {
            std::free(*tex0);
            *tex0 = nullptr;
            g_hw.signature_valid = false;
        }
        if (fifo && *fifo) {
            std::free(*fifo);
            *fifo = nullptr;
        }
        if (fb1 && *fb1) {
            std::free(MEM_K1_TO_K0(*fb1));
            *fb1 = nullptr;
        }
        if (fb0 && *fb0) {
            std::free(MEM_K1_TO_K0(*fb0));
            *fb0 = nullptr;
        }
    }
};

REGML_ALWAYS_INLINE bool swizzle_single_tile(u32 *REGML_RESTRICT dst, u64 *REGML_RESTRICT sig_slot, bool sig_valid,
                                             u32 r0_0, u32 r0_1, u32 r0_2, u32 r0_3, u32 r1_0, u32 r1_1, u32 r1_2,
                                             u32 r1_3, u32 r2_0, u32 r2_1, u32 r2_2, u32 r2_3, u32 r3_0, u32 r3_1,
                                             u32 r3_2, u32 r3_3) noexcept {

    const u32 uniform_diff = ((r0_0 ^ r0_1) | (r0_0 ^ r0_2) | (r0_0 ^ r0_3)) |
                             ((r0_0 ^ r1_0) | (r0_0 ^ r1_1) | (r0_0 ^ r1_2) | (r0_0 ^ r1_3)) |
                             ((r0_0 ^ r2_0) | (r0_0 ^ r2_1) | (r0_0 ^ r2_2) | (r0_0 ^ r2_3)) |
                             ((r0_0 ^ r3_0) | (r0_0 ^ r3_1) | (r0_0 ^ r3_2) | (r0_0 ^ r3_3));

    if (REGML_LIKELY(uniform_diff == 0u)) {
        const u64 uniform_sig = (static_cast<u64>(r0_0) << 32) | (r0_0 ^ 0xA5A55A5Au);
        if (sig_slot) {
            if (sig_valid && *sig_slot == uniform_sig) {
                bool matches = true;
                const u32 ar_word = pack_ar(r0_0, r0_0);
                const u32 gb_word = pack_gb(r0_0, r0_0);
                for (int i = 0; i < 8; ++i) {
                    matches = matches && dst[i] == ar_word && dst[i + 8] == gb_word;
                }
                if (matches)
                    return false;
            }
            *sig_slot = uniform_sig;
        }

        if (r0_0 == 0u) {
            ppc_dcbz(dst);
            ppc_dcbz(dst + 8);
            return true;
        }

        const u32 ar_word = pack_ar(r0_0, r0_0);
        const u32 gb_word = pack_gb(r0_0, r0_0);

        ppc_dcbz(dst);
        dst[0] = ar_word;
        dst[1] = ar_word;
        dst[2] = ar_word;
        dst[3] = ar_word;
        dst[4] = ar_word;
        dst[5] = ar_word;
        dst[6] = ar_word;
        dst[7] = ar_word;

        ppc_dcbz(dst + 8);
        dst[8] = gb_word;
        dst[9] = gb_word;
        dst[10] = gb_word;
        dst[11] = gb_word;
        dst[12] = gb_word;
        dst[13] = gb_word;
        dst[14] = gb_word;
        dst[15] = gb_word;
        return true;
    }

    if (sig_slot) {
        const u64 sig = compute_tile_signature(r0_0, r0_1, r0_2, r0_3, r1_0, r1_1, r1_2, r1_3, r2_0, r2_1, r2_2, r2_3,
                                               r3_0, r3_1, r3_2, r3_3);
        if (sig_valid && *sig_slot == sig &&
            tile_matches(dst, r0_0, r0_1, r0_2, r0_3, r1_0, r1_1, r1_2, r1_3, r2_0, r2_1, r2_2, r2_3, r3_0, r3_1, r3_2,
                         r3_3)) {
            return false;
        }
        *sig_slot = sig;
    }

    ppc_dcbz(dst);
    dst[0] = pack_ar(r0_0, r0_1);
    dst[1] = pack_ar(r0_2, r0_3);
    dst[2] = pack_ar(r1_0, r1_1);
    dst[3] = pack_ar(r1_2, r1_3);
    dst[4] = pack_ar(r2_0, r2_1);
    dst[5] = pack_ar(r2_2, r2_3);
    dst[6] = pack_ar(r3_0, r3_1);
    dst[7] = pack_ar(r3_2, r3_3);

    ppc_dcbz(dst + 8);
    dst[8] = pack_gb(r0_0, r0_1);
    dst[9] = pack_gb(r0_2, r0_3);
    dst[10] = pack_gb(r1_0, r1_1);
    dst[11] = pack_gb(r1_2, r1_3);
    dst[12] = pack_gb(r2_0, r2_1);
    dst[13] = pack_gb(r2_2, r2_3);
    dst[14] = pack_gb(r3_0, r3_1);
    dst[15] = pack_gb(r3_2, r3_3);
    return true;
}

REGML_ALWAYS_INLINE void flush_changed_tiles(u32 *base, const bool *changed, std::size_t count) noexcept {
    std::size_t i = 0;
    while (i < count) {
        while (i < count && !changed[i])
            ++i;
        const auto start = i;
        while (i < count && changed[i])
            ++i;
        if (i != start)
            DCFlushRange(base + start * 16, (i - start) * kTileBytes);
    }
}

// Convert 640x480 RGBA pixels to GX tiled RGBA8.
REGML_HOT void swizzle_canvas_640x480(const u32 *REGML_RESTRICT src, u32 *REGML_RESTRICT dst, u64 *REGML_RESTRICT sigs,
                                      bool sig_valid) noexcept {

    std::size_t tile_idx = 0;

    for (int tile_y = 0; tile_y < kHeight; tile_y += 4) {
        const u32 *REGML_RESTRICT r0 = src + static_cast<std::size_t>(tile_y) * kWidth;
        const u32 *REGML_RESTRICT r1 = r0 + kWidth;
        const u32 *REGML_RESTRICT r2 = r1 + kWidth;
        const u32 *REGML_RESTRICT r3 = r2 + kWidth;

        ppc_dcbt(r0);
        ppc_dcbt(r1);
        ppc_dcbt(r2);
        ppc_dcbt(r3);
        ppc_dcbt(r0 + 8);
        ppc_dcbt(r1 + 8);
        ppc_dcbt(r2 + 8);
        ppc_dcbt(r3 + 8);

        for (int tile_x = 0; tile_x < kWidth; tile_x += 16) {
            ppc_dcbt(r0 + 16);
            ppc_dcbt(r1 + 16);
            ppc_dcbt(r2 + 16);
            ppc_dcbt(r3 + 16);
            ppc_dcbt(r0 + 24);
            ppc_dcbt(r1 + 24);
            ppc_dcbt(r2 + 24);
            ppc_dcbt(r3 + 24);

            const bool changed[4] = {
                swizzle_single_tile(dst, sigs ? (sigs + tile_idx) : nullptr, sig_valid, r0[0], r0[1], r0[2], r0[3],
                                    r1[0], r1[1], r1[2], r1[3], r2[0], r2[1], r2[2], r2[3], r3[0], r3[1], r3[2], r3[3]),

                swizzle_single_tile(dst + 16, sigs ? (sigs + tile_idx + 1) : nullptr, sig_valid, r0[4], r0[5], r0[6],
                                    r0[7], r1[4], r1[5], r1[6], r1[7], r2[4], r2[5], r2[6], r2[7], r3[4], r3[5], r3[6],
                                    r3[7]),

                swizzle_single_tile(dst + 32, sigs ? (sigs + tile_idx + 2) : nullptr, sig_valid, r0[8], r0[9], r0[10],
                                    r0[11], r1[8], r1[9], r1[10], r1[11], r2[8], r2[9], r2[10], r2[11], r3[8], r3[9],
                                    r3[10], r3[11]),

                swizzle_single_tile(dst + 48, sigs ? (sigs + tile_idx + 3) : nullptr, sig_valid, r0[12], r0[13], r0[14],
                                    r0[15], r1[12], r1[13], r1[14], r1[15], r2[12], r2[13], r2[14], r2[15], r3[12],
                                    r3[13], r3[14], r3[15])};
            flush_changed_tiles(dst, changed, 4);

            r0 += 16;
            r1 += 16;
            r2 += 16;
            r3 += 16;
            dst += 64;
            tile_idx += 4;
        }
    }
}

// Expand 320x240 pixels while converting them to GX tiled RGBA8.
REGML_HOT void swizzle_canvas_320x240(const u32 *REGML_RESTRICT src, u32 *REGML_RESTRICT dst, u64 *REGML_RESTRICT sigs,
                                      bool sig_valid) noexcept {

    constexpr int kSrcWidth = 320;
    constexpr int kSrcHeight = 240;
    std::size_t tile_idx = 0;
    u32 *dirty_start = nullptr;
    std::size_t dirty_tile_count = 0;

    for (int src_y = 0; src_y < kSrcHeight; src_y += 2) {
        const u32 *REGML_RESTRICT sr0 = src + static_cast<std::size_t>(src_y) * kSrcWidth;
        const u32 *REGML_RESTRICT sr1 = sr0 + kSrcWidth;

        ppc_dcbt(sr0);
        ppc_dcbt(sr1);

        for (int src_x = 0; src_x < kSrcWidth; src_x += 2) {
            const u32 p00 = sr0[0];
            const u32 p01 = sr0[1];
            const u32 p10 = sr1[0];
            const u32 p11 = sr1[1];

            const bool changed = swizzle_single_tile(dst, sigs ? (sigs + tile_idx) : nullptr, sig_valid, p00, p00, p01,
                                                     p01, p00, p00, p01, p01, p10, p10, p11, p11, p10, p10, p11, p11);
            if (changed) {
                if (dirty_tile_count == 0)
                    dirty_start = dst;
                ++dirty_tile_count;
            } else if (dirty_tile_count != 0) {
                DCFlushRange(dirty_start, dirty_tile_count * kTileBytes);
                dirty_tile_count = 0;
            }

            sr0 += 2;
            sr1 += 2;
            dst += 16;
            ++tile_idx;
        }
    }
    if (dirty_tile_count != 0)
        DCFlushRange(dirty_start, dirty_tile_count * kTileBytes);
}

// Scale the source canvas to 640x480.
REGML_COLD void swizzle_canvas_safe_fallback(const u32 *REGML_RESTRICT src, std::size_t src_count, int src_width,
                                             int src_height, u32 *REGML_RESTRICT dst,
                                             u64 *REGML_RESTRICT sigs) noexcept {

    if (sigs) {
        std::memset(sigs, 0, kTotalTiles * sizeof(u64));
    }

    if (REGML_UNLIKELY(!src || src_count == 0 || src_width < 1 || src_height < 1)) {
        for (std::size_t i = 0; i < kTotalTiles; ++i) {
            ppc_dcbz(dst);
            ppc_dcbz(dst + 8);
            dst += 16;
        }
        return;
    }

    for (int tile_y = 0; tile_y < kHeight; tile_y += 4) {
        for (int tile_x = 0; tile_x < kWidth; tile_x += 4) {
            u32 p[4][4];
            for (int y = 0; y < 4; ++y) {
                const std::size_t row_base =
                    static_cast<std::size_t>((tile_y + y) * src_height / kHeight) * static_cast<std::size_t>(src_width);
                for (int x = 0; x < 4; ++x) {
                    const int src_x = (tile_x + x) * src_width / kWidth;
                    const std::size_t idx = row_base + static_cast<std::size_t>(src_x);
                    p[y][x] = (idx < src_count) ? src[idx] : 0x00000000u;
                }
            }

            ppc_dcbz(dst);
            dst[0] = pack_ar(p[0][0], p[0][1]);
            dst[1] = pack_ar(p[0][2], p[0][3]);
            dst[2] = pack_ar(p[1][0], p[1][1]);
            dst[3] = pack_ar(p[1][2], p[1][3]);
            dst[4] = pack_ar(p[2][0], p[2][1]);
            dst[5] = pack_ar(p[2][2], p[2][3]);
            dst[6] = pack_ar(p[3][0], p[3][1]);
            dst[7] = pack_ar(p[3][2], p[3][3]);

            ppc_dcbz(dst + 8);
            dst[8] = pack_gb(p[0][0], p[0][1]);
            dst[9] = pack_gb(p[0][2], p[0][3]);
            dst[10] = pack_gb(p[1][0], p[1][1]);
            dst[11] = pack_gb(p[1][2], p[1][3]);
            dst[12] = pack_gb(p[2][0], p[2][1]);
            dst[13] = pack_gb(p[2][2], p[2][3]);
            dst[14] = pack_gb(p[3][0], p[3][1]);
            dst[15] = pack_gb(p[3][2], p[3][3]);

            dst += 16;
        }
    }
}

void compile_fullscreen_quad_display_list() noexcept {
    std::memset(g_hw.quad_disp_list, 0, sizeof(g_hw.quad_disp_list));
    DCInvalidateRange(g_hw.quad_disp_list, sizeof(g_hw.quad_disp_list));

    GX_BeginDispList(g_hw.quad_disp_list, sizeof(g_hw.quad_disp_list));
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position2s16(0, 0);
    GX_TexCoord2u8(0, 0);
    GX_Position2s16(kWidth, 0);
    GX_TexCoord2u8(1, 0);
    GX_Position2s16(kWidth, kHeight);
    GX_TexCoord2u8(1, 1);
    GX_Position2s16(0, kHeight);
    GX_TexCoord2u8(0, 1);
    GX_End();
    g_hw.quad_disp_list_size = GX_EndDispList();

    if (g_hw.quad_disp_list_size > 0) {
        DCFlushRange(g_hw.quad_disp_list, sizeof(g_hw.quad_disp_list));
    }
}

void configure_gx_pipeline(GXRModeObj *vmode) noexcept {
    GX_SetCopyClear({0, 0, 0, 0xff}, 0x00ffffff);
    GX_SetViewport(0.0f, 0.0f, static_cast<f32>(vmode->fbWidth), static_cast<f32>(vmode->efbHeight), 0.0f, 1.0f);
    const f32 yscale = GX_GetYScaleFactor(vmode->efbHeight, vmode->xfbHeight);
    const u32 xfb_height = GX_SetDispCopyYScale(yscale);
    GX_SetScissor(0, 0, vmode->fbWidth, vmode->efbHeight);
    GX_SetDispCopySrc(0, 0, vmode->fbWidth, vmode->efbHeight);
    GX_SetDispCopyDst(vmode->fbWidth, xfb_height);
    GX_SetCopyFilter(vmode->aa, vmode->sample_pattern, GX_TRUE, vmode->vfilter);
    GX_SetFieldMode(vmode->field_rendering, (vmode->viHeight == 2 * vmode->xfbHeight) ? GX_ENABLE : GX_DISABLE);
    GX_SetPixelFmt(vmode->aa ? GX_PF_RGB565_Z16 : GX_PF_RGB8_Z24, GX_ZC_LINEAR);

    GX_SetCullMode(GX_CULL_NONE);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetZCompLoc(GX_TRUE);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_COPY);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_TRUE);

    GX_SetNumChans(0);
    GX_SetNumTexGens(1);
    GX_SetNumTevStages(1);
    GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);

    GX_ClearVtxDesc();
    GX_InvVtxCache();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_S16, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_U8, 0);

    guOrtho(g_hw.projection, 0.0f, static_cast<f32>(kHeight), 0.0f, static_cast<f32>(kWidth), 0.0f, 300.0f);
    guMtxIdentity(g_hw.model_view);
    guMtxTransApply(g_hw.model_view, g_hw.model_view, 0.0f, 0.0f, -5.0f);
    GX_LoadProjectionMtx(g_hw.projection, GX_ORTHOGRAPHIC);
    GX_LoadPosMtxImm(g_hw.model_view, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);
    GX_SetDispCopyGamma(GX_GM_1_0);
}

void ensure_controllers_initialized() noexcept {
    if (REGML_LIKELY(g_hw.input_initialized))
        return;
    PAD_Init();
    WPAD_Init();
    WPAD_SetDataFormat(WPAD_CHAN_ALL, WPAD_FMT_BTNS_ACC);
    g_hw.input_initialized = true;
}

} // namespace

WiiRenderer::WiiRenderer() {
    AllocationRollbackGuard guard{};
    guard.fb0 = &framebuffers_[0];
    guard.fb1 = &framebuffers_[1];
    guard.fifo = &fifo_;
    guard.tex0 = &texture_pixels_;
    guard.sig0 = &g_hw.tile_signatures;

    VIDEO_Init();
    g_hw.video_mode = VIDEO_GetPreferredMode(nullptr);
    if (REGML_UNLIKELY(!g_hw.video_mode)) {
        throw Error(ErrorKind::io, "could not select Wii video mode");
    }

    if (REGML_UNLIKELY(g_hw.video_mode->fbWidth == 0 || g_hw.video_mode->efbHeight == 0 ||
                       g_hw.video_mode->xfbHeight == 0)) {
        throw Error(ErrorKind::io, "invalid Wii video mode dimensions");
    }

    for (auto &framebuffer : framebuffers_) {
        void *raw_fb = SYS_AllocateFramebuffer(g_hw.video_mode);
        if (REGML_UNLIKELY(!raw_fb)) {
            throw Error(ErrorKind::io, "could not allocate Wii framebuffer");
        }
        framebuffer = MEM_K0_TO_K1(raw_fb);
        VIDEO_ClearFrameBuffer(g_hw.video_mode, framebuffer, COLOR_BLACK);
    }

    VIDEO_Configure(g_hw.video_mode);
    VIDEO_SetNextFramebuffer(framebuffers_[0]);
    guard.previous_retrace_callback = VIDEO_SetPostRetraceCallback(vi_post_retrace_callback);
    guard.callback_installed = true;
    guard.video_configured = true;
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (g_hw.video_mode->viTVMode & VI_NON_INTERLACE) {
        VIDEO_WaitVSync();
    }

    fifo_ = memalign(32, kFifoSize);
    if (REGML_UNLIKELY(!fifo_ || !is_aligned_32(fifo_))) {
        throw Error(ErrorKind::io, "could not allocate 32-byte aligned GX command buffer");
    }
    std::memset(fifo_, 0, kFifoSize);
    DCFlushRange(fifo_, kFifoSize);
    GX_Init(fifo_, kFifoSize);

    configure_gx_pipeline(g_hw.video_mode);

    texture_bytes_ = kTextureBytes;
    texture_pixels_ = memalign(32, texture_bytes_);
    if (REGML_UNLIKELY(!texture_pixels_ || !is_aligned_32(texture_pixels_))) {
        throw Error(ErrorKind::io, "could not allocate 32-byte aligned GX framebuffer texture");
    }
    std::memset(texture_pixels_, 0, texture_bytes_);
    DCFlushRange(texture_pixels_, texture_bytes_);
    g_hw.tile_signatures = static_cast<u64 *>(memalign(32, kTotalTiles * sizeof(u64)));
    if (g_hw.tile_signatures) {
        std::memset(g_hw.tile_signatures, 0, kTotalTiles * sizeof(u64));
    }

    GX_InitTexObj(&g_hw.tex_obj, texture_pixels_, kWidth, kHeight, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjLOD(&g_hw.tex_obj, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);

    GX_LoadTexObj(&g_hw.tex_obj, GX_TEXMAP0);
    compile_fullscreen_quad_display_list();

    ensure_controllers_initialized();
    g_hw.retrace_counter = 0;
    g_hw.last_presented_retrace = g_hw.retrace_counter;
    g_hw.signature_valid = false;

    guard.committed = true;
}

REGML_HOT void WiiRenderer::present(const Canvas &canvas) {
    if (REGML_UNLIKELY(!framebuffers_[0] || !framebuffers_[1])) {
        return;
    }

    if (canvas.gpu_target()) {
#if REGML_240P
        GX_SetTexCopySrc(0, 0, render_width, render_height);
        GX_SetTexCopyDst(render_width, render_height, GX_TF_RGBA8, GX_FALSE);
        GX_SetCopyFilter(GX_FALSE, g_hw.video_mode->sample_pattern, GX_FALSE, g_hw.video_mode->vfilter);
        GX_CopyTex(texture_pixels_, GX_FALSE);
        GX_PixModeSync();
        configure_gx_pipeline(g_hw.video_mode);
        GX_InitTexObj(&g_hw.tex_obj, texture_pixels_, render_width, render_height, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP,
                      GX_FALSE);
        GX_InitTexObjLOD(&g_hw.tex_obj, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
        GX_InvalidateTexAll();
        GX_LoadTexObj(&g_hw.tex_obj, GX_TEXMAP0);
        GX_CallDispList(g_hw.quad_disp_list, g_hw.quad_disp_list_size);
#else
        if (texture_pixels_) {
            GX_DrawDone();
            std::free(texture_pixels_);
            texture_pixels_ = nullptr;
            std::free(g_hw.tile_signatures);
            g_hw.tile_signatures = nullptr;
        }
#endif
        const int slot = framebuffer_ & 1;
        GX_CopyDisp(framebuffers_[slot], GX_FALSE);
        GX_DrawDone();
        wii_gpu_presented();
        VIDEO_SetNextFramebuffer(framebuffers_[slot]);
        VIDEO_Flush();
        VIDEO_WaitVSync();
        framebuffer_ = slot ^ 1;
        return;
    }
    if (!texture_pixels_)
        return;
    void *target_tex_mem = texture_pixels_;
    u64 *target_sigs = g_hw.tile_signatures;
    const bool sig_valid = g_hw.signature_valid && (target_sigs != nullptr);

    const auto &source = canvas.pixels();
    const std::size_t source_count = source.size();
    const u32 *REGML_RESTRICT src_ptr = (source_count > 0) ? &source[0] : nullptr;
    u32 *REGML_RESTRICT dst_ptr = static_cast<u32 *>(target_tex_mem);

    if (REGML_LIKELY(src_ptr != nullptr && canvas.width() == kWidth && canvas.height() == kHeight &&
                     source_count >= kPixelCount && is_aligned_32(dst_ptr))) {
        swizzle_canvas_640x480(src_ptr, dst_ptr, target_sigs, sig_valid);
        g_hw.signature_valid = target_sigs != nullptr;
    } else if (src_ptr != nullptr && canvas.width() == 320 && canvas.height() == 240 && source_count >= (320u * 240u) &&
               is_aligned_32(dst_ptr)) {
        swizzle_canvas_320x240(src_ptr, dst_ptr, target_sigs, sig_valid);
        g_hw.signature_valid = target_sigs != nullptr;
    } else if (is_aligned_32(dst_ptr)) {
        swizzle_canvas_safe_fallback(src_ptr, source_count, canvas.width(), canvas.height(), dst_ptr, target_sigs);
        DCFlushRange(target_tex_mem, texture_bytes_);
        g_hw.signature_valid = false;
    } else {
        // Handle an unexpected unaligned texture buffer.
        for (int tile_y = 0; tile_y < kHeight; tile_y += 4) {
            for (int tile_x = 0; tile_x < kWidth; tile_x += 4) {
                u8 *ar = reinterpret_cast<u8 *>(dst_ptr);
                u8 *gb = ar + 32;
                for (int y = 0; y < 4; ++y) {
                    const std::size_t row =
                        static_cast<std::size_t>(tile_y + y) * kWidth + static_cast<std::size_t>(tile_x);
                    for (int x = 0; x < 4; ++x) {
                        const std::size_t idx = row + static_cast<std::size_t>(x);
                        const u32 pixel = (src_ptr && idx < source_count) ? src_ptr[idx] : 0u;
                        const std::size_t out_idx = static_cast<std::size_t>(y) * 8 + static_cast<std::size_t>(x) * 2;
                        ar[out_idx] = static_cast<u8>(pixel >> 24);
                        ar[out_idx + 1] = static_cast<u8>(pixel >> 16);
                        gb[out_idx] = static_cast<u8>(pixel >> 8);
                        gb[out_idx + 1] = static_cast<u8>(pixel);
                    }
                }
                dst_ptr += 16;
            }
        }
        DCFlushRange(target_tex_mem, texture_bytes_);
        g_hw.signature_valid = false;
    }

    // Make CPU writes visible to GX.
    ppc_sync();
    ppc_eieio();

    GX_LoadTexObj(&g_hw.tex_obj, GX_TEXMAP0);
    GX_InvalidateTexAll();

    // Draw the fullscreen quad.
    if (REGML_LIKELY(g_hw.quad_disp_list_size > 0)) {
        GX_CallDispList(g_hw.quad_disp_list, g_hw.quad_disp_list_size);
    } else {
        GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
        GX_Position2s16(0, 0);
        GX_TexCoord2u8(0, 0);
        GX_Position2s16(kWidth, 0);
        GX_TexCoord2u8(1, 0);
        GX_Position2s16(kWidth, kHeight);
        GX_TexCoord2u8(1, 1);
        GX_Position2s16(0, kHeight);
        GX_TexCoord2u8(0, 1);
        GX_End();
    }

    const int fb_slot = framebuffer_ & 1;
    GX_CopyDisp(framebuffers_[fb_slot], GX_FALSE);
    GX_Flush();
    GX_DrawDone();

    VIDEO_SetNextFramebuffer(framebuffers_[fb_slot]);
    VIDEO_Flush();

    // Wait for the next retrace if rendering missed it.
    const u32 current_retrace = g_hw.retrace_counter;
    if (current_retrace == g_hw.last_presented_retrace) {
        VIDEO_WaitVSync();
    }
    g_hw.last_presented_retrace = g_hw.retrace_counter;
    framebuffer_ = fb_slot ^ 1;
}

REGML_HOT bool WiiInput::poll() {
    ensure_controllers_initialized();

    PAD_ScanPads();
    WPAD_ScanPads();

    u32 wii_held_all = 0;
    u32 wii_down_all = 0;
    u32 pad_held_all = 0;

    bool stick_left = false;
    bool stick_right = false;
    bool stick_up = false;
    bool stick_down = false;

    for (int chan = 0; chan < 4; ++chan) {
        wii_held_all |= WPAD_ButtonsHeld(chan);
        wii_down_all |= WPAD_ButtonsDown(chan);
        pad_held_all |= PAD_ButtonsHeld(chan);

        // GameCube stick deadzone.
        const s8 gc_x = PAD_StickX(chan);
        const s8 gc_y = PAD_StickY(chan);
        stick_left = stick_left || (gc_x < -36);
        stick_right = stick_right || (gc_x > 36);
        stick_up = stick_up || (gc_y > 36);
        stick_down = stick_down || (gc_y < -36);

        // Read analog input from attached extensions.
        const WPADData *wdata = WPAD_Data(chan);
        if (wdata) {
            if (wdata->exp.type == WPAD_EXP_NUNCHUK) {
                const int nx =
                    static_cast<int>(wdata->exp.nunchuk.js.pos.x) - static_cast<int>(wdata->exp.nunchuk.js.center.x);
                const int ny =
                    static_cast<int>(wdata->exp.nunchuk.js.pos.y) - static_cast<int>(wdata->exp.nunchuk.js.center.y);
                stick_left = stick_left || (nx < -36);
                stick_right = stick_right || (nx > 36);
                stick_up = stick_up || (ny > 36);
                stick_down = stick_down || (ny < -36);
            } else if (wdata->exp.type == WPAD_EXP_CLASSIC) {
                const int cx =
                    static_cast<int>(wdata->exp.classic.ljs.pos.x) - static_cast<int>(wdata->exp.classic.ljs.center.x);
                const int cy =
                    static_cast<int>(wdata->exp.classic.ljs.pos.y) - static_cast<int>(wdata->exp.classic.ljs.center.y);
                stick_left = stick_left || (cx < -12);
                stick_right = stick_right || (cx > 12);
                stick_up = stick_up || (cy > 12);
                stick_down = stick_down || (cy < -12);
            }
        }
    }

    const bool next_left = ((wii_held_all & (WPAD_BUTTON_LEFT | WPAD_CLASSIC_BUTTON_LEFT)) != 0u) ||
                           ((pad_held_all & PAD_BUTTON_LEFT) != 0u) || stick_left;

    const bool next_up = ((wii_held_all & (WPAD_BUTTON_UP | WPAD_CLASSIC_BUTTON_UP)) != 0u) ||
                         ((pad_held_all & PAD_BUTTON_UP) != 0u) || stick_up;

    const bool next_right = ((wii_held_all & (WPAD_BUTTON_RIGHT | WPAD_CLASSIC_BUTTON_RIGHT)) != 0u) ||
                            ((pad_held_all & PAD_BUTTON_RIGHT) != 0u) || stick_right;

    const bool next_down = ((wii_held_all & (WPAD_BUTTON_DOWN | WPAD_CLASSIC_BUTTON_DOWN)) != 0u) ||
                           ((pad_held_all & PAD_BUTTON_DOWN) != 0u) || stick_down;

    const bool next_confirm =
        ((wii_held_all & (WPAD_BUTTON_A | WPAD_CLASSIC_BUTTON_A | WPAD_NUNCHUK_BUTTON_Z)) != 0u) ||
        ((pad_held_all & PAD_BUTTON_A) != 0u);

    const bool next_cancel = ((wii_held_all & (WPAD_BUTTON_B | WPAD_CLASSIC_BUTTON_B | WPAD_NUNCHUK_BUTTON_C)) != 0u) ||
                             ((pad_held_all & PAD_BUTTON_B) != 0u);

    const bool next_menu =
        ((wii_held_all & (WPAD_BUTTON_1 | WPAD_BUTTON_2 | WPAD_CLASSIC_BUTTON_X | WPAD_CLASSIC_BUTTON_Y)) != 0u) ||
        ((pad_held_all & (PAD_BUTTON_X | PAD_BUTTON_Y)) != 0u);

    const bool next_start = ((wii_held_all & (WPAD_BUTTON_PLUS | WPAD_CLASSIC_BUTTON_PLUS)) != 0u) ||
                            ((pad_held_all & PAD_BUTTON_START) != 0u);

    held_[kKeyLeft] = next_left;
    held_[kKeyUp] = next_up;
    held_[kKeyRight] = next_right;
    held_[kKeyDown] = next_down;
    held_[kKeyConfirm] = next_confirm;
    held_[kKeyCancel] = next_cancel;
    held_[kKeyMenu] = next_menu;
    held_[kKeyStart] = next_start;

    for (const std::size_t key : kMappedKeys) {
        const bool is_held = held_[key];
        pressed_[key] = is_held && !previous_[key];
        previous_[key] = is_held;
    }

    return (wii_down_all & (WPAD_BUTTON_HOME | WPAD_CLASSIC_BUTTON_HOME)) == 0u;
}

KeyboardState WiiInput::take_pressed() noexcept {
    KeyboardState result{};
    for (const std::size_t key : kMappedKeys) {
        result[key] = pressed_[key];
        pressed_[key] = false;
    }
    return result;
}

} // namespace regml
#endif
