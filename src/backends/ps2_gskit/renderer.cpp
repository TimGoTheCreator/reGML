#include "renderer.hpp"

#if REGML_PS2
#include "gpu.hpp"

#include "error.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <malloc.h>
#include <variant>

namespace regml::render {
namespace {
#if REGML_240P
constexpr int kTextureWidth = 512;
constexpr int kTextureHeight = 256;
#else
constexpr int kTextureWidth = 1024;
constexpr int kTextureHeight = 512;
#endif
constexpr int kDisplayWidth = 640;
constexpr int kDisplayHeight = 448;
constexpr std::size_t kTexturePixels = static_cast<std::size_t>(kTextureWidth) * kTextureHeight;
constexpr std::size_t kTextureBytes = kTexturePixels * sizeof(std::uint16_t);

std::uint16_t rgba5551(std::uint32_t argb) noexcept {
    const auto red = (argb >> 19) & 0x1f;
    const auto green = (argb >> 11) & 0x1f;
    const auto blue = (argb >> 3) & 0x1f;
    return static_cast<std::uint16_t>(0x8000u | red | (green << 5) | (blue << 10));
}

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

Ps2GsKitRenderer::Ps2GsKitRenderer(GSGLOBAL *gs, bool uncapped) noexcept : gs_(gs), uncapped_(uncapped) {}

Ps2GsKitRenderer::~Ps2GsKitRenderer() {
    ps2_gpu_shutdown();
    std::free(software_texture_memory_);
}

RendererCapabilities Ps2GsKitRenderer::capabilities() const noexcept {
    return {.hardware_accelerated = true,
            .render_targets = false,
            .texture_sampling = false,
            .readback = false,
            .max_texture_size = 0};
}

bool Ps2GsKitRenderer::begin_frame(const FrameInfo &frame) {
    if (frame_active_ || !gs_ || frame.output_size.width == 0 || frame.output_size.height == 0 ||
        frame.output_size.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        frame.output_size.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
        return false;

    if (!gpu_initialized_ || frame_width_ != frame.output_size.width ||
        frame_height_ != frame.output_size.height) {
        ps2_gpu_initialize(gs_, static_cast<int>(frame.output_size.width),
                           static_cast<int>(frame.output_size.height));
        gpu_initialized_ = true;
        frame_width_ = frame.output_size.width;
        frame_height_ = frame.output_size.height;
    }
    ps2_gpu_begin();
    frame_active_ = true;
    frame_ready_ = false;
    return true;
}

SubmitResult Ps2GsKitRenderer::submit(const RenderCommand &command) {
    if (!frame_active_)
        return SubmitResult::Failed;

    if (const auto *clear = std::get_if<ClearCommand>(&command)) {
        if (clear->target.surface.valid() || clear->clip.has_value() || clear->color.a != 255)
            return SubmitResult::Unsupported;
        ps2_gpu_box(0, 0, frame_width_, frame_height_, pack_color(clear->color));
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
        ps2_gpu_triangle(triangle.points[0].x, triangle.points[0].y, triangle.points[1].x,
                         triangle.points[1].y, triangle.points[2].x, triangle.points[2].y, triangle.color);
    }
    return SubmitResult::Submitted;
}

bool Ps2GsKitRenderer::end_frame() {
    if (!frame_active_)
        return false;
    ps2_gpu_flush();
    frame_active_ = false;
    frame_ready_ = true;
    return true;
}

void Ps2GsKitRenderer::present() {
    if (!frame_ready_ || !gpu_initialized_)
        return;
    if (uncapped_) {
        if (!gs_->FirstFrame && gs_->DoubleBuffering == GS_SETTING_ON) {
            GS_SET_DISPFB2(gs_->ScreenBuffer[gs_->ActiveBuffer & 1] / 8192, gs_->Width / 64, gs_->PSM, 0, 0);
            gs_->ActiveBuffer ^= 1;
        }
        gsKit_setactive(gs_);
    } else {
        gsKit_sync_flip(gs_);
    }
    ps2_gpu_presented();
    frame_ready_ = false;
}

bool Ps2GsKitRenderer::create_texture(TextureHandle, const TextureDesc &, ConstPixelBufferView) {
    return false;
}

bool Ps2GsKitRenderer::update_texture(TextureHandle, RectI, ConstPixelBufferView) {
    return false;
}

void Ps2GsKitRenderer::destroy_texture(TextureHandle) {}

bool Ps2GsKitRenderer::create_surface(SurfaceHandle, const SurfaceDesc &) {
    return false;
}

void Ps2GsKitRenderer::destroy_surface(SurfaceHandle) {}

bool Ps2GsKitRenderer::read_surface(SurfaceHandle, PixelBufferView) {
    return false;
}

void Ps2GsKitRenderer::present_canvas(const Canvas &canvas) {
    if (!gs_ || canvas.width() <= 0 || canvas.height() <= 0)
        return;
    if (!software_texture_memory_) {
        auto *pixels = static_cast<std::uint16_t *>(memalign(64, kTextureBytes));
        if (!pixels)
            throw Error(ErrorKind::io, "PS2 software framebuffer allocation failed");
        software_texture_memory_ = pixels;
        std::memset(pixels, 0, kTextureBytes);
        software_texture_.Width = kTextureWidth;
        software_texture_.Height = kTextureHeight;
        software_texture_.PSM = GS_PSM_CT16;
        software_texture_.TBW = kTextureWidth / 64;
        software_texture_.Mem = static_cast<u32 *>(software_texture_memory_);
        software_texture_.Filter = GS_FILTER_NEAREST;
        software_texture_.Vram = gsKit_vram_alloc(
            gs_, gsKit_texture_size(kTextureWidth, kTextureHeight, GS_PSM_CT16), GSKIT_ALLOC_USERBUFFER);
        if (software_texture_.Vram == GSKIT_ALLOC_ERROR)
            throw Error(ErrorKind::io, "PS2 software framebuffer VRAM allocation failed");
    }
    auto *pixels = static_cast<std::uint16_t *>(software_texture_memory_);
    const auto source = canvas.pixels();
    const auto width = std::min(canvas.width(), kTextureWidth);
    const auto height = std::min(canvas.height(), kTextureHeight);
    for (int y = 0; y < height; ++y) {
        auto *row = pixels + static_cast<std::size_t>(y) * kTextureWidth;
        const auto *src = source.data() + static_cast<std::size_t>(y) * canvas.width();
        for (int x = 0; x < width; ++x)
            row[x] = rgba5551(src[x]);
        std::fill(row + width, row + kTextureWidth, 0);
    }
    for (int y = height; y < kTextureHeight; ++y) {
        auto *row = pixels + static_cast<std::size_t>(y) * kTextureWidth;
        std::fill(row, row + kTextureWidth, 0);
    }

    gsKit_texture_upload(gs_, &software_texture_);

    const float scale = std::min(static_cast<float>(kDisplayWidth) / canvas.width(),
                                 static_cast<float>(kDisplayHeight) / canvas.height());
    const float draw_width = canvas.width() * scale;
    const float draw_height = canvas.height() * scale;
    const float left = (kDisplayWidth - draw_width) * 0.5f;
    const float top = (kDisplayHeight - draw_height) * 0.5f;
    gsKit_clear(gs_, GS_SETREG_RGBAQ(0, 0, 0, 0x80, 0));
    gsKit_prim_sprite_texture(gs_, &software_texture_, left, top, 0.0f, 0.0f, left + draw_width,
                              top + draw_height, static_cast<float>(canvas.width()),
                              static_cast<float>(canvas.height()), 1,
                              GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0));
    gsKit_queue_exec(gs_);
    gsKit_sync_flip(gs_);
}

} // namespace regml::render
#endif
