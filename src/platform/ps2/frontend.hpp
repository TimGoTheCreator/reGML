#pragma once

#include "demo/demo.hpp"
#include "rendersoftware/canvas.hpp"
#if REGML_PS2
#include <gsKit.h>
#endif

namespace regml {
class GameSession;
void initialize_ps2_storage();

class Ps2Frontend {
  public:
    explicit Ps2Frontend(bool hidden = false, bool escape_quits = true, bool speed_controls = false);
    ~Ps2Frontend();
    Ps2Frontend(const Ps2Frontend &) = delete;
    Ps2Frontend &operator=(const Ps2Frontend &) = delete;

    bool poll(double *speed_multiplier = nullptr);
    template <class Session> void tick(Session &session, bool autoplay) {
        const auto pressed = take_pressed();
        session.tick(autoplay ? demo_autoplay(session.frames()) : held_, autoplay ? KeyboardState{} : pressed);
    }
    void tick_game(GameSession &session, const KeyboardState *replay);
    void present(const Canvas &canvas);

  private:
    KeyboardState take_pressed() noexcept;
#if REGML_PS2
    GSGLOBAL *gs_ = nullptr;
    GSTEXTURE texture_{};
#else
    void *gs_ = nullptr;
#endif
    void *texture_memory_ = nullptr;
    KeyboardState held_{}, pressed_{}, previous_{};
    alignas(64) unsigned char pad_buffer_[256]{};
};
} // namespace regml
