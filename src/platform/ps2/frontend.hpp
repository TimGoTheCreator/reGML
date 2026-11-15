#pragma once

#include "backends/software/canvas.hpp"
#include "input.hpp"
#include <memory>
#if REGML_PS2
#include <gsKit.h>
#endif

namespace regml {
class GameSession;
#if REGML_PS2
namespace render {
class Ps2GsKitRenderer;
}
#endif
void initialize_ps2_storage();

class Ps2Frontend {
  public:
    explicit Ps2Frontend(bool hidden = false,
                         bool escape_quits = true,
                         bool speed_controls = false,
                         bool uncapped = false);
    ~Ps2Frontend();
    Ps2Frontend(const Ps2Frontend &) = delete;
    Ps2Frontend &operator=(const Ps2Frontend &) = delete;

    bool poll(double *speed_multiplier = nullptr);
    bool poll_error_dismiss();
    void tick_game(GameSession &session,
                   const KeyboardState *replay,
                   const KeyboardState *replay_pressed = nullptr);
    void present(const Canvas &canvas);

  private:
    KeyboardState take_pressed() noexcept;
#if REGML_PS2
    GSGLOBAL *gs_ = nullptr;
    std::unique_ptr<render::Ps2GsKitRenderer> renderer_;
#else
    void *gs_ = nullptr;
#endif
    KeyboardState held_{}, pressed_{}, previous_{};
    alignas(64) unsigned char pad_buffer_[256]{};
};
} // namespace regml
