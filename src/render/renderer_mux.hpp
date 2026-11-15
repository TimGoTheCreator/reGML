#pragma once

#include "renderer.hpp"

#include <unordered_map>
#include <variant>
#include <vector>

namespace regml::render {

class RendererMux final : public IRenderer {
  public:
    RendererMux(IRenderer &primary, IRenderer &software) noexcept;

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
    bool present_software_frame(ConstPixelBufferView pixels) override;

    [[nodiscard]] bool software_fallback_active() const noexcept override { return software_mode_; }
    bool force_software_fallback() noexcept override;

  private:
    struct SurfaceRecord {
        SurfaceDesc description{};
        bool primary_available = false;
    };
    struct OwnedTriangles {
        DrawTrianglesCommand state{};
        std::vector<Vertex2D> vertices;
        std::vector<std::uint32_t> indices;
        [[nodiscard]] RenderCommand view() const noexcept;
    };
    using OwnedCommand = std::variant<ClearCommand, OwnedTriangles>;

    static OwnedCommand own(const RenderCommand &command);
    bool activate_software() noexcept;
    bool sync_primary_to_software();
    bool sync_software_to_primary();
    bool sync_surface(IRenderer &source, IRenderer &destination, SurfaceHandle handle, Extent2D size);

    IRenderer &primary_;
    IRenderer &software_;
    FrameInfo frame_{};
    bool frame_active_ = false;
    bool primary_frame_active_ = false;
    bool primary_needs_present_ = false;
    bool software_frame_active_ = false;
    bool software_mode_ = false;
    bool previous_frame_software_ = false;
    std::vector<OwnedCommand> commands_;
    std::unordered_map<std::uint32_t, SurfaceRecord> surfaces_;
    std::unordered_map<std::uint32_t, bool> primary_textures_;
};

} // namespace regml::render
