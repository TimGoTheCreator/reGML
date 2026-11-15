#pragma once

#include "render/renderer.hpp"

#if REGML_WII
#include "presenter.hpp"

namespace regml::render {

// Transitional command adapter over the existing GX immediate-mode path.
// Canvas remains the owner of the current optimized texture and software
// drawing paths; commands this first adapter cannot express are reported as
// Unsupported so the caller can route them through its Canvas fallback.
class WiiGxRenderer final : public IRenderer {
  public:
    explicit WiiGxRenderer(WiiRenderer &presenter) noexcept : presenter_(presenter) {}

    [[nodiscard]] RendererCapabilities capabilities() const noexcept override;
    bool begin_frame(const FrameInfo &frame) override;
    SubmitResult submit(const RenderCommand &command) override;
    bool end_frame() override;
    void present() override;

    bool create_texture(TextureHandle handle,
                        const TextureDesc &description,
                        ConstPixelBufferView initial_pixels = {}) override;
    bool update_texture(TextureHandle handle, RectI destination, ConstPixelBufferView pixels) override;
    void destroy_texture(TextureHandle handle) override;
    bool create_surface(SurfaceHandle handle, const SurfaceDesc &description) override;
    void destroy_surface(SurfaceHandle handle) override;
    bool read_surface(SurfaceHandle handle, PixelBufferView destination) override;

  private:
    WiiRenderer &presenter_;
    bool frame_active_ = false;
    bool frame_ready_ = false;
};

} // namespace regml::render
#endif
