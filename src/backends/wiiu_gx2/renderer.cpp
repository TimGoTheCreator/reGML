#include "renderer.hpp"

#if defined(REGML_WIIU) && REGML_WIIU

#include "renderer_gfd.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gx2/clear.h>
#include <gx2/draw.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/shaders.h>
#include <gx2/state.h>
#include <gx2/surface.h>
#include <gx2/swap.h>
#include <gx2/texture.h>
#include <gx2/utils.h>
#include <gx2r/buffer.h>
#include <gx2r/resource.h>
#include <gx2r/surface.h>
#include <whb/gfx.h>

namespace regml::render {
namespace {

constexpr std::uint32_t kMaxTextureDimension = 4096;
constexpr GX2RResourceFlags kNoResourceFlags = static_cast<GX2RResourceFlags>(0);

struct GpuVertex {
    float x;
    float y;
    float u;
    float v;
    std::uint8_t color[4];
};

struct VertexBufferOwner {
    GX2RBuffer buffer{};
    bool created = false;

    VertexBufferOwner() = default;
    VertexBufferOwner(const VertexBufferOwner &) = delete;
    VertexBufferOwner &operator=(const VertexBufferOwner &) = delete;
    VertexBufferOwner(VertexBufferOwner &&other) noexcept : buffer(other.buffer), created(other.created) {
        other.buffer = {};
        other.created = false;
    }
    VertexBufferOwner &operator=(VertexBufferOwner &&other) noexcept {
        if (this != &other) {
            reset();
            buffer = other.buffer;
            created = other.created;
            other.buffer = {};
            other.created = false;
        }
        return *this;
    }
    ~VertexBufferOwner() { reset(); }

    void reset() noexcept {
        if (created) {
            GX2RDestroyBufferEx(&buffer, kNoResourceFlags);
            buffer = {};
            created = false;
        }
    }
};

struct SurfaceOwner {
    GX2Surface surface{};
    bool created = false;

    SurfaceOwner() = default;
    SurfaceOwner(const SurfaceOwner &) = delete;
    SurfaceOwner &operator=(const SurfaceOwner &) = delete;
    SurfaceOwner(SurfaceOwner &&other) noexcept : surface(other.surface), created(other.created) {
        other.surface = {};
        other.created = false;
    }
    SurfaceOwner &operator=(SurfaceOwner &&other) noexcept {
        if (this != &other) {
            reset();
            surface = other.surface;
            created = other.created;
            other.surface = {};
            other.created = false;
        }
        return *this;
    }
    ~SurfaceOwner() { reset(); }

    void reset() noexcept {
        if (created) {
            GX2RDestroySurfaceEx(&surface, kNoResourceFlags);
            surface = {};
            created = false;
        }
    }
};

std::size_t pixel_size(PixelFormat format) noexcept {
    switch (format) {
    case PixelFormat::RGBA8:
    case PixelFormat::BGRA8:
        return 4;
    case PixelFormat::R8:
    case PixelFormat::A8:
        return 1;
    }
    return 0;
}

bool valid_size(Extent2D size) noexcept {
    return size.width > 0 && size.height > 0 && size.width <= kMaxTextureDimension &&
           size.height <= kMaxTextureDimension;
}

std::uint8_t multiply_channel(std::uint8_t source, std::uint8_t tint) noexcept {
    return static_cast<std::uint8_t>((static_cast<unsigned>(source) * tint + 127u) / 255u);
}

std::uint8_t multiply_opacity(std::uint8_t value, float opacity) noexcept {
    if (!std::isfinite(opacity))
        return 0;
    const float scaled = static_cast<float>(value) * std::clamp(opacity, 0.0F, 1.0F);
    return static_cast<std::uint8_t>(std::clamp(std::lround(scaled), 0L, 255L));
}

GX2BlendMode gx2_blend_factor(BlendFactor factor) noexcept {
    switch (factor) {
    case BlendFactor::Zero:
        return GX2_BLEND_MODE_ZERO;
    case BlendFactor::One:
        return GX2_BLEND_MODE_ONE;
    case BlendFactor::SourceColor:
        return GX2_BLEND_MODE_SRC_COLOR;
    case BlendFactor::OneMinusSourceColor:
        return GX2_BLEND_MODE_INV_SRC_COLOR;
    case BlendFactor::DestinationColor:
        return GX2_BLEND_MODE_DST_COLOR;
    case BlendFactor::OneMinusDestinationColor:
        return GX2_BLEND_MODE_INV_DST_COLOR;
    case BlendFactor::SourceAlpha:
        return GX2_BLEND_MODE_SRC_ALPHA;
    case BlendFactor::OneMinusSourceAlpha:
        return GX2_BLEND_MODE_INV_SRC_ALPHA;
    case BlendFactor::DestinationAlpha:
        return GX2_BLEND_MODE_DST_ALPHA;
    case BlendFactor::OneMinusDestinationAlpha:
        return GX2_BLEND_MODE_INV_DST_ALPHA;
    case BlendFactor::ConstantColor:
        return GX2_BLEND_MODE_BLEND_FACTOR;
    case BlendFactor::OneMinusConstantColor:
        return GX2_BLEND_MODE_INV_BLEND_FACTOR;
    case BlendFactor::ConstantAlpha:
        return GX2_BLEND_MODE_CONSTANT_ALPHA;
    case BlendFactor::OneMinusConstantAlpha:
        return GX2_BLEND_MODE_INV_CONSTANT_ALPHA;
    case BlendFactor::SourceAlphaSaturate:
        return GX2_BLEND_MODE_SRC_ALPHA_SAT;
    }
    return GX2_BLEND_MODE_ONE;
}

GX2BlendCombineMode gx2_blend_equation(BlendEquation equation) noexcept {
    switch (equation) {
    case BlendEquation::Add:
        return GX2_BLEND_COMBINE_MODE_ADD;
    case BlendEquation::Subtract:
        return GX2_BLEND_COMBINE_MODE_SUB;
    case BlendEquation::ReverseSubtract:
        return GX2_BLEND_COMBINE_MODE_REV_SUB;
    case BlendEquation::Minimum:
        return GX2_BLEND_COMBINE_MODE_MIN;
    case BlendEquation::Maximum:
        return GX2_BLEND_COMBINE_MODE_MAX;
    }
    return GX2_BLEND_COMBINE_MODE_ADD;
}

GX2CompareFunction gx2_compare(AlphaCompare compare) noexcept {
    switch (compare) {
    case AlphaCompare::Never:
        return GX2_COMPARE_FUNC_NEVER;
    case AlphaCompare::Less:
        return GX2_COMPARE_FUNC_LESS;
    case AlphaCompare::LessEqual:
        return GX2_COMPARE_FUNC_LEQUAL;
    case AlphaCompare::Equal:
        return GX2_COMPARE_FUNC_EQUAL;
    case AlphaCompare::GreaterEqual:
        return GX2_COMPARE_FUNC_GEQUAL;
    case AlphaCompare::Greater:
        return GX2_COMPARE_FUNC_GREATER;
    case AlphaCompare::NotEqual:
        return GX2_COMPARE_FUNC_NOT_EQUAL;
    case AlphaCompare::Always:
        return GX2_COMPARE_FUNC_ALWAYS;
    }
    return GX2_COMPARE_FUNC_ALWAYS;
}

GX2TexClampMode gx2_address_mode(AddressMode mode) noexcept {
    switch (mode) {
    case AddressMode::Clamp:
        return GX2_TEX_CLAMP_MODE_CLAMP;
    case AddressMode::Repeat:
        return GX2_TEX_CLAMP_MODE_WRAP;
    case AddressMode::Mirror:
        return GX2_TEX_CLAMP_MODE_MIRROR;
    }
    return GX2_TEX_CLAMP_MODE_CLAMP;
}

GX2ChannelMask gx2_channel_mask(ColorWriteMask mask) noexcept {
    unsigned bits = 0;
    if (mask.red)
        bits |= GX2_CHANNEL_MASK_R;
    if (mask.green)
        bits |= GX2_CHANNEL_MASK_G;
    if (mask.blue)
        bits |= GX2_CHANNEL_MASK_B;
    if (mask.alpha)
        bits |= GX2_CHANNEL_MASK_A;
    return static_cast<GX2ChannelMask>(bits);
}

void setup_texture_regs(GX2Texture &texture, GX2Surface &surface) noexcept {
    texture = {};
    texture.surface = surface;
    texture.viewFirstMip = 0;
    texture.viewNumMips = 1;
    texture.viewFirstSlice = 0;
    texture.viewNumSlices = 1;
    texture.compMap = GX2_COMP_MAP(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_Z, GX2_SQ_SEL_W);
    GX2InitTextureRegs(&texture);
}

bool init_surface(GX2Surface &surface, Extent2D size, GX2TileMode tile_mode, GX2SurfaceUse use) noexcept {
    if (!valid_size(size))
        return false;
    surface = {};
    surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    surface.width = size.width;
    surface.height = size.height;
    surface.depth = 1;
    surface.mipLevels = 1;
    surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    surface.aa = GX2_AA_MODE1X;
    surface.use = use;
    surface.tileMode = tile_mode;
    GX2CalcSurfaceSizeAndAlignment(&surface);
    return surface.imageSize != 0 && surface.alignment != 0;
}

RectI intersect_rect(RectI rect, Extent2D extent) noexcept {
    const auto left = std::max<std::int64_t>(0, rect.x);
    const auto top = std::max<std::int64_t>(0, rect.y);
    const auto right = std::min<std::int64_t>(extent.width,
                                              static_cast<std::int64_t>(rect.x) + rect.width);
    const auto bottom = std::min<std::int64_t>(extent.height,
                                               static_cast<std::int64_t>(rect.y) + rect.height);
    if (rect.width <= 0 || rect.height <= 0 || right <= left || bottom <= top)
        return {};
    return {static_cast<std::int32_t>(left), static_cast<std::int32_t>(top),
            static_cast<std::int32_t>(right - left), static_cast<std::int32_t>(bottom - top)};
}

} // namespace

struct WiiUGX2Renderer::Impl {
    struct Texture {
        GX2Texture texture{};
        GX2RResourceFlags flags{};
        Extent2D size{};
        bool valid = false;
    };

    struct Surface {
        GX2ColorBuffer color{};
        GX2Texture texture{};
        GX2RResourceFlags flags{};
        Extent2D size{};
        bool sampleable = true;
        bool texture_cache_dirty = false;
        bool valid = false;
    };

    struct Viewport {
        float x = 0;
        float y = 0;
        float width = 0;
        float height = 0;
        float scale_x = 0;
        float scale_y = 0;
    };

    bool initialized = false;
    bool frame_active = false;
    bool frame_ended = false;
    FrameInfo frame{};
    WHBGfxShaderGroup shader_group{};
    GX2Sampler sampler{};
    GX2Texture white_texture{};
    bool white_texture_created = false;
    std::uint32_t sampler_location = 0;
    std::unordered_map<std::uint32_t, Texture> textures;
    std::unordered_map<std::uint32_t, Surface> surfaces;
    std::vector<VertexBufferOwner> frame_vertex_buffers;
    std::vector<SurfaceOwner> deferred_surfaces;
    std::vector<SurfaceOwner> frame_staging_surfaces;

    ~Impl() { shutdown(); }

    bool initialize() noexcept {
        if (!WHBGfxInit())
            return false;

        if (!WHBGfxLoadGFDShaderGroup(
                &shader_group, 0, wiiu_gx2_detail::renderer_gfd.data())) {
            WHBGfxShutdown();
            shader_group = {};
            return false;
        }

        if (!shader_group.vertexShader || !shader_group.pixelShader ||
            !WHBGfxInitShaderAttribute(&shader_group, "aPosition", 0, offsetof(GpuVertex, x),
                                       GX2_ATTRIB_FORMAT_FLOAT_32_32) ||
            !WHBGfxInitShaderAttribute(&shader_group, "aTexCoord", 0, offsetof(GpuVertex, u),
                                       GX2_ATTRIB_FORMAT_FLOAT_32_32) ||
            !WHBGfxInitShaderAttribute(&shader_group, "aColor", 0, offsetof(GpuVertex, color),
                                       GX2_ATTRIB_FORMAT_UNORM_8_8_8_8) ||
            shader_group.pixelShader->samplerVarCount == 0 || !WHBGfxInitFetchShader(&shader_group)) {
            WHBGfxFreeShaderGroup(&shader_group);
            WHBGfxShutdown();
            shader_group = {};
            return false;
        }

        sampler_location = shader_group.pixelShader->samplerVars[0].location;
        GX2InitSampler(&sampler, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_POINT);
        GX2InitSamplerClamping(&sampler, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_CLAMP_MODE_CLAMP,
                               GX2_TEX_CLAMP_MODE_CLAMP);
        GX2InitSamplerZMFilter(&sampler, GX2_TEX_Z_FILTER_MODE_NONE, GX2_TEX_MIP_FILTER_MODE_NONE);

        constexpr GX2RResourceFlags white_flags =
            GX2R_RESOURCE_BIND_TEXTURE | GX2R_RESOURCE_USAGE_CPU_WRITE | GX2R_RESOURCE_USAGE_GPU_READ;
        GX2Surface white_surface{};
        if (!init_surface(white_surface, {1, 1}, GX2_TILE_MODE_DEFAULT, GX2_SURFACE_USE_TEXTURE) ||
            !GX2RCreateSurface(&white_surface, white_flags)) {
            WHBGfxFreeShaderGroup(&shader_group);
            WHBGfxShutdown();
            shader_group = {};
            return false;
        }
        white_texture_created = true;
        setup_texture_regs(white_texture, white_surface);
        const std::array<std::uint8_t, 4> white{255, 255, 255, 255};
        if (!upload_pixels(white_texture.surface, {0, 0, 1, 1},
                           {std::as_bytes(std::span(white)), {1, 1}, 4, PixelFormat::RGBA8})) {
            GX2RDestroySurfaceEx(&white_texture.surface, kNoResourceFlags);
            white_texture_created = false;
            WHBGfxFreeShaderGroup(&shader_group);
            WHBGfxShutdown();
            shader_group = {};
            return false;
        }

        initialized = true;
        return true;
    }

    void shutdown() noexcept {
        if (!initialized && !white_texture_created && !shader_group.vertexShader &&
            !shader_group.pixelShader)
            return;
        if (initialized || frame_active || !frame_vertex_buffers.empty() || !deferred_surfaces.empty() ||
            !frame_staging_surfaces.empty()) {
            GX2DrawDone();
            frame_vertex_buffers.clear();
            deferred_surfaces.clear();
            frame_staging_surfaces.clear();
        }
        for (auto &[id, texture] : textures) {
            (void)id;
            if (texture.valid)
                GX2RDestroySurfaceEx(&texture.texture.surface, kNoResourceFlags);
        }
        textures.clear();
        for (auto &[id, surface] : surfaces) {
            (void)id;
            if (surface.valid)
                GX2RDestroySurfaceEx(&surface.color.surface, kNoResourceFlags);
        }
        surfaces.clear();
        if (white_texture_created) {
            GX2RDestroySurfaceEx(&white_texture.surface, kNoResourceFlags);
            white_texture_created = false;
        }
        if (shader_group.vertexShader || shader_group.pixelShader)
            WHBGfxFreeShaderGroup(&shader_group);
        shader_group = {};
        if (initialized || frame_active)
            WHBGfxShutdown();
        initialized = false;
        frame_active = false;
        frame_ended = false;
    }

    static bool create_texture_resource(Texture &out, const TextureDesc &description) noexcept {
        if (!valid_size(description.size) || description.mipmapped)
            return false;
        constexpr GX2RResourceFlags flags =
            GX2R_RESOURCE_BIND_TEXTURE | GX2R_RESOURCE_USAGE_CPU_WRITE | GX2R_RESOURCE_USAGE_GPU_READ;
        GX2Surface surface{};
        if (!init_surface(surface, description.size, GX2_TILE_MODE_DEFAULT, GX2_SURFACE_USE_TEXTURE) ||
            !GX2RCreateSurface(&surface, flags))
            return false;
        setup_texture_regs(out.texture, surface);
        out.flags = flags;
        out.size = description.size;
        out.valid = true;
        return true;
    }

    static bool create_surface_resource(Surface &out, const SurfaceDesc &description) noexcept {
        if (!valid_size(description.size) || description.format != PixelFormat::RGBA8)
            return false;
        constexpr GX2RResourceFlags flags = GX2R_RESOURCE_BIND_COLOR_BUFFER | GX2R_RESOURCE_BIND_TEXTURE |
                                             GX2R_RESOURCE_USAGE_GPU_READ | GX2R_RESOURCE_USAGE_GPU_WRITE;
        auto &color = out.color;
        color = {};
        if (!init_surface(color.surface, description.size, GX2_TILE_MODE_DEFAULT,
                          static_cast<GX2SurfaceUse>(GX2_SURFACE_USE_TEXTURE | GX2_SURFACE_USE_COLOR_BUFFER)) ||
            !GX2RCreateSurface(&color.surface, flags))
            return false;
        color.viewNumSlices = 1;
        GX2InitColorBufferRegs(&color);
        setup_texture_regs(out.texture, color.surface);
        out.flags = flags;
        out.size = description.size;
        out.sampleable = description.sampleable;
        out.valid = true;
        return true;
    }

    bool upload_pixels(GX2Surface &destination, RectI rectangle, ConstPixelBufferView pixels) {
        const Extent2D region{static_cast<std::uint32_t>(rectangle.width),
                              static_cast<std::uint32_t>(rectangle.height)};
        const auto input_pixel_size = pixel_size(pixels.format);
        const auto input_row_bytes = static_cast<std::size_t>(region.width) * input_pixel_size;
        if (!valid_size(region) || input_pixel_size == 0 || pixels.size.width < region.width ||
            pixels.size.height < region.height || pixels.row_pitch < input_row_bytes ||
            pixels.row_pitch > std::numeric_limits<std::size_t>::max() / region.height ||
            pixels.bytes.size() < pixels.row_pitch * region.height || rectangle.x < 0 || rectangle.y < 0 ||
            static_cast<std::uint64_t>(rectangle.x) + region.width > destination.width ||
            static_cast<std::uint64_t>(rectangle.y) + region.height > destination.height)
            return false;

        GX2Surface staging{};
        if (!init_surface(staging, region, GX2_TILE_MODE_LINEAR_ALIGNED, GX2_SURFACE_USE_TEXTURE))
            return false;
        constexpr GX2RResourceFlags staging_flags =
            GX2R_RESOURCE_BIND_TEXTURE | GX2R_RESOURCE_USAGE_CPU_WRITE | GX2R_RESOURCE_USAGE_GPU_READ;
        if (!GX2RCreateSurface(&staging, staging_flags))
            return false;
        SurfaceOwner staging_owner;
        staging_owner.surface = staging;
        staging_owner.created = true;

        auto *output = static_cast<std::uint8_t *>(GX2RLockSurfaceEx(
            &staging_owner.surface, 0, GX2R_RESOURCE_USAGE_CPU_WRITE));
        if (!output)
            return false;
        const auto *input = reinterpret_cast<const std::uint8_t *>(pixels.bytes.data());
        auto *rgba = output;
        const std::size_t output_pitch = static_cast<std::size_t>(staging_owner.surface.pitch) * 4;
        for (std::uint32_t y = 0; y < region.height; ++y) {
            const auto *src = input + static_cast<std::size_t>(y) * pixels.row_pitch;
            auto *dst = rgba + static_cast<std::size_t>(y) * output_pitch;
            for (std::uint32_t x = 0; x < region.width; ++x) {
                switch (pixels.format) {
                case PixelFormat::RGBA8:
                    std::memcpy(dst + x * 4, src + x * 4, 4);
                    break;
                case PixelFormat::BGRA8:
                    dst[x * 4 + 0] = src[x * 4 + 2];
                    dst[x * 4 + 1] = src[x * 4 + 1];
                    dst[x * 4 + 2] = src[x * 4 + 0];
                    dst[x * 4 + 3] = src[x * 4 + 3];
                    break;
                case PixelFormat::R8:
                    dst[x * 4 + 0] = src[x];
                    dst[x * 4 + 1] = 0;
                    dst[x * 4 + 2] = 0;
                    dst[x * 4 + 3] = 255;
                    break;
                case PixelFormat::A8:
                    dst[x * 4 + 0] = 255;
                    dst[x * 4 + 1] = 255;
                    dst[x * 4 + 2] = 255;
                    dst[x * 4 + 3] = src[x];
                    break;
                }
            }
        }
        GX2RUnlockSurfaceEx(&staging_owner.surface, 0, kNoResourceFlags);

        GX2Rect source_rect{0, 0, static_cast<std::int32_t>(region.width),
                            static_cast<std::int32_t>(region.height)};
        GX2Point destination_point{rectangle.x, rectangle.y};
        GX2CopySurfaceEx(&staging_owner.surface, 0, 0, &destination, 0, 0, 1, &source_rect,
                         &destination_point);
        GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, destination.image, destination.imageSize);
        frame_staging_surfaces.emplace_back(std::move(staging_owner));
        return true;
    }

    bool read_pixels(const GX2Surface &source, PixelBufferView destination) {
        const Extent2D size{source.width, source.height};
        const auto output_pixel_size = pixel_size(destination.format);
        const auto output_row_bytes = static_cast<std::size_t>(size.width) * output_pixel_size;
        if (!valid_size(size) || destination.size.width != size.width || destination.size.height != size.height ||
            output_pixel_size == 0 || destination.row_pitch < output_row_bytes ||
            destination.row_pitch > std::numeric_limits<std::size_t>::max() / size.height ||
            destination.bytes.size() < destination.row_pitch * size.height)
            return false;

        GX2DrawDone();
        GX2Surface staging{};
        if (!init_surface(staging, size, GX2_TILE_MODE_LINEAR_ALIGNED, GX2_SURFACE_USE_TEXTURE))
            return false;
        constexpr GX2RResourceFlags flags =
            GX2R_RESOURCE_BIND_TEXTURE | GX2R_RESOURCE_USAGE_CPU_READ | GX2R_RESOURCE_USAGE_GPU_WRITE;
        if (!GX2RCreateSurface(&staging, flags))
            return false;
        SurfaceOwner owner;
        owner.surface = staging;
        owner.created = true;
        GX2CopySurface(&source, 0, 0, &owner.surface, 0, 0);
        GX2DrawDone();
        auto *input = static_cast<const std::uint8_t *>(GX2RLockSurfaceEx(
            &owner.surface, 0, GX2R_RESOURCE_USAGE_CPU_READ));
        if (!input)
            return false;
        const auto input_pitch = static_cast<std::size_t>(owner.surface.pitch) * 4;
        auto *output = reinterpret_cast<std::uint8_t *>(destination.bytes.data());
        for (std::uint32_t y = 0; y < size.height; ++y) {
            const auto *src = input + static_cast<std::size_t>(y) * input_pitch;
            auto *dst = output + static_cast<std::size_t>(y) * destination.row_pitch;
            for (std::uint32_t x = 0; x < size.width; ++x) {
                switch (destination.format) {
                case PixelFormat::RGBA8:
                    std::memcpy(dst + x * 4, src + x * 4, 4);
                    break;
                case PixelFormat::BGRA8:
                    dst[x * 4 + 0] = src[x * 4 + 2];
                    dst[x * 4 + 1] = src[x * 4 + 1];
                    dst[x * 4 + 2] = src[x * 4 + 0];
                    dst[x * 4 + 3] = src[x * 4 + 3];
                    break;
                case PixelFormat::R8:
                    dst[x] = src[x * 4 + 0];
                    break;
                case PixelFormat::A8:
                    dst[x] = src[x * 4 + 3];
                    break;
                }
            }
        }
        GX2RUnlockSurfaceEx(&owner.surface, 0, kNoResourceFlags);
        return true;
    }

    bool build_vertex_buffer(std::span<const Vertex2D> source,
                             std::span<const std::uint32_t> indices,
                             Affine2D transform,
                             ColorRGBA8 tint,
                             float opacity,
                             Extent2D logical_size,
                             std::vector<GpuVertex> &expanded) {
        if (logical_size.width == 0 || logical_size.height == 0)
            return false;
        const std::size_t count = indices.empty() ? source.size() : indices.size();
        if (count == 0 || count % 3 != 0 || count > std::numeric_limits<std::uint32_t>::max())
            return false;
        expanded.clear();
        expanded.reserve(count);
        const float inverse_width = 1.0F / static_cast<float>(logical_size.width);
        const float inverse_height = 1.0F / static_cast<float>(logical_size.height);
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint32_t index = indices.empty() ? static_cast<std::uint32_t>(i) : indices[i];
            if (index >= source.size())
                return false;
            const auto &vertex = source[index];
            const float x = transform.m00 * vertex.x + transform.m01 * vertex.y + transform.tx;
            const float y = transform.m10 * vertex.x + transform.m11 * vertex.y + transform.ty;
            GpuVertex out{};
            out.x = x * inverse_width * 2.0F - 1.0F;
            out.y = 1.0F - y * inverse_height * 2.0F;
            out.u = vertex.u;
            out.v = vertex.v;
            out.color[0] = multiply_channel(vertex.color.r, tint.r);
            out.color[1] = multiply_channel(vertex.color.g, tint.g);
            out.color[2] = multiply_channel(vertex.color.b, tint.b);
            out.color[3] = multiply_opacity(multiply_channel(vertex.color.a, tint.a), opacity);
            expanded.emplace_back(out);
        }
        return true;
    }

    VertexBufferOwner upload_vertices(std::span<const GpuVertex> vertices) {
        VertexBufferOwner owner;
        if (vertices.empty() || vertices.size() > std::numeric_limits<std::uint32_t>::max())
            return owner;
        owner.buffer.elemSize = sizeof(GpuVertex);
        owner.buffer.elemCount = static_cast<std::uint32_t>(vertices.size());
        owner.buffer.flags = GX2R_RESOURCE_BIND_VERTEX_BUFFER | GX2R_RESOURCE_USAGE_CPU_WRITE |
                             GX2R_RESOURCE_USAGE_GPU_READ;
        if (!GX2RCreateBuffer(&owner.buffer))
            return owner;
        owner.created = true;
        auto *memory = static_cast<GpuVertex *>(GX2RLockBufferEx(&owner.buffer, kNoResourceFlags));
        if (!memory) {
            owner.reset();
            return owner;
        }
        std::memcpy(memory, vertices.data(), vertices.size_bytes());
        GX2RUnlockBufferEx(&owner.buffer, kNoResourceFlags);
        return owner;
    }

    Viewport viewport_for(Extent2D target, Extent2D logical_size, bool screen_output) const noexcept {
        Viewport result{};
        if (target.width == 0 || target.height == 0 || logical_size.width == 0 || logical_size.height == 0)
            return result;
        if (screen_output) {
            const float sx = static_cast<float>(target.width) / logical_size.width;
            const float sy = static_cast<float>(target.height) / logical_size.height;
            const float scale = std::min(sx, sy);
            result.width = logical_size.width * scale;
            result.height = logical_size.height * scale;
            result.x = (target.width - result.width) * 0.5F;
            result.y = (target.height - result.height) * 0.5F;
        } else {
            result.x = 0;
            result.y = 0;
            result.width = static_cast<float>(target.width);
            result.height = static_cast<float>(target.height);
        }
        result.scale_x = result.width / logical_size.width;
        result.scale_y = result.height / logical_size.height;
        return result;
    }

    void set_viewport_and_clip(Extent2D target,
                               Extent2D logical_size,
                               bool screen_output,
                               std::optional<RectI> clip) noexcept {
        const auto viewport = viewport_for(target, logical_size, screen_output);
        if (viewport.width <= 0 || viewport.height <= 0) {
            GX2SetScissor(0, 0, 0, 0);
            return;
        }
        GX2SetViewport(viewport.x, viewport.y, viewport.width, viewport.height, 0.0F, 1.0F);
        RectI scissor{static_cast<std::int32_t>(std::floor(viewport.x)),
                      static_cast<std::int32_t>(std::floor(viewport.y)),
                      static_cast<std::int32_t>(std::ceil(viewport.width)),
                      static_cast<std::int32_t>(std::ceil(viewport.height))};
        if (clip) {
            const auto clipped = intersect_rect(*clip, logical_size);
            if (clipped.width <= 0 || clipped.height <= 0) {
                GX2SetScissor(0, 0, 0, 0);
                return;
            }
            const auto left = viewport.x + clipped.x * viewport.scale_x;
            const auto top = viewport.y + clipped.y * viewport.scale_y;
            const auto right = viewport.x + (clipped.x + clipped.width) * viewport.scale_x;
            const auto bottom = viewport.y + (clipped.y + clipped.height) * viewport.scale_y;
            scissor = {static_cast<std::int32_t>(std::floor(left)),
                       static_cast<std::int32_t>(std::floor(top)),
                       static_cast<std::int32_t>(std::ceil(right) - std::floor(left)),
                       static_cast<std::int32_t>(std::ceil(bottom) - std::floor(top))};
        }
        scissor = intersect_rect(scissor, target);
        GX2SetScissor(static_cast<std::uint32_t>(std::max(0, scissor.x)),
                      static_cast<std::uint32_t>(std::max(0, scissor.y)),
                      static_cast<std::uint32_t>(std::max(0, scissor.width)),
                      static_cast<std::uint32_t>(std::max(0, scissor.height)));
    }

    void set_draw_state(const BlendState &blend, const AlphaTestState &alpha_test,
                       ColorWriteMask color_write) noexcept {
        GX2SetColorControl(GX2_LOGIC_OP_COPY, blend.enabled ? 1 : 0, FALSE, TRUE);
        GX2SetBlendControl(GX2_RENDER_TARGET_0, gx2_blend_factor(blend.source_rgb),
                           gx2_blend_factor(blend.destination_rgb), gx2_blend_equation(blend.equation_rgb),
                           TRUE, gx2_blend_factor(blend.source_alpha),
                           gx2_blend_factor(blend.destination_alpha),
                           gx2_blend_equation(blend.equation_alpha));
        GX2SetBlendConstantColor(blend.constant.r / 255.0F, blend.constant.g / 255.0F,
                                 blend.constant.b / 255.0F, blend.constant.a / 255.0F);
        GX2SetTargetChannelMasks(gx2_channel_mask(color_write), GX2_CHANNEL_MASK_RGBA,
                                 GX2_CHANNEL_MASK_RGBA, GX2_CHANNEL_MASK_RGBA,
                                 GX2_CHANNEL_MASK_RGBA, GX2_CHANNEL_MASK_RGBA,
                                 GX2_CHANNEL_MASK_RGBA, GX2_CHANNEL_MASK_RGBA);
        GX2SetAlphaTest(alpha_test.enabled, gx2_compare(alpha_test.compare),
                        alpha_test.reference / 255.0F);
        GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
        GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
        GX2SetRasterizerClipControl(TRUE, TRUE);
    }

    const GX2Texture *find_texture(ImageSource source) noexcept {
        if (const auto *handle = std::get_if<TextureHandle>(&source)) {
            const auto found = textures.find(handle->id);
            return found == textures.end() || !found->second.valid ? nullptr : &found->second.texture;
        }
        if (const auto *handle = std::get_if<SurfaceHandle>(&source)) {
            auto found = surfaces.find(handle->id);
            if (found == surfaces.end() || !found->second.valid || !found->second.sampleable)
                return nullptr;
            auto &surface = found->second;
            if (surface.texture_cache_dirty) {
                GX2DrawDone();
                GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, surface.color.surface.image,
                              surface.color.surface.imageSize);
                surface.texture_cache_dirty = false;
            }
            return &surface.texture;
        }
        return &white_texture;
    }

    void bind_pipeline(const GX2Texture &texture, const SamplerState &state) noexcept {
        GX2SetShaderMode(shader_group.vertexShader->mode);
        GX2SetFetchShader(&shader_group.fetchShader);
        GX2SetVertexShader(shader_group.vertexShader);
        GX2SetPixelShader(shader_group.pixelShader);
        GX2Sampler active_sampler{};
        const auto min_filter = state.min_filter == FilterMode::Linear ? GX2_TEX_XY_FILTER_MODE_LINEAR
                                                                       : GX2_TEX_XY_FILTER_MODE_POINT;
        const auto mag_filter = state.mag_filter == FilterMode::Linear ? GX2_TEX_XY_FILTER_MODE_LINEAR
                                                                       : GX2_TEX_XY_FILTER_MODE_POINT;
        GX2InitSampler(&active_sampler, gx2_address_mode(state.address_u), mag_filter);
        GX2InitSamplerClamping(&active_sampler, gx2_address_mode(state.address_u),
                               gx2_address_mode(state.address_v), GX2_TEX_CLAMP_MODE_CLAMP);
        GX2InitSamplerXYFilter(&active_sampler, mag_filter, min_filter, GX2_TEX_ANISO_RATIO_NONE);
        GX2InitSamplerZMFilter(&active_sampler, GX2_TEX_Z_FILTER_MODE_NONE, GX2_TEX_MIP_FILTER_MODE_NONE);
        GX2SetPixelTexture(&texture, sampler_location);
        GX2SetPixelSampler(&active_sampler, sampler_location);
    }

    void bind_screen(bool tv) noexcept {
        if (tv) {
            WHBGfxBeginRenderTV();
            GX2SetColorBuffer(WHBGfxGetTVColourBuffer(), GX2_RENDER_TARGET_0);
            GX2SetDepthBuffer(WHBGfxGetTVDepthBuffer());
        } else {
            WHBGfxBeginRenderDRC();
            GX2SetColorBuffer(WHBGfxGetDRCColourBuffer(), GX2_RENDER_TARGET_0);
            GX2SetDepthBuffer(WHBGfxGetDRCDepthBuffer());
        }
    }

    Extent2D screen_size(bool tv) const noexcept {
        const auto *color = tv ? WHBGfxGetTVColourBuffer() : WHBGfxGetDRCColourBuffer();
        if (!color)
            return {};
        return {color->surface.width, color->surface.height};
    }

    bool draw_vertex_buffer(const GX2RBuffer &vertices,
                            std::uint32_t vertex_count,
                            const GX2Texture &texture,
                            const BlendState &blend,
                            const AlphaTestState &alpha_test,
                            ColorWriteMask color_write,
                            const SamplerState &sampler_state,
                            std::optional<RectI> clip,
                            Surface *surface_target = nullptr) noexcept {
        if (surface_target) {
            GX2SetContextState(WHBGfxGetTVContextState());
            GX2SetColorBuffer(&surface_target->color, GX2_RENDER_TARGET_0);
            set_viewport_and_clip(surface_target->size, surface_target->size, false, clip);
            bind_pipeline(texture, sampler_state);
            set_draw_state(blend, alpha_test, color_write);
            GX2SetAttribBuffer(0, vertices.elemCount * vertices.elemSize, sizeof(GpuVertex), vertices.buffer);
            GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, vertex_count, 0, 1);
            surface_target->texture_cache_dirty = true;
            return true;
        }

        for (const bool tv : {true, false}) {
            const auto target = screen_size(tv);
            if (!valid_size(target))
                continue;
            bind_screen(tv);
            set_viewport_and_clip(target, frame.output_size, true, clip);
            bind_pipeline(texture, sampler_state);
            set_draw_state(blend, alpha_test, color_write);
            GX2SetAttribBuffer(0, vertices.elemCount * vertices.elemSize, sizeof(GpuVertex), vertices.buffer);
            GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, vertex_count, 0, 1);
        }
        return true;
    }

    bool draw_clear(const ClearCommand &command, Surface *target_surface) {
        if (!command.clip) {
            if (target_surface) {
                GX2SetContextState(WHBGfxGetTVContextState());
                GX2SetColorBuffer(&target_surface->color, GX2_RENDER_TARGET_0);
                GX2ClearColor(&target_surface->color, command.color.r / 255.0F,
                              command.color.g / 255.0F, command.color.b / 255.0F,
                              command.color.a / 255.0F);
                target_surface->texture_cache_dirty = true;
            } else {
                for (const bool tv : {true, false}) {
                    bind_screen(tv);
                    auto *color = tv ? WHBGfxGetTVColourBuffer() : WHBGfxGetDRCColourBuffer();
                    GX2ClearColor(color, command.color.r / 255.0F, command.color.g / 255.0F,
                                  command.color.b / 255.0F, command.color.a / 255.0F);
                }
            }
            return true;
        }

        const Extent2D logical_size = target_surface ? target_surface->size : frame.output_size;
        const RectI clipped = intersect_rect(*command.clip, logical_size);
        if (clipped.width <= 0 || clipped.height <= 0)
            return true;
        if ((!target_surface ||
             (target_surface->size.width == logical_size.width &&
              target_surface->size.height == logical_size.height)) &&
            clipped.x == 0 && clipped.y == 0 &&
            static_cast<std::uint32_t>(clipped.width) == logical_size.width &&
            static_cast<std::uint32_t>(clipped.height) == logical_size.height) {
            ClearCommand full = command;
            full.clip.reset();
            return draw_clear(full, target_surface);
        }

        const float left = static_cast<float>(clipped.x);
        const float top = static_cast<float>(clipped.y);
        const float right = left + clipped.width;
        const float bottom = top + clipped.height;
        const float iw = 1.0F / logical_size.width;
        const float ih = 1.0F / logical_size.height;
        const float x0 = left * iw * 2.0F - 1.0F;
        const float x1 = right * iw * 2.0F - 1.0F;
        const float y0 = 1.0F - top * ih * 2.0F;
        const float y1 = 1.0F - bottom * ih * 2.0F;
        const GpuVertex quad[6] = {
            {x0, y0, 0, 0, {command.color.r, command.color.g, command.color.b, command.color.a}},
            {x1, y0, 0, 0, {command.color.r, command.color.g, command.color.b, command.color.a}},
            {x1, y1, 0, 0, {command.color.r, command.color.g, command.color.b, command.color.a}},
            {x0, y0, 0, 0, {command.color.r, command.color.g, command.color.b, command.color.a}},
            {x1, y1, 0, 0, {command.color.r, command.color.g, command.color.b, command.color.a}},
            {x0, y1, 0, 0, {command.color.r, command.color.g, command.color.b, command.color.a}},
        };
        auto vertex_buffer = upload_vertices(quad);
        if (!vertex_buffer.created)
            return false;
        const BlendState opaque{.enabled = false};
        const AlphaTestState no_alpha_test{};
        const ColorWriteMask all_channels{};
        const auto clip = command.clip;
        const bool drawn = draw_vertex_buffer(vertex_buffer.buffer, 6, white_texture, opaque,
                                              no_alpha_test, all_channels, SamplerState{}, clip, target_surface);
        frame_vertex_buffers.emplace_back(std::move(vertex_buffer));
        return drawn;
    }

    SubmitResult draw_triangles(const DrawTrianglesCommand &command) {
        if (command.target.surface.valid() && !surfaces.contains(command.target.surface.id))
            return SubmitResult::Unsupported;
        Surface *target_surface = nullptr;
        if (command.target.surface.valid())
            target_surface = &surfaces.find(command.target.surface.id)->second;
        const Extent2D logical_size = target_surface ? target_surface->size : frame.output_size;
        if (const auto *surface_source = std::get_if<SurfaceHandle>(&command.source);
            surface_source && surface_source->id == command.target.surface.id && command.target.surface.valid())
            return SubmitResult::Unsupported;
        const auto *texture = find_texture(command.source);
        if (!texture)
            return SubmitResult::Unsupported;

        std::vector<GpuVertex> expanded;
        if (!build_vertex_buffer(command.vertices, command.indices, command.transform, command.tint,
                                 command.opacity, logical_size, expanded))
            return SubmitResult::Unsupported;
        auto vertex_buffer = upload_vertices(expanded);
        if (!vertex_buffer.created)
            return SubmitResult::Failed;

        const auto *active_texture = texture;
        const bool drawn = draw_vertex_buffer(vertex_buffer.buffer,
                                              static_cast<std::uint32_t>(expanded.size()),
                                              *active_texture, command.blend, command.alpha_test,
                                              command.color_write, command.sampler, command.clip, target_surface);
        frame_vertex_buffers.emplace_back(std::move(vertex_buffer));
        return drawn ? SubmitResult::Submitted : SubmitResult::Failed;
    }

    void release_frame_resources() noexcept {
        frame_vertex_buffers.clear();
        frame_staging_surfaces.clear();
        deferred_surfaces.clear();
    }

    void finish_gpu_work() noexcept {
        if (!frame_vertex_buffers.empty() || !frame_staging_surfaces.empty() ||
            !deferred_surfaces.empty())
            GX2DrawDone();
        release_frame_resources();
    }
};

WiiUGX2Renderer::WiiUGX2Renderer() : impl_(std::make_unique<Impl>()) { impl_->initialize(); }

WiiUGX2Renderer::~WiiUGX2Renderer() = default;

bool WiiUGX2Renderer::ready() const noexcept { return impl_ && impl_->initialized; }

RendererCapabilities WiiUGX2Renderer::capabilities() const noexcept {
    return {.hardware_accelerated = ready(),
            .render_targets = ready(),
            .texture_sampling = ready(),
            .readback = ready(),
            .max_texture_size = ready() ? kMaxTextureDimension : 0};
}

Extent2D WiiUGX2Renderer::presentation_size() const noexcept {
    return ready() ? impl_->screen_size(true) : Extent2D{};
}

bool WiiUGX2Renderer::begin_frame(const FrameInfo &frame) {
    if (!ready() || frame.output_size.width == 0 || frame.output_size.height == 0)
        return false;
    if (impl_->frame_active && !impl_->frame_ended)
        return false;
    impl_->finish_gpu_work();
    WHBGfxBeginRender();
    impl_->frame = frame;
    impl_->frame_active = true;
    impl_->frame_ended = false;
    return true;
}

bool WiiUGX2Renderer::resize_frame_output(Extent2D size) {
    if (!ready() || !impl_->frame_active || impl_->frame_ended || !valid_size(size))
        return false;
    impl_->frame.output_size = size;
    return true;
}

SubmitResult WiiUGX2Renderer::submit(const RenderCommand &command) {
    if (!ready() || !impl_->frame_active || impl_->frame_ended)
        return SubmitResult::Failed;
    if (const auto *clear = std::get_if<ClearCommand>(&command)) {
        Impl::Surface *target_surface = nullptr;
        if (clear->target.surface.valid()) {
            const auto found = impl_->surfaces.find(clear->target.surface.id);
            if (found == impl_->surfaces.end())
                return SubmitResult::Unsupported;
            target_surface = &found->second;
        }
        return impl_->draw_clear(*clear, target_surface) ? SubmitResult::Submitted : SubmitResult::Failed;
    }
    if (const auto *draw = std::get_if<DrawTrianglesCommand>(&command))
        return impl_->draw_triangles(*draw);
    return SubmitResult::Unsupported;
}

bool WiiUGX2Renderer::end_frame() {
    if (!ready() || !impl_->frame_active || impl_->frame_ended)
        return false;
    WHBGfxFinishRenderTV();
    WHBGfxFinishRenderDRC();
    impl_->frame_ended = true;
    return true;
}

void WiiUGX2Renderer::present() {
    if (!ready())
        return;
    if (impl_->frame_active && !impl_->frame_ended)
        end_frame();
    WHBGfxFinishRender();
    // WHBGfxFinishRender waits for GX2 work before swapping the buffers, so
    // frame-owned GPU resources can be released without another global wait.
    impl_->release_frame_resources();
    impl_->frame_active = false;
    impl_->frame_ended = false;
}

bool WiiUGX2Renderer::update_surface(SurfaceHandle handle, ConstPixelBufferView pixels) {
    if (!ready() || (impl_->frame_active && impl_->frame_ended))
        return false;
    const auto found = impl_->surfaces.find(handle.id);
    if (found == impl_->surfaces.end() || !found->second.valid ||
        pixels.size.width != found->second.size.width || pixels.size.height != found->second.size.height)
        return false;
    const RectI full{0, 0, static_cast<std::int32_t>(found->second.size.width),
                     static_cast<std::int32_t>(found->second.size.height)};
    return impl_->upload_pixels(found->second.color.surface, full, pixels);
}

bool WiiUGX2Renderer::create_texture(TextureHandle handle,
                                     const TextureDesc &description,
                                     ConstPixelBufferView initial_pixels) {
    if (!ready() || !handle.valid() || !valid_size(description.size) || description.mipmapped)
        return false;
    auto existing = impl_->textures.find(handle.id);
    if (existing != impl_->textures.end()) {
        if (existing->second.valid) {
            SurfaceOwner old;
            old.surface = existing->second.texture.surface;
            old.created = true;
            impl_->deferred_surfaces.emplace_back(std::move(old));
        }
        impl_->textures.erase(existing);
    }
    Impl::Texture texture{};
    if (!Impl::create_texture_resource(texture, description))
        return false;
    const auto [inserted, ok] = impl_->textures.emplace(handle.id, texture);
    if (!ok) {
        GX2RDestroySurfaceEx(&texture.texture.surface, kNoResourceFlags);
        return false;
    }
    if (!initial_pixels.bytes.empty()) {
        const RectI full{0, 0, static_cast<std::int32_t>(description.size.width),
                         static_cast<std::int32_t>(description.size.height)};
        if (!impl_->upload_pixels(inserted->second.texture.surface, full, initial_pixels)) {
            SurfaceOwner failed;
            failed.surface = inserted->second.texture.surface;
            failed.created = true;
            impl_->deferred_surfaces.emplace_back(std::move(failed));
            impl_->textures.erase(inserted);
            return false;
        }
    }
    return true;
}

bool WiiUGX2Renderer::update_texture(TextureHandle handle, RectI destination, ConstPixelBufferView pixels) {
    if (!ready())
        return false;
    const auto found = impl_->textures.find(handle.id);
    if (found == impl_->textures.end() || !found->second.valid)
        return false;
    if (!impl_->frame_vertex_buffers.empty())
        GX2DrawDone();
    return impl_->upload_pixels(found->second.texture.surface, destination, pixels);
}

void WiiUGX2Renderer::destroy_texture(TextureHandle handle) {
    if (!impl_)
        return;
    const auto found = impl_->textures.find(handle.id);
    if (found == impl_->textures.end())
        return;
    if (found->second.valid) {
        SurfaceOwner old;
        old.surface = found->second.texture.surface;
        old.created = true;
        impl_->deferred_surfaces.emplace_back(std::move(old));
    }
    impl_->textures.erase(found);
}

bool WiiUGX2Renderer::create_surface(SurfaceHandle handle, const SurfaceDesc &description) {
    if (!ready() || !handle.valid() || !valid_size(description.size) ||
        description.format != PixelFormat::RGBA8)
        return false;
    auto existing = impl_->surfaces.find(handle.id);
    if (existing != impl_->surfaces.end()) {
        if (existing->second.valid) {
            SurfaceOwner old;
            old.surface = existing->second.color.surface;
            old.created = true;
            impl_->deferred_surfaces.emplace_back(std::move(old));
        }
        impl_->surfaces.erase(existing);
    }
    Impl::Surface surface{};
    if (!Impl::create_surface_resource(surface, description))
        return false;
    const auto [inserted, ok] = impl_->surfaces.emplace(handle.id, surface);
    if (!ok) {
        GX2RDestroySurfaceEx(&surface.color.surface, kNoResourceFlags);
        return false;
    }
    return true;
}

void WiiUGX2Renderer::destroy_surface(SurfaceHandle handle) {
    if (!impl_)
        return;
    const auto found = impl_->surfaces.find(handle.id);
    if (found == impl_->surfaces.end())
        return;
    if (found->second.valid) {
        SurfaceOwner old;
        old.surface = found->second.color.surface;
        old.created = true;
        impl_->deferred_surfaces.emplace_back(std::move(old));
    }
    impl_->surfaces.erase(found);
}

bool WiiUGX2Renderer::read_surface(SurfaceHandle handle, PixelBufferView destination) {
    if (!ready())
        return false;
    if (!handle.valid()) {
        const Extent2D logical_size = impl_->frame.output_size;
        auto *screen = WHBGfxGetTVColourBuffer();
        if (!screen || !valid_size(logical_size) ||
            destination.size.width != logical_size.width || destination.size.height != logical_size.height)
            return false;

        const Extent2D physical_size{screen->surface.width, screen->surface.height};
        const auto output_pixel_size = pixel_size(destination.format);
        const auto output_row_bytes = static_cast<std::size_t>(logical_size.width) * output_pixel_size;
        const auto pixel_count = static_cast<std::uint64_t>(physical_size.width) * physical_size.height;
        const auto physical_bytes = pixel_count * 4;
        if (!valid_size(physical_size) || output_pixel_size == 0 || destination.row_pitch < output_row_bytes ||
            destination.row_pitch > std::numeric_limits<std::size_t>::max() / logical_size.height ||
            destination.bytes.size() < destination.row_pitch * logical_size.height ||
            physical_bytes > std::numeric_limits<std::size_t>::max())
            return false;

        std::vector<std::byte> physical_pixels(static_cast<std::size_t>(physical_bytes));
        PixelBufferView physical_view{physical_pixels, physical_size,
                                      static_cast<std::size_t>(physical_size.width) * 4, PixelFormat::RGBA8};
        if (!impl_->read_pixels(screen->surface, physical_view))
            return false;

        const auto viewport = impl_->viewport_for(physical_size, logical_size, true);
        auto *output = reinterpret_cast<std::uint8_t *>(destination.bytes.data());
        const auto *input = reinterpret_cast<const std::uint8_t *>(physical_pixels.data());
        const auto input_pitch = static_cast<std::size_t>(physical_size.width) * 4;
        for (std::uint32_t y = 0; y < logical_size.height; ++y) {
            const auto physical_y = std::min(
                physical_size.height - 1,
                static_cast<std::uint32_t>(viewport.y + (static_cast<float>(y) + 0.5F) * viewport.scale_y));
            const auto *src = input + static_cast<std::size_t>(physical_y) * input_pitch;
            auto *dst = output + static_cast<std::size_t>(y) * destination.row_pitch;
            for (std::uint32_t x = 0; x < logical_size.width; ++x) {
                const auto physical_x = std::min(
                    physical_size.width - 1,
                    static_cast<std::uint32_t>(viewport.x + (static_cast<float>(x) + 0.5F) * viewport.scale_x));
                const auto *pixel = src + static_cast<std::size_t>(physical_x) * 4;
                switch (destination.format) {
                case PixelFormat::RGBA8:
                    std::memcpy(dst + static_cast<std::size_t>(x) * 4, pixel, 4);
                    break;
                case PixelFormat::BGRA8:
                    dst[static_cast<std::size_t>(x) * 4 + 0] = pixel[2];
                    dst[static_cast<std::size_t>(x) * 4 + 1] = pixel[1];
                    dst[static_cast<std::size_t>(x) * 4 + 2] = pixel[0];
                    dst[static_cast<std::size_t>(x) * 4 + 3] = pixel[3];
                    break;
                case PixelFormat::R8:
                    dst[x] = pixel[0];
                    break;
                case PixelFormat::A8:
                    dst[x] = pixel[3];
                    break;
                }
            }
        }
        return true;
    }
    const auto found = impl_->surfaces.find(handle.id);
    return found != impl_->surfaces.end() && found->second.valid &&
           impl_->read_pixels(found->second.color.surface, destination);
}

} // namespace regml::render

#else

namespace regml::render {

struct WiiUGX2Renderer::Impl {};

WiiUGX2Renderer::WiiUGX2Renderer() : impl_(std::make_unique<Impl>()) {}
WiiUGX2Renderer::~WiiUGX2Renderer() = default;
bool WiiUGX2Renderer::ready() const noexcept { return false; }
RendererCapabilities WiiUGX2Renderer::capabilities() const noexcept { return {}; }
Extent2D WiiUGX2Renderer::presentation_size() const noexcept { return {}; }
bool WiiUGX2Renderer::begin_frame(const FrameInfo &) { return false; }
bool WiiUGX2Renderer::resize_frame_output(Extent2D) { return false; }
SubmitResult WiiUGX2Renderer::submit(const RenderCommand &) { return SubmitResult::Unsupported; }
bool WiiUGX2Renderer::end_frame() { return false; }
void WiiUGX2Renderer::present() {}
bool WiiUGX2Renderer::update_surface(SurfaceHandle, ConstPixelBufferView) { return false; }
bool WiiUGX2Renderer::create_texture(TextureHandle, const TextureDesc &, ConstPixelBufferView) { return false; }
bool WiiUGX2Renderer::update_texture(TextureHandle, RectI, ConstPixelBufferView) { return false; }
void WiiUGX2Renderer::destroy_texture(TextureHandle) {}
bool WiiUGX2Renderer::create_surface(SurfaceHandle, const SurfaceDesc &) { return false; }
void WiiUGX2Renderer::destroy_surface(SurfaceHandle) {}
bool WiiUGX2Renderer::read_surface(SurfaceHandle, PixelBufferView) { return false; }

} // namespace regml::render

#endif
