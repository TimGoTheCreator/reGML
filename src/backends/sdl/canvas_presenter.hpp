#pragma once

#if REGML_WITH_SDL2

#include <SDL2/SDL.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace regml {

class Canvas;

// Temporary bridge between the software Canvas renderer and an SDL display.
// Window ownership, input, audio, and event handling remain with the platform layer.
class SdlCanvasPresenter {
  public:
    explicit SdlCanvasPresenter(SDL_Window *window);
    ~SdlCanvasPresenter();

    SdlCanvasPresenter(const SdlCanvasPresenter &) = delete;
    SdlCanvasPresenter &operator=(const SdlCanvasPresenter &) = delete;

    void present(const Canvas &canvas);

  private:
    struct RendererDelete {
        void operator()(SDL_Renderer *renderer) const noexcept;
    };
    struct TextureDelete {
        void operator()(SDL_Texture *texture) const noexcept;
    };

    std::unique_ptr<SDL_Renderer, RendererDelete> renderer_;
    std::unique_ptr<SDL_Texture, TextureDelete> texture_;
    std::vector<std::uint32_t> present_pixels_;
};

} // namespace regml

#endif // REGML_WITH_SDL2
