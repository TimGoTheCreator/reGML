#pragma once

#include <array>

namespace regml {
using KeyboardState = std::array<bool, 256>;
struct GamepadState {
    bool connected = false;
    std::array<bool, 16> buttons{};
    std::array<bool, 16> pressed{};
    std::array<double, 4> axes{};
};
}
