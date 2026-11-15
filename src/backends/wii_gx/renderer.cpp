#include "renderer.hpp"

#if REGML_WII
#include "gpu.hpp"
#include "lookup.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <variant>

namespace regml::render {
namespace {

bool is_standard_blend(const BlendState &blend) noexcept {
    return blend.enabled && blend.source_rgb == BlendFactor::SourceAlpha &&
           blend.destination_rgb == BlendFactor::OneMinusSourceAlpha &&
           blend.source_alpha == BlendFactor::One &&
           blend.destination_alpha == BlendFactor::OneMinusSourceAlpha &&
           blend.equation_rgb == BlendEquation::Add && blend.equation_alpha == BlendEquation::Add;
}

bool all_channels_writable(const ColorWriteMask &mask) noexcept {
    return mask.red && mask.green && mask.blue && mask.alpha;
}

std::uint8_t multiply_channel(std::uint8_t left, std::uint8_t right) noexcept {
    return static_cast<std::uint8_t>((static_cast<unsigned>(left) * right + 127U) / 255U);
}

ColorRGBA8 modulate(ColorRGBA8 color, ColorRGBA8 tint, float opacity) noexcept {
    color.r = multiply_channel(color.r, tint.r);
    color.g = multiply_channel(color.g, tint.g);
    color.b = multiply_channel(color.b, tint.b);
    color.a = multiply_channel(color.a, tint.a);
    color.a = static_cast<std::uint8_t>(std::clamp(color.a * opacity, 0.0F, 255.0F) + 0.5F);
    return color;
}

bool same_color(ColorRGBA8 left, ColorRGBA8 right) noexcept {
    return left.r == right.r && left.g == right.g && left.b == right.b && left.a == right.a;
}

std::uint32_t pack_color(ColorRGBA8 color) noexcept {
    return (static_cast<std::uint32_t>(color.a) << 24) | (static_cast<std::uint32_t>(color.r) << 16) |
           (static_cast<std::uint32_t>(color.g) << 8) | color.b;
}

struct Triangle {
    Vec2 points[3]{};
    std::uint32_t color = 0;
};

enum class TriangleStatus { valid, unsupported, invalid };

TriangleStatus
get_triangle(const DrawTrianglesCommand &command, std::size_t triangle_index, Triangle &result) noexcept {
    const auto element = triangle_index * 3;
    const auto vertex_count = command.indices.empty() ? command.vertices.size() : command.indices.size();
    if (element + 2 >= vertex_count)
        return TriangleStatus::invalid;

    ColorRGBA8 final_color{};
    for (std::size_t corner = 0; corner < 3; ++corner) {
        const std::size_t index =
            command.indices.empty() ? element + corner : command.indices[element + corner];
        if (index >= command.vertices.size())
            return TriangleStatus::invalid;
        const auto &vertex = command.vertices[index];
        if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y))
            return TriangleStatus::invalid;
        const Vec2 transformed{
            command.transform.m00 * vertex.x + command.transform.m01 * vertex.y + command.transform.tx,
            command.transform.m10 * vertex.x + command.transform.m11 * vertex.y + command.transform.ty};
        if (!std::isfinite(transformed.x) || !std::isfinite(transformed.y))
            return TriangleStatus::invalid;
        result.points[corner] = transformed;

        const auto color = modulate(vertex.color, command.tint, command.opacity);
        if (corner == 0)
            final_color = color;
        else if (!same_color(final_color, color))
            return TriangleStatus::unsupported;
    }
    result.color = pack_color(final_color);
    return TriangleStatus::valid;
}

SubmitResult validate(const DrawTrianglesCommand &command) noexcept {
    if (command.target.surface.valid() || !std::holds_alternative<std::monostate>(command.source) ||
        command.clip.has_value() || command.alpha_test.enabled ||
        !all_channels_writable(command.color_write) || !is_standard_blend(command.blend))
        return SubmitResult::Unsupported;
    if (!std::isfinite(command.opacity) || command.opacity < 0.0F || command.opacity > 1.0F)
        return SubmitResult::Unsupported;

    const auto element_count = command.indices.empty() ? command.vertices.size() : command.indices.size();
    if (element_count % 3 != 0)
        return SubmitResult::Failed;
    for (std::size_t i = 0; i < element_count / 3; ++i) {
        Triangle triangle{};
        const auto status = get_triangle(command, i, triangle);
        if (status == TriangleStatus::invalid)
            return SubmitResult::Failed;
        if (status == TriangleStatus::unsupported)
            return SubmitResult::Unsupported;
    }
    return SubmitResult::Submitted;
}

} // namespace

RendererCapabilities WiiGxRenderer::capabilities() const noexcept {
    return {.hardware_accelerated = true,
            .render_targets = false,
            .texture_sampling = false,
            .readback = false,
            .max_texture_size = 0};
}

bool WiiGxRenderer::begin_frame(const FrameInfo &frame) {
    if (frame_active_ || frame.output_size.width != static_cast<std::uint32_t>(render_width) ||
        frame.output_size.height != static_cast<std::uint32_t>(render_height))
        return false;
    wii_gpu_begin();
    frame_active_ = true;
    frame_ready_ = false;
    return true;
}

SubmitResult WiiGxRenderer::submit(const RenderCommand &command) {
    if (!frame_active_)
        return SubmitResult::Failed;

    if (const auto *clear = std::get_if<ClearCommand>(&command)) {
        if (clear->target.surface.valid() || clear->clip.has_value() || clear->color.a != 255)
            return SubmitResult::Unsupported;
        wii_gpu_box(0, 0, render_width, render_height, pack_color(clear->color));
        return SubmitResult::Submitted;
    }

    const auto &draw = std::get<DrawTrianglesCommand>(command);
    const auto status = validate(draw);
    if (status != SubmitResult::Submitted)
        return status;
    const auto element_count = draw.indices.empty() ? draw.vertices.size() : draw.indices.size();
    for (std::size_t i = 0; i < element_count / 3; ++i) {
        Triangle triangle{};
        (void)get_triangle(draw, i, triangle);
        wii_gpu_triangle(triangle.points[0].x, triangle.points[0].y, triangle.points[1].x,
                         triangle.points[1].y, triangle.points[2].x, triangle.points[2].y, triangle.color);
    }
    return SubmitResult::Submitted;
}

bool WiiGxRenderer::end_frame() {
    if (!frame_active_)
        return false;
    wii_gpu_flush();
    frame_active_ = false;
    frame_ready_ = true;
    return true;
}

void WiiGxRenderer::present() {
    if (!frame_ready_)
        return;
    presenter_.present_gpu();
    frame_ready_ = false;
}

bool WiiGxRenderer::create_texture(TextureHandle, const TextureDesc &, ConstPixelBufferView) {
    return false;
}

bool WiiGxRenderer::update_texture(TextureHandle, RectI, ConstPixelBufferView) {
    return false;
}

void WiiGxRenderer::destroy_texture(TextureHandle) {}

bool WiiGxRenderer::create_surface(SurfaceHandle, const SurfaceDesc &) {
    return false;
}

void WiiGxRenderer::destroy_surface(SurfaceHandle) {}

bool WiiGxRenderer::read_surface(SurfaceHandle, PixelBufferView) {
    return false;
}

} // namespace regml::render
#endif
