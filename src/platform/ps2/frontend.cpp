#include "frontend.hpp"

#if REGML_PS2
#include "backends/ps2_gskit/renderer.hpp"
#include "error.hpp"
#include "game.hpp"
#include <algorithm>
#include <audsrv.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dmaKit.h>
#include <gsKit.h>
#include <iopcontrol.h>
#include <libpad.h>
#include <loadfile.h>
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
extern unsigned char audsrv_irx[];
extern unsigned int size_audsrv_irx;
}

namespace regml {
namespace {
constexpr std::size_t kMappedKeys[] = {37, 38, 39, 40, 'Z', 'X', 'C', 13};
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
    if (SifLoadModule("rom0:LIBSD", 0, nullptr) < 0)
        throw Error(ErrorKind::io, "PS2 sound module failed to load");
    load_irx(audsrv_irx, size_audsrv_irx, "AUDSRV");
}

Ps2Frontend::Ps2Frontend(bool hidden, bool escape_quits, bool speed_controls, bool uncapped) {
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
#if REGML_PS2_NTSC
    gs->Mode = GS_MODE_NTSC;
#else
    gs->Mode = gsKit_detect_signal();
#endif
    gsKit_init_screen(gs);
    gsKit_mode_switch(gs, GS_ONESHOT);

    if (SifLoadModule("rom0:SIO2MAN", 0, nullptr) < 0 || SifLoadModule("rom0:PADMAN", 0, nullptr) < 0 ||
        padInit(0) != 1 || padPortOpen(0, 0, pad_buffer_) == 0)
        throw Error(ErrorKind::io, "PS2 controller initialization failed");

    renderer_ = std::make_unique<render::Ps2GsKitRenderer>(gs_, uncapped);
}

Ps2Frontend::~Ps2Frontend() {
    renderer_.reset();
    if (gs_)
        gsKit_deinit_global(gs_);
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

bool Ps2Frontend::poll_error_dismiss() {
    if (!poll())
        return true;
    const auto pressed = take_pressed();
    for (const bool key : pressed)
        if (key)
            return true;
    return false;
}

KeyboardState Ps2Frontend::take_pressed() noexcept {
    KeyboardState result{};
    for (const std::size_t key : kMappedKeys) {
        result[key] = pressed_[key];
        pressed_[key] = false;
    }
    return result;
}

void Ps2Frontend::tick_game(GameSession &session,
                            const KeyboardState *replay,
                            const KeyboardState *replay_pressed) {
    const auto width = session.canvas().width();
    const auto height = session.canvas().height();
    const bool accelerated =
        renderer_ &&
        renderer_->begin_frame({{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)}, 0});
    session.canvas().gpu_target(accelerated);
    const auto pressed = take_pressed();
    session.tick(replay ? *replay : held_,
                 replay ? (replay_pressed ? *replay_pressed : KeyboardState{}) : pressed);
}

void Ps2Frontend::present(const Canvas &canvas) {
    if (canvas.gpu_target()) {
        if (renderer_ && renderer_->end_frame())
            renderer_->present();
        return;
    }
    if (renderer_)
        renderer_->present_canvas(canvas);
}
} // namespace regml
#endif
