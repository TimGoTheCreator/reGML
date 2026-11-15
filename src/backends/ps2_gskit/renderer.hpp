#pragma once

#include "render/renderer.hpp"

#if REGML_PS2
#include "backends/software/canvas.hpp"
#include <gsKit.h>

namespace regml::render {

// GSKit adapter for the common command API. The legacy Canvas presenter is
// retained as a staging path while existing Canvas fast paths are migrated.
class Ps2GsKitRenderer final : public IRenderer {
  public:
    explicit Ps2GsKitRenderer(GSGLOBAL *gs, bool uncapped = false) noexcept;
    ~Ps2GsKitRenderer() override;
    Ps2GsKitRenderer(const Ps2GsKitRenderer &) = delete;
    Ps2GsKitRenderer &operator=(const Ps2GsKitRenderer &) = delete;

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

    // Compatibility presentation for the current Canvas software fallback.
    void present_canvas(const Canvas &canvas);

  private:
    GSGLOBAL *gs_ = nullptr;
    GSTEXTURE software_texture_{};
    void *software_texture_memory_ = nullptr;
    std::uint32_t frame_width_ = 0;
    std::uint32_t frame_height_ = 0;
    bool gpu_initialized_ = false;
    bool frame_active_ = false;
    bool frame_ready_ = false;
    bool uncapped_ = false;
};

} // namespace regml::render
#endif
