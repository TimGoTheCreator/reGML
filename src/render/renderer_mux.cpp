#include "render/renderer_mux.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace regml::render {

RendererMux::RendererMux(IRenderer &primary, IRenderer &software) noexcept : primary_(primary), software_(software) {}

RendererCapabilities RendererMux::capabilities() const noexcept { return primary_.capabilities(); }

bool RendererMux::begin_frame(const FrameInfo &frame) {
    if (frame_active_)
        return false;
    if (previous_frame_software_ && !sync_software_to_primary())
        return false;
    frame_ = frame;
    commands_.clear();
    software_mode_ = false;
    frame_active_ = true;
    previous_frame_software_ = false;

    primary_frame_active_ = primary_.begin_frame(frame);
    primary_needs_present_ = primary_frame_active_;
    if (primary_frame_active_)
        return true;

    software_mode_ = true;
    software_frame_active_ = software_.begin_frame(frame);
    if (!software_frame_active_) {
        software_mode_ = false;
        frame_active_ = false;
        return false;
    }
    return true;
}

SubmitResult RendererMux::submit(const RenderCommand &command) {
    if (!frame_active_)
        return SubmitResult::Failed;
    if (software_mode_)
        return software_.submit(command);

    const auto result = primary_.submit(command);
    if (result == SubmitResult::Failed)
        return SubmitResult::Failed;
    if (result == SubmitResult::Unsupported)
        return activate_software() ? SubmitResult::Unsupported : SubmitResult::Failed;

    try {
        commands_.push_back(own(command));
    } catch (...) {
        return SubmitResult::Failed;
    }
    return SubmitResult::Submitted;
}

bool RendererMux::end_frame() {
    if (!frame_active_)
        return false;
    frame_active_ = false;
    if (software_mode_) {
        software_frame_active_ = false;
        previous_frame_software_ = true;
        return software_.end_frame();
    }
    if (!primary_frame_active_)
        return false;
    primary_frame_active_ = false;
    if (primary_.end_frame())
        return true;

    frame_active_ = true;
    if (!activate_software()) {
        frame_active_ = false;
        return false;
    }
    frame_active_ = false;
    software_frame_active_ = false;
    previous_frame_software_ = true;
    return software_.end_frame();
}

void RendererMux::present() {
    if (!software_mode_) {
        primary_.present();
        primary_needs_present_ = false;
    } else {
        software_.present();
    }
}

bool RendererMux::create_texture(TextureHandle handle,
                                 const TextureDesc &description,
                                 ConstPixelBufferView initial_pixels) {
    if (!software_.create_texture(handle, description, initial_pixels))
        return false;
    primary_textures_.insert_or_assign(handle.id, primary_.create_texture(handle, description, initial_pixels));
    return true;
}

bool RendererMux::update_texture(TextureHandle handle, RectI destination, ConstPixelBufferView pixels) {
    const bool software_ok = software_.update_texture(handle, destination, pixels);
    const auto primary = primary_textures_.find(handle.id);
    const bool primary_ok = primary == primary_textures_.end() || !primary->second ||
                            primary_.update_texture(handle, destination, pixels);
    return software_ok && primary_ok;
}

void RendererMux::destroy_texture(TextureHandle handle) {
    primary_.destroy_texture(handle);
    software_.destroy_texture(handle);
    primary_textures_.erase(handle.id);
}

bool RendererMux::create_surface(SurfaceHandle handle, const SurfaceDesc &description) {
    if (!handle.valid() || !software_.create_surface(handle, description))
        return false;
    surfaces_.insert_or_assign(
        handle.id, SurfaceRecord{description, primary_.create_surface(handle, description)});
    return true;
}

void RendererMux::destroy_surface(SurfaceHandle handle) {
    if (!handle.valid())
        return;
    primary_.destroy_surface(handle);
    software_.destroy_surface(handle);
    surfaces_.erase(handle.id);
}

bool RendererMux::read_surface(SurfaceHandle handle, PixelBufferView destination) {
    if (software_mode_)
        return software_.read_surface(handle, destination);
    if (handle.valid()) {
        const auto surface = surfaces_.find(handle.id);
        if (surface == surfaces_.end())
            return false;
        if (!surface->second.primary_available)
            return software_.read_surface(handle, destination);
    }
    if (primary_.read_surface(handle, destination))
        return true;
    if (!frame_active_)
        return false;
    if (!force_software_fallback())
        return false;
    return software_.read_surface(handle, destination);
}

bool RendererMux::update_surface(SurfaceHandle handle, ConstPixelBufferView pixels) {
    const bool software_ok = software_.update_surface(handle, pixels);
    if (!handle.valid())
        return software_ok;
    const auto surface = surfaces_.find(handle.id);
    if (surface == surfaces_.end())
        return false;
    if (!surface->second.primary_available)
        return software_ok;
    const bool primary_ok = primary_.update_surface(handle, pixels);
    return software_ok && primary_ok;
}

bool RendererMux::present_software_frame(ConstPixelBufferView pixels) {
    return primary_.present_software_frame(pixels);
}

bool RendererMux::force_software_fallback() noexcept { return activate_software(); }

bool RendererMux::activate_software() noexcept {
    if (software_mode_)
        return true;
    try {
        if (primary_frame_active_) {
            (void)primary_.end_frame();
            primary_frame_active_ = false;
        }
        if (!software_.begin_frame(frame_))
            return false;
        software_frame_active_ = true;
        if (!sync_primary_to_software()) {
            (void)software_.end_frame();
            software_frame_active_ = false;
            return false;
        }
        for (const auto &command : commands_) {
            const auto replay = std::visit(
                [](const auto &owned) -> RenderCommand {
                    using T = std::decay_t<decltype(owned)>;
                    if constexpr (std::is_same_v<T, ClearCommand>)
                        return owned;
                    else
                        return owned.view();
                },
                command);
            if (software_.submit(replay) != SubmitResult::Submitted) {
                (void)software_.end_frame();
                software_frame_active_ = false;
                return false;
            }
        }
        if (primary_needs_present_) {
            primary_.present();
            primary_needs_present_ = false;
        }
        software_mode_ = true;
        previous_frame_software_ = true;
        return true;
    } catch (...) {
        if (software_frame_active_)
            (void)software_.end_frame();
        software_frame_active_ = false;
        return false;
    }
}

bool RendererMux::sync_primary_to_software() {
    if (frame_.output_size.width == 0 || frame_.output_size.height == 0)
        return false;
    for (const auto &[id, surface] : surfaces_) {
        if (!surface.primary_available)
            continue;
        const auto &description = surface.description;
        if (!sync_surface(primary_, software_, SurfaceHandle{id}, description.size))
            return false;
    }
    return sync_surface(primary_, software_, SurfaceHandle{}, frame_.output_size);
}

bool RendererMux::sync_software_to_primary() {
    for (const auto &[id, surface] : surfaces_) {
        if (!surface.primary_available)
            continue;
        const auto &description = surface.description;
        if (!sync_surface(software_, primary_, SurfaceHandle{id}, description.size))
            return false;
    }
    return true;
}

bool RendererMux::sync_surface(IRenderer &source, IRenderer &destination, SurfaceHandle handle, Extent2D size) {
    const auto bytes = static_cast<std::uint64_t>(size.width) * size.height * 4;
    const auto row_pitch = static_cast<std::uint64_t>(size.width) * 4;
    if (size.width == 0 || size.height == 0 || bytes > std::numeric_limits<std::size_t>::max() ||
        row_pitch > std::numeric_limits<std::size_t>::max())
        return false;
    std::vector<std::byte> buffer(static_cast<std::size_t>(bytes));
    PixelBufferView output{buffer, size, static_cast<std::size_t>(row_pitch), PixelFormat::RGBA8};
    if (!source.read_surface(handle, output))
        return false;
    ConstPixelBufferView input{buffer, size, static_cast<std::size_t>(row_pitch), PixelFormat::RGBA8};
    return destination.update_surface(handle, input);
}

RendererMux::OwnedCommand RendererMux::own(const RenderCommand &command) {
    if (const auto *clear = std::get_if<ClearCommand>(&command))
        return *clear;
    const auto &draw = std::get<DrawTrianglesCommand>(command);
    OwnedTriangles owned{};
    owned.state = draw;
    owned.vertices.assign(draw.vertices.begin(), draw.vertices.end());
    owned.indices.assign(draw.indices.begin(), draw.indices.end());
    owned.state.vertices = {};
    owned.state.indices = {};
    return owned;
}

RenderCommand RendererMux::OwnedTriangles::view() const noexcept {
    auto command = state;
    command.vertices = vertices;
    command.indices = indices;
    return command;
}

} // namespace regml::render
