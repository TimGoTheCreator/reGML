#pragma once

#include "render/renderer.hpp"

#include <unordered_map>
#include <vector>

namespace regml::render {

// Reference implementation for the renderer command API. It is also the
// command replay target when a hardware backend declines a frame operation.
class SoftwareRenderer final : public IRenderer {
  public:
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
    bool update_surface(SurfaceHandle handle, ConstPixelBufferView pixels) override;

  private:
    struct Image {
        Extent2D size{};
        std::vector<std::uint32_t> pixels;
    };

    Image *target(SurfaceHandle handle) noexcept;
    const Image *target(SurfaceHandle handle) const noexcept;
    bool copy_into(Image &image, RectI destination, ConstPixelBufferView source) noexcept;
    bool copy_out(const Image &image, PixelBufferView destination) const noexcept;
    SubmitResult draw(const DrawTrianglesCommand &command);

    Image output_;
    std::unordered_map<std::uint32_t, Image> textures_;
    std::unordered_map<std::uint32_t, Image> surfaces_;
    bool frame_active_ = false;
};

} // namespace regml::render
