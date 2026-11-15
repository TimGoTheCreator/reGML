#pragma once

#include "render/renderer.hpp"

#include <memory>

namespace regml::render {

class WiiUGX2Renderer final : public IRenderer {
  public:
    WiiUGX2Renderer();
    ~WiiUGX2Renderer() override;

    WiiUGX2Renderer(const WiiUGX2Renderer &) = delete;
    WiiUGX2Renderer &operator=(const WiiUGX2Renderer &) = delete;
    WiiUGX2Renderer(WiiUGX2Renderer &&) = delete;
    WiiUGX2Renderer &operator=(WiiUGX2Renderer &&) = delete;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] RendererCapabilities capabilities() const noexcept override;
    [[nodiscard]] Extent2D presentation_size() const noexcept override;

    bool begin_frame(const FrameInfo &frame) override;
    bool resize_frame_output(Extent2D size) override;
    SubmitResult submit(const RenderCommand &command) override;
    bool end_frame() override;
    void present() override;
    bool update_surface(SurfaceHandle handle, ConstPixelBufferView pixels) override;

    bool create_texture(TextureHandle handle,
                        const TextureDesc &description,
                        ConstPixelBufferView initial_pixels = {}) override;
    bool update_texture(TextureHandle handle, RectI destination, ConstPixelBufferView pixels) override;
    void destroy_texture(TextureHandle handle) override;

    bool create_surface(SurfaceHandle handle, const SurfaceDesc &description) override;
    void destroy_surface(SurfaceHandle handle) override;
    bool read_surface(SurfaceHandle handle, PixelBufferView destination) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace regml::render
