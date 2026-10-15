#include "frontend.hpp"

#if REGML_PS2
#include "error.hpp"
#include "game.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dmaKit.h>
#include <gsKit.h>
#include <libpad.h>
#include <loadfile.h>
#include <iopcontrol.h>
#include <malloc.h>
#include <sbv_patches.h>
#include <sifrpc.h>
#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>

extern "C" {
int __iomanX_id = -1;
int __fileXio_id = -1;
extern unsigned char iomanX_irx[];
extern unsigned int size_iomanX_irx;
extern unsigned char fileXio_irx[];
extern unsigned int size_fileXio_irx;
extern unsigned char usbd_irx[];
extern unsigned int size_usbd_irx;
extern unsigned char usbmass_bd_irx[];
extern unsigned int size_usbmass_bd_irx;
}

namespace regml {
namespace {
#if REGML_240P
constexpr int kTextureWidth = 512;
constexpr int kTextureHeight = 256;
#else
constexpr int kTextureWidth = 1024;
constexpr int kTextureHeight = 512;
#endif
constexpr int kDisplayWidth = 640;
constexpr int kDisplayHeight = 448;
constexpr std::size_t kTexturePixels = static_cast<std::size_t>(kTextureWidth) * kTextureHeight;
constexpr std::size_t kTextureBytes = kTexturePixels * sizeof(std::uint16_t);
constexpr std::size_t kMappedKeys[] = {37, 38, 39, 40, 'Z', 'X', 'C', 13};

std::uint16_t rgba5551(std::uint32_t argb) noexcept {
    const auto red = (argb >> 19) & 0x1f;
    const auto green = (argb >> 11) & 0x1f;
    const auto blue = (argb >> 3) & 0x1f;
    return static_cast<std::uint16_t>(0x8000u | red | (green << 5) | (blue << 10));
}
} // namespace

void initialize_ps2_storage() {
    while (!SifIopReset(nullptr, 0)) {
    }
    while (!SifIopSync()) {
    }
    SifInitRpc(0);
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();
    const auto load_irx = [](unsigned char *data, unsigned int size, const char *name, bool required = true) {
        if (SifExecModuleBuffer(data, size, 0, nullptr, nullptr) < 0 && required)
            throw Error(ErrorKind::io, std::string("PS2 IOP module failed: ") + name);
    };
    load_irx(iomanX_irx, size_iomanX_irx, "IOMANX");
    load_irx(fileXio_irx, size_fileXio_irx, "FILEXIO");
    load_irx(usbd_irx, size_usbd_irx, "USBD", false);
    load_irx(usbmass_bd_irx, size_usbmass_bd_irx, "USB mass storage", false);
    if (fileXioInit() < 0)
        throw Error(ErrorKind::io, "PS2 fileXio initialization failed");
}

Ps2Frontend::Ps2Frontend(bool hidden, bool escape_quits, bool speed_controls) {
    (void)hidden;
    (void)escape_quits;
    (void)speed_controls;
    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8,
                1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);
    auto *gs = gsKit_init_global();
    if (!gs)
        throw Error(ErrorKind::io, "gsKit initialization failed");
    gs_ = gs;
    gs->ZBuffering = GS_SETTING_OFF;
    gs->DoubleBuffering = GS_SETTING_ON;
    gs->PSM = GS_PSM_CT16;
    gs->PSMZ = GS_PSMZ_16;
    gs->Mode = gsKit_detect_signal();
    gsKit_init_screen(gs);
    gsKit_mode_switch(gs, GS_ONESHOT);

    auto *pixels = static_cast<std::uint16_t *>(memalign(64, kTextureBytes));
    if (!pixels)
        throw Error(ErrorKind::io, "PS2 framebuffer texture allocation failed");
    texture_memory_ = pixels;
    std::memset(pixels, 0, kTextureBytes);
    texture_.Width = kTextureWidth;
    texture_.Height = kTextureHeight;
    texture_.PSM = GS_PSM_CT16;
    texture_.TBW = kTextureWidth / 64;
    texture_.Mem = static_cast<u32 *>(texture_memory_);
    texture_.Filter = GS_FILTER_NEAREST;
    texture_.Vram = gsKit_vram_alloc(gs, gsKit_texture_size(kTextureWidth, kTextureHeight, GS_PSM_CT16),
                                     GSKIT_ALLOC_USERBUFFER);
    if (texture_.Vram == GSKIT_ALLOC_ERROR)
        throw Error(ErrorKind::io, "PS2 framebuffer texture VRAM allocation failed");

    if (SifLoadModule("rom0:SIO2MAN", 0, nullptr) < 0 || SifLoadModule("rom0:PADMAN", 0, nullptr) < 0 ||
        padInit(0) != 1 || padPortOpen(0, 0, pad_buffer_) == 0)
        throw Error(ErrorKind::io, "PS2 controller initialization failed");
}

Ps2Frontend::~Ps2Frontend() {
    if (gs_)
        gsKit_deinit_global(gs_);
    std::free(texture_memory_);
}

bool Ps2Frontend::poll(double *speed_multiplier) {
    (void)speed_multiplier;
    padButtonStatus state{};
    const bool connected = padGetState(0, 0) == PAD_STATE_STABLE && padRead(0, 0, &state) != 0;
    const std::uint16_t buttons = connected ? state.btns : 0xffffu;
    const auto down = [buttons](std::uint16_t mask) { return (buttons & mask) == 0; };
    const bool analog_left = connected && state.ljoy_h < 72;
    const bool analog_right = connected && state.ljoy_h > 184;
    const bool analog_up = connected && state.ljoy_v < 72;
    const bool analog_down = connected && state.ljoy_v > 184;

    held_.fill(false);
    held_[37] = down(PAD_LEFT) || analog_left;
    held_[38] = down(PAD_UP) || analog_up;
    held_[39] = down(PAD_RIGHT) || analog_right;
    held_[40] = down(PAD_DOWN) || analog_down;
    held_['Z'] = down(PAD_CROSS);
    held_['X'] = down(PAD_CIRCLE);
    held_['C'] = down(PAD_SQUARE) || down(PAD_TRIANGLE);
    held_[13] = down(PAD_START);

    for (const std::size_t key : kMappedKeys) {
        pressed_[key] = held_[key] && !previous_[key];
        previous_[key] = held_[key];
    }
    return !(down(PAD_L3) && down(PAD_R3));
}

KeyboardState Ps2Frontend::take_pressed() noexcept {
    KeyboardState result{};
    for (const std::size_t key : kMappedKeys) {
        result[key] = pressed_[key];
        pressed_[key] = false;
    }
    return result;
}

void Ps2Frontend::tick_game(GameSession &session, const KeyboardState *replay) {
    const auto pressed = take_pressed();
    session.tick(replay ? *replay : held_, replay ? KeyboardState{} : pressed);
}

void Ps2Frontend::present(const Canvas &canvas) {
    auto *pixels = static_cast<std::uint16_t *>(texture_memory_);
    const auto source = canvas.pixels();
    const auto width = std::min(canvas.width(), kTextureWidth);
    const auto height = std::min(canvas.height(), kTextureHeight);
    for (int y = 0; y < kTextureHeight; ++y) {
        auto *row = pixels + static_cast<std::size_t>(y) * kTextureWidth;
        if (y < height) {
            const auto *src = source.data() + static_cast<std::size_t>(y) * canvas.width();
            for (int x = 0; x < width; ++x)
                row[x] = rgba5551(src[x]);
            std::fill(row + width, row + kTextureWidth, 0);
        } else {
            std::fill(row, row + kTextureWidth, 0);
        }
    }

    gsKit_texture_upload(gs_, &texture_);

    const float scale = std::min(static_cast<float>(kDisplayWidth) / canvas.width(),
                                 static_cast<float>(kDisplayHeight) / canvas.height());
    const float draw_width = canvas.width() * scale;
    const float draw_height = canvas.height() * scale;
    const float left = (kDisplayWidth - draw_width) * 0.5f;
    const float top = (kDisplayHeight - draw_height) * 0.5f;
    gsKit_clear(gs_, GS_SETREG_RGBAQ(0, 0, 0, 0x80, 0));
    gsKit_prim_sprite_texture(gs_, &texture_, left, top, 0.0f, 0.0f, left + draw_width, top + draw_height,
                              static_cast<float>(canvas.width()), static_cast<float>(canvas.height()), 1,
                              GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0));
    gsKit_queue_exec(gs_);
    gsKit_sync_flip(gs_);
}
} // namespace regml
#endif
