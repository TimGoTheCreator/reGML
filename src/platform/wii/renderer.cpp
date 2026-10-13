#include "renderer.hpp"

#if REGML_WII
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <gccore.h>
#include <malloc.h>
#include <ogc/pad.h>
#include <wiiuse/wpad.h>

namespace regml {
namespace {
constexpr int width = 640;
constexpr int height = 480;
constexpr std::size_t fifo_size = 256 * 1024;
GXRModeObj *video_mode = nullptr;
GXTexObj frame_texture{};
Mtx44 projection{};
Mtx model_view{};

void map_button(KeyboardState &state, u32 buttons, u32 mask, int key) {
    if (buttons & mask)
        state[static_cast<std::size_t>(key)] = true;
}

} // namespace

WiiRenderer::WiiRenderer() {
    VIDEO_Init();
    video_mode = VIDEO_GetPreferredMode(nullptr);
    if (!video_mode)
        throw Error(ErrorKind::io, "could not select Wii video mode");

    for (auto &framebuffer : framebuffers_) {
        framebuffer = MEM_K0_TO_K1(SYS_AllocateFramebuffer(video_mode));
        if (!framebuffer)
            throw Error(ErrorKind::io, "could not allocate Wii framebuffer");
    }
    VIDEO_Configure(video_mode);
    VIDEO_SetNextFramebuffer(framebuffers_[0]);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (video_mode->viTVMode & VI_NON_INTERLACE)
        VIDEO_WaitVSync();

    fifo_ = memalign(32, fifo_size);
    if (!fifo_)
        throw Error(ErrorKind::io, "could not allocate GX command buffer");
    std::memset(fifo_, 0, fifo_size);
    GX_Init(fifo_, fifo_size);
    GX_SetCopyClear({0, 0, 0, 0xff}, 0x00ffffff);
    GX_SetViewport(0, 0, video_mode->fbWidth, video_mode->efbHeight, 0, 1);
    const auto yscale = GX_GetYScaleFactor(video_mode->efbHeight, video_mode->xfbHeight);
    const auto xfb_height = GX_SetDispCopyYScale(yscale);
    GX_SetScissor(0, 0, video_mode->fbWidth, video_mode->efbHeight);
    GX_SetDispCopySrc(0, 0, video_mode->fbWidth, video_mode->efbHeight);
    GX_SetDispCopyDst(video_mode->fbWidth, xfb_height);
    GX_SetCopyFilter(video_mode->aa, video_mode->sample_pattern, GX_TRUE, video_mode->vfilter);
    GX_SetFieldMode(video_mode->field_rendering,
                    (video_mode->viHeight == 2 * video_mode->xfbHeight) ? GX_ENABLE : GX_DISABLE);
    GX_SetPixelFmt(video_mode->aa ? GX_PF_RGB565_Z16 : GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetNumChans(1);
    GX_SetNumTexGens(1);
    GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);

    guOrtho(projection, 0, height - 1, 0, width - 1, 0, 300);
    guMtxIdentity(model_view);
    guMtxTransApply(model_view, model_view, 0, 0, -5);
    GX_LoadProjectionMtx(projection, GX_ORTHOGRAPHIC);
    GX_LoadPosMtxImm(model_view, GX_PNMTX0);

    texture_bytes_ = static_cast<std::size_t>(width) * height * 4;
    texture_pixels_ = memalign(32, texture_bytes_);
    if (!texture_pixels_)
        throw Error(ErrorKind::io, "could not allocate GX framebuffer texture");
    GX_InitTexObj(&frame_texture, texture_pixels_, width, height, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjLOD(&frame_texture, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
    GX_SetDispCopyGamma(GX_GM_1_0);
}

void WiiRenderer::present(const Canvas &canvas) {
    const auto &source = canvas.pixels();
    auto *destination = static_cast<u8 *>(texture_pixels_);
    std::size_t tile_offset = 0;
    for (int tile_y = 0; tile_y < height; tile_y += 4) {
        for (int tile_x = 0; tile_x < width; tile_x += 4) {
            auto *ar = destination + tile_offset;
            auto *gb = ar + 32;
            for (int y = 0; y < 4; ++y) {
                const auto row = static_cast<std::size_t>(tile_y + y) * width + tile_x;
                for (int x = 0; x < 4; ++x) {
                    const auto pixel = source[row + static_cast<std::size_t>(x)];
                    const auto index = static_cast<std::size_t>((y * 4 + x) * 2);
                    ar[index] = static_cast<u8>(pixel >> 24);
                    ar[index + 1] = static_cast<u8>(pixel >> 16);
                    gb[index] = static_cast<u8>(pixel >> 8);
                    gb[index + 1] = static_cast<u8>(pixel);
                }
            }
            tile_offset += 64;
        }
    }
    DCFlushRange(texture_pixels_, texture_bytes_);
    GX_InvalidateTexAll();
    GX_LoadTexObj(&frame_texture, GX_TEXMAP0);
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position2f32(0, 0);
    GX_TexCoord2f32(0, 0);
    GX_Position2f32(width, 0);
    GX_TexCoord2f32(1, 0);
    GX_Position2f32(width, height);
    GX_TexCoord2f32(1, 1);
    GX_Position2f32(0, height);
    GX_TexCoord2f32(0, 1);
    GX_End();
    GX_DrawDone();
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_TRUE);
    GX_CopyDisp(framebuffers_[framebuffer_], GX_TRUE);
    VIDEO_SetNextFramebuffer(framebuffers_[framebuffer_]);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    framebuffer_ ^= 1;
}

bool WiiInput::poll() {
    PAD_ScanPads();
    WPAD_ScanPads();
    held_.fill(false);
    pressed_.fill(false);
    const auto wii_held = WPAD_ButtonsHeld(0);
    const auto wii_down = WPAD_ButtonsDown(0);
    const auto pad_held = PAD_ButtonsHeld(0);
    const auto pad_down = PAD_ButtonsDown(0);
    map_button(held_, wii_held, WPAD_BUTTON_LEFT | WPAD_CLASSIC_BUTTON_LEFT, 37);
    map_button(held_, wii_held, WPAD_BUTTON_UP | WPAD_CLASSIC_BUTTON_UP, 38);
    map_button(held_, wii_held, WPAD_BUTTON_RIGHT | WPAD_CLASSIC_BUTTON_RIGHT, 39);
    map_button(held_, wii_held, WPAD_BUTTON_DOWN | WPAD_CLASSIC_BUTTON_DOWN, 40);
    map_button(held_, wii_held, WPAD_BUTTON_A | WPAD_CLASSIC_BUTTON_A, 'Z');
    map_button(held_, wii_held, WPAD_BUTTON_B | WPAD_CLASSIC_BUTTON_B, 'X');
    map_button(held_, wii_held, WPAD_BUTTON_1 | WPAD_BUTTON_2 | WPAD_CLASSIC_BUTTON_X | WPAD_CLASSIC_BUTTON_Y,
               'C');
    map_button(held_, wii_held, WPAD_BUTTON_PLUS | WPAD_CLASSIC_BUTTON_PLUS, 13);
    map_button(held_, wii_held, WPAD_BUTTON_LEFT, 37);
    map_button(held_, pad_held, PAD_BUTTON_LEFT, 37);
    map_button(held_, pad_held, PAD_BUTTON_UP, 38);
    map_button(held_, pad_held, PAD_BUTTON_RIGHT, 39);
    map_button(held_, pad_held, PAD_BUTTON_DOWN, 40);
    map_button(held_, pad_held, PAD_BUTTON_A, 'Z');
    map_button(held_, pad_held, PAD_BUTTON_B, 'X');
    map_button(held_, pad_held, PAD_BUTTON_X | PAD_BUTTON_Y, 'C');
    map_button(held_, pad_held, PAD_BUTTON_START, 13);
    for (std::size_t key = 0; key < held_.size(); ++key)
        pressed_[key] = held_[key] && !previous_[key];
    previous_ = held_;
    return (wii_down & (WPAD_BUTTON_HOME | WPAD_CLASSIC_BUTTON_HOME)) == 0;
}

KeyboardState WiiInput::take_pressed() noexcept {
    const auto result = pressed_;
    pressed_.fill(false);
    return result;
}
} // namespace regml
#endif
