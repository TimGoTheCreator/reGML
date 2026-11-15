#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

namespace regml::render {

struct TextureHandle {
    std::uint32_t id = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return id != 0; }
    friend constexpr bool operator==(TextureHandle, TextureHandle) noexcept = default;
};

struct SurfaceHandle {
    std::uint32_t id = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return id != 0; }
    friend constexpr bool operator==(SurfaceHandle, SurfaceHandle) noexcept = default;
};

struct Extent2D {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct RectI {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
};

struct Vec2 {
    float x = 0.0F;
    float y = 0.0F;
};

struct ColorRGBA8 {
    std::uint8_t r = 255;
    std::uint8_t g = 255;
    std::uint8_t b = 255;
    std::uint8_t a = 255;
};

struct Affine2D {
    float m00 = 1.0F;
    float m01 = 0.0F;
    float m10 = 0.0F;
    float m11 = 1.0F;
    float tx = 0.0F;
    float ty = 0.0F;
};

enum class PixelFormat : std::uint8_t { RGBA8, BGRA8, R8, A8 };

struct ConstPixelBufferView {
    std::span<const std::byte> bytes{};
    Extent2D size{};
    std::size_t row_pitch = 0;
    PixelFormat format = PixelFormat::RGBA8;
};

struct PixelBufferView {
    std::span<std::byte> bytes{};
    Extent2D size{};
    std::size_t row_pitch = 0;
    PixelFormat format = PixelFormat::RGBA8;
};

struct TextureDesc {
    Extent2D size{};
    PixelFormat format = PixelFormat::RGBA8;
    bool mipmapped = false;
};

struct SurfaceDesc {
    Extent2D size{};
    PixelFormat format = PixelFormat::RGBA8;
    bool sampleable = true;
};

enum class BlendFactor : std::uint8_t {
    Zero,
    One,
    SourceColor,
    OneMinusSourceColor,
    DestinationColor,
    OneMinusDestinationColor,
    SourceAlpha,
    OneMinusSourceAlpha,
    DestinationAlpha,
    OneMinusDestinationAlpha,
    ConstantColor,
    OneMinusConstantColor,
    ConstantAlpha,
    OneMinusConstantAlpha,
    SourceAlphaSaturate,
};

enum class BlendEquation : std::uint8_t { Add, Subtract, ReverseSubtract, Minimum, Maximum };

struct BlendState {
    bool enabled = true;
    BlendFactor source_rgb = BlendFactor::SourceAlpha;
    BlendFactor destination_rgb = BlendFactor::OneMinusSourceAlpha;
    BlendFactor source_alpha = BlendFactor::One;
    BlendFactor destination_alpha = BlendFactor::OneMinusSourceAlpha;
    BlendEquation equation_rgb = BlendEquation::Add;
    BlendEquation equation_alpha = BlendEquation::Add;
    ColorRGBA8 constant{0, 0, 0, 0};
};

enum class AlphaCompare : std::uint8_t { Never, Less, LessEqual, Equal, GreaterEqual, Greater, NotEqual, Always };

struct AlphaTestState {
    bool enabled = false;
    AlphaCompare compare = AlphaCompare::Greater;
    std::uint8_t reference = 0;
};

struct ColorWriteMask {
    bool red = true;
    bool green = true;
    bool blue = true;
    bool alpha = true;
};

enum class FilterMode : std::uint8_t { Nearest, Linear };
enum class AddressMode : std::uint8_t { Clamp, Repeat, Mirror };

struct SamplerState {
    FilterMode min_filter = FilterMode::Nearest;
    FilterMode mag_filter = FilterMode::Nearest;
    AddressMode address_u = AddressMode::Clamp;
    AddressMode address_v = AddressMode::Clamp;
};

struct Vertex2D {
    float x = 0.0F;
    float y = 0.0F;
    float u = 0.0F;
    float v = 0.0F;
    ColorRGBA8 color{};
};

struct RenderTarget {
    SurfaceHandle surface{};
};

using ImageSource = std::variant<std::monostate, TextureHandle, SurfaceHandle>;

struct ClearCommand {
    RenderTarget target{};
    ColorRGBA8 color{0, 0, 0, 255};
    std::optional<RectI> clip{};
};

struct DrawTrianglesCommand {
    RenderTarget target{};
    ImageSource source{};
    std::span<const Vertex2D> vertices{};
    std::span<const std::uint32_t> indices{};
    Affine2D transform{};
    ColorRGBA8 tint{};
    float opacity = 1.0F;
    SamplerState sampler{};
    BlendState blend{};
    AlphaTestState alpha_test{};
    ColorWriteMask color_write{};
    std::optional<RectI> clip{};
};

using RenderCommand = std::variant<ClearCommand, DrawTrianglesCommand>;

struct FrameInfo {
    Extent2D output_size{};
    std::uint64_t frame_index = 0;
};

enum class SubmitResult : std::uint8_t { Submitted, Unsupported, Failed };

struct RendererCapabilities {
    bool hardware_accelerated = false;
    bool render_targets = true;
    bool texture_sampling = true;
    bool readback = true;
    std::uint32_t max_texture_size = 0;
};

} // namespace regml::render
