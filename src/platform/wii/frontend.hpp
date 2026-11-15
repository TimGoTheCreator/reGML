#pragma once

#include "input.hpp"

namespace regml {

class WiiInput {
  public:
    bool poll();
    const KeyboardState &held() const noexcept { return held_; }
    KeyboardState take_pressed() noexcept;

  private:
    KeyboardState held_{}, pressed_{}, previous_{};
};

} // namespace regml
