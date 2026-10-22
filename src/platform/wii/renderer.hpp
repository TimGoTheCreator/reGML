#pragma once
#include "input.hpp"
#include "rendersoftware/canvas.hpp"
namespace regml {
class WiiRenderer {
  public:
    WiiRenderer();
    void present(const Canvas &canvas);

  private:
    void *fifo_ = nullptr;
    void *framebuffers_[2]{};
    void *texture_pixels_ = nullptr;
    std::size_t texture_bytes_ = 0;
    int framebuffer_ = 0;
};
class WiiInput {
  public:
    bool poll();
    const KeyboardState &held() const noexcept { return held_; }
    KeyboardState take_pressed() noexcept;

  private:
    KeyboardState held_{}, pressed_{}, previous_{};
};
} // namespace regml
