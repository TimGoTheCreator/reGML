#pragma once
#include "backends/software/canvas.hpp"
namespace regml {
class WiiRenderer {
  public:
    WiiRenderer();
    void present(const Canvas &canvas);
    // Present commands submitted directly to the GX framebuffer by an
    // IRenderer adapter. The Canvas overload remains the current frontend
    // entry point and continues to handle its software fallback.
    void present_gpu();

  private:
    void *fifo_ = nullptr;
    void *framebuffers_[2]{};
    void *texture_pixels_ = nullptr;
    std::size_t texture_bytes_ = 0;
    int framebuffer_ = 0;
};
} // namespace regml
