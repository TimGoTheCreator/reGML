#pragma once

#include "render_types.hpp"

#include <span>

namespace regml::render {

class IRenderer {
  public:
    virtual ~IRenderer() = default;

    [[nodiscard]] virtual RendererCapabilities capabilities() const noexcept = 0;

    // Physical presentation dimensions, when the backend owns a fixed-size
    // display. A zero extent means that the host window supplies its own size.
    [[nodiscard]] virtual Extent2D presentation_size() const noexcept { return {}; }

    virtual bool begin_frame(const FrameInfo &frame) = 0;
    virtual bool resize_frame_output(Extent2D size) {
        (void)size;
        return false;
    }
    virtual SubmitResult submit(const RenderCommand &command) = 0;
    virtual bool end_frame() = 0;
    virtual void present() = 0;

    [[nodiscard]] virtual bool software_fallback_active() const noexcept { return false; }
    virtual bool force_software_fallback() noexcept { return false; }

    virtual bool update_surface(SurfaceHandle handle, ConstPixelBufferView pixels) {
        (void)handle;
        (void)pixels;
        return false;
    }
    virtual bool present_software_frame(ConstPixelBufferView pixels) {
        (void)pixels;
        return false;
    }

    virtual bool create_texture(TextureHandle handle,
                                const TextureDesc &description,
                                ConstPixelBufferView initial_pixels = {}) = 0;
    virtual bool update_texture(TextureHandle handle, RectI destination, ConstPixelBufferView pixels) = 0;
    virtual void destroy_texture(TextureHandle handle) = 0;

    virtual bool create_surface(SurfaceHandle handle, const SurfaceDesc &description) = 0;
    virtual void destroy_surface(SurfaceHandle handle) = 0;
    virtual bool read_surface(SurfaceHandle handle, PixelBufferView destination) = 0;
};

struct BatchSubmitResult {
    SubmitResult result = SubmitResult::Submitted;
    std::size_t command_index = 0;
};

inline BatchSubmitResult submit(IRenderer &renderer, std::span<const RenderCommand> commands) {
    for (std::size_t i = 0; i < commands.size(); ++i) {
        const auto result = renderer.submit(commands[i]);
        if (result != SubmitResult::Submitted)
            return {result, i};
    }
    return {SubmitResult::Submitted, commands.size()};
}

} // namespace regml::render
