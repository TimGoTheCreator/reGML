#include "backends/sdl/canvas_presenter.hpp"

#if REGML_WITH_SDL2

#include "error.hpp"
#include "lookup.hpp"
#include "backends/software/canvas.hpp"

#include <string>
#include <string_view>

namespace regml {
namespace {
void sdl_ok(int status, std::string_view operation) {
    if (status < 0)
        throw Error(ErrorKind::io, std::string(operation) + ": " + SDL_GetError());
}
} // namespace

void SdlCanvasPresenter::RendererDelete::operator()(SDL_Renderer *renderer) const noexcept {
    if (renderer)
        SDL_DestroyRenderer(renderer);
}

void SdlCanvasPresenter::TextureDelete::operator()(SDL_Texture *texture) const noexcept {
    if (texture)
        SDL_DestroyTexture(texture);
}

SdlCanvasPresenter::SdlCanvasPresenter(SDL_Window *window) {
    if (!window)
        throw Error(ErrorKind::usage, "SDL presenter requires a window");

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    // Prefer a vsynced accelerated renderer, while keeping the existing accelerated
    // and software fallbacks for platforms or drivers that do not support it.
    renderer_.reset(
        SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC));
    if (!renderer_)
        renderer_.reset(SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED));
    if (!renderer_)
        renderer_.reset(SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE | SDL_RENDERER_PRESENTVSYNC));
    if (!renderer_)
        renderer_.reset(SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE));
    if (!renderer_)
        throw Error(ErrorKind::io, std::string("renderer creation: ") + SDL_GetError());

    sdl_ok(SDL_RenderSetLogicalSize(renderer_.get(), 640, 480), "logical resolution");
    texture_.reset(SDL_CreateTexture(renderer_.get(), SDL_PIXELFORMAT_ARGB8888,
                                     SDL_TEXTUREACCESS_STREAMING, render_width, render_height));
    if (!texture_)
        throw Error(ErrorKind::io, std::string("framebuffer texture: ") + SDL_GetError());
    sdl_ok(SDL_SetTextureBlendMode(texture_.get(), SDL_BLENDMODE_NONE), "texture blend mode");
}

SdlCanvasPresenter::~SdlCanvasPresenter() = default;

void SdlCanvasPresenter::present(const Canvas &canvas) {
    const auto *pixels = canvas.pixels().data();
    int pitch = canvas.width() * static_cast<int>(sizeof(std::uint32_t));
    if (canvas.width() != render_width || canvas.height() != render_height) {
        present_pixels_.resize(static_cast<std::size_t>(render_width) * render_height);
        for (int y = 0; y < render_height; ++y) {
            const int source_y =
                static_cast<int>(static_cast<std::int64_t>(y) * canvas.height() / render_height);
            const auto *source_row =
                canvas.pixels().data() + static_cast<std::size_t>(source_y) * canvas.width();
            auto *target_row = present_pixels_.data() + static_cast<std::size_t>(y) * render_width;
            for (int x = 0; x < render_width; ++x) {
                const int source_x =
                    static_cast<int>(static_cast<std::int64_t>(x) * canvas.width() / render_width);
                target_row[x] = source_row[source_x];
            }
        }
        pixels = present_pixels_.data();
        pitch = render_width * static_cast<int>(sizeof(std::uint32_t));
    }
    sdl_ok(SDL_UpdateTexture(texture_.get(), nullptr, pixels, pitch), "texture upload");
    sdl_ok(SDL_SetRenderDrawColor(renderer_.get(), 0, 0, 0, 255), "clear color");
    sdl_ok(SDL_RenderClear(renderer_.get()), "renderer clear");
    sdl_ok(SDL_RenderCopy(renderer_.get(), texture_.get(), nullptr, nullptr), "frame presentation");
    SDL_RenderPresent(renderer_.get());
}

} // namespace regml

#endif // REGML_WITH_SDL2
