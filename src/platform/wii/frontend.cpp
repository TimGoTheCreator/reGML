#include "platform/wii/frontend.hpp"

#if REGML_WII
#include <gccore.h>
#include <ogc/pad.h>
#include <wiiuse/wpad.h>

#include <cstddef>
#include <cstdint>

#if defined(__GNUC__) || defined(__clang__)
#define REGML_HOT __attribute__((hot))
#define REGML_LIKELY(x) __builtin_expect(!!(x), 1)
#else
#define REGML_HOT
#define REGML_LIKELY(x) (x)
#endif

namespace regml {
namespace {
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
bool input_initialized = false;
void ensure_controllers_initialized() noexcept {
    if (REGML_LIKELY(input_initialized))
        return;
    PAD_Init();
    WPAD_Init();
    WPAD_SetDataFormat(WPAD_CHAN_ALL, WPAD_FMT_BTNS_ACC);
    input_initialized = true;
}
} // namespace
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

        // deadzone for the GameCube stick
        const s8 gc_x = PAD_StickX(chan);
        const s8 gc_y = PAD_StickY(chan);
        stick_left = stick_left || (gc_x < -36);
        stick_right = stick_right || (gc_x > 36);
        stick_up = stick_up || (gc_y > 36);
        stick_down = stick_down || (gc_y < -36);

        // read analog input from the attached extensions
        const WPADData *wdata = WPAD_Data(chan);
        if (wdata) {
            if (wdata->exp.type == WPAD_EXP_NUNCHUK) {
                const int nx = static_cast<int>(wdata->exp.nunchuk.js.pos.x) -
                               static_cast<int>(wdata->exp.nunchuk.js.center.x);
                const int ny = static_cast<int>(wdata->exp.nunchuk.js.pos.y) -
                               static_cast<int>(wdata->exp.nunchuk.js.center.y);
                stick_left = stick_left || (nx < -36);
                stick_right = stick_right || (nx > 36);
                stick_up = stick_up || (ny > 36);
                stick_down = stick_down || (ny < -36);
            } else if (wdata->exp.type == WPAD_EXP_CLASSIC) {
                const int cx = static_cast<int>(wdata->exp.classic.ljs.pos.x) -
                               static_cast<int>(wdata->exp.classic.ljs.center.x);
                const int cy = static_cast<int>(wdata->exp.classic.ljs.pos.y) -
                               static_cast<int>(wdata->exp.classic.ljs.center.y);
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

    const bool next_cancel =
        ((wii_held_all & (WPAD_BUTTON_B | WPAD_CLASSIC_BUTTON_B | WPAD_NUNCHUK_BUTTON_C)) != 0u) ||
        ((pad_held_all & PAD_BUTTON_B) != 0u);

    const bool next_menu =
        ((wii_held_all & (WPAD_BUTTON_1 | WPAD_BUTTON_2 | WPAD_CLASSIC_BUTTON_X | WPAD_CLASSIC_BUTTON_Y)) !=
         0u) ||
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
