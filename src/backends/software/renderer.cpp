#include "backends/software/renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <variant>

namespace regml::render {
namespace {

constexpr std::uint32_t pack(ColorRGBA8 color) noexcept {
    return (static_cast<std::uint32_t>(color.a) << 24) | (static_cast<std::uint32_t>(color.r) << 16) |
           (static_cast<std::uint32_t>(color.g) << 8) | color.b;
}

ColorRGBA8 unpack(std::uint32_t color) noexcept {
    return {static_cast<std::uint8_t>(color >> 16),
            static_cast<std::uint8_t>(color >> 8),
            static_cast<std::uint8_t>(color),
            static_cast<std::uint8_t>(color >> 24)};
}

bool valid_view(ConstPixelBufferView view) noexcept {
    const auto bytes_per_pixel = view.format == PixelFormat::R8 || view.format == PixelFormat::A8 ? 1u : 4u;
    const auto row_bytes = static_cast<std::uint64_t>(view.size.width) * bytes_per_pixel;
    if (row_bytes > std::numeric_limits<std::size_t>::max() || view.row_pitch < row_bytes)
        return false;
    if (view.size.height == 0 || view.size.width == 0)
        return view.bytes.empty() || view.bytes.size() >= row_bytes;
    const auto required = static_cast<std::uint64_t>(view.row_pitch) * (view.size.height - 1) + row_bytes;
    return required <= view.bytes.size();
}

bool valid_view(PixelBufferView view) noexcept {
    return valid_view(ConstPixelBufferView{view.bytes, view.size, view.row_pitch, view.format});
}

std::uint32_t read_pixel(const std::byte *pixel, PixelFormat format) noexcept {
    switch (format) {
    case PixelFormat::RGBA8:
        return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(pixel[3])) << 24) |
               (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(pixel[0])) << 16) |
               (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(pixel[1])) << 8) |
               std::to_integer<std::uint8_t>(pixel[2]);
    case PixelFormat::BGRA8:
        return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(pixel[3])) << 24) |
               (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(pixel[2])) << 16) |
               (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(pixel[1])) << 8) |
               std::to_integer<std::uint8_t>(pixel[0]);
    case PixelFormat::R8: {
        const auto red = std::to_integer<std::uint8_t>(pixel[0]);
        return 0xff000000U | (static_cast<std::uint32_t>(red) << 16);
    }
    case PixelFormat::A8: {
        const auto alpha = std::to_integer<std::uint8_t>(pixel[0]);
        return (static_cast<std::uint32_t>(alpha) << 24) | 0x00ffffffU;
    }
    }
    return 0;
}

void write_pixel(std::byte *pixel, PixelFormat format, std::uint32_t color) noexcept {
    const auto rgba = unpack(color);
    switch (format) {
    case PixelFormat::RGBA8:
        pixel[0] = static_cast<std::byte>(rgba.r);
        pixel[1] = static_cast<std::byte>(rgba.g);
        pixel[2] = static_cast<std::byte>(rgba.b);
        pixel[3] = static_cast<std::byte>(rgba.a);
        break;
    case PixelFormat::BGRA8:
        pixel[0] = static_cast<std::byte>(rgba.b);
        pixel[1] = static_cast<std::byte>(rgba.g);
        pixel[2] = static_cast<std::byte>(rgba.r);
        pixel[3] = static_cast<std::byte>(rgba.a);
        break;
    case PixelFormat::R8:
        pixel[0] = static_cast<std::byte>(rgba.r);
        break;
    case PixelFormat::A8:
        pixel[0] = static_cast<std::byte>(rgba.a);
        break;
    }
}

double channel(const ColorRGBA8 &color, unsigned index) noexcept {
    switch (index) {
    case 0:
        return color.r / 255.0;
    case 1:
        return color.g / 255.0;
    case 2:
        return color.b / 255.0;
    default:
        return color.a / 255.0;
    }
}

double blend_factor(BlendFactor factor,
                    unsigned channel_index,
                    const ColorRGBA8 &source,
                    const ColorRGBA8 &destination,
                    const ColorRGBA8 &constant) noexcept {
    switch (factor) {
    case BlendFactor::Zero:
        return 0.0;
    case BlendFactor::One:
        return 1.0;
    case BlendFactor::SourceColor:
        return channel(source, channel_index);
    case BlendFactor::OneMinusSourceColor:
        return 1.0 - channel(source, channel_index);
    case BlendFactor::DestinationColor:
        return channel(destination, channel_index);
    case BlendFactor::OneMinusDestinationColor:
        return 1.0 - channel(destination, channel_index);
    case BlendFactor::SourceAlpha:
        return channel(source, 3);
    case BlendFactor::OneMinusSourceAlpha:
        return 1.0 - channel(source, 3);
    case BlendFactor::DestinationAlpha:
        return channel(destination, 3);
    case BlendFactor::OneMinusDestinationAlpha:
        return 1.0 - channel(destination, 3);
    case BlendFactor::ConstantColor:
        return channel(constant, channel_index);
    case BlendFactor::OneMinusConstantColor:
        return 1.0 - channel(constant, channel_index);
    case BlendFactor::ConstantAlpha:
        return channel(constant, 3);
    case BlendFactor::OneMinusConstantAlpha:
        return 1.0 - channel(constant, 3);
    case BlendFactor::SourceAlphaSaturate:
        return channel_index == 3 ? 1.0 : std::min(channel(source, 3), 1.0 - channel(destination, 3));
    }
    return 1.0;
}

std::uint8_t blend_channel(std::uint8_t source,
                           std::uint8_t destination,
                           double source_factor,
                           double destination_factor,
                           BlendEquation equation) noexcept {
    const double src = source / 255.0 * source_factor;
    const double dst = destination / 255.0 * destination_factor;
    double result = 0.0;
    switch (equation) {
    case BlendEquation::Add:
        result = src + dst;
        break;
    case BlendEquation::Subtract:
        result = src - dst;
        break;
    case BlendEquation::ReverseSubtract:
        result = dst - src;
        break;
    case BlendEquation::Minimum:
        result = std::min(src, dst);
        break;
    case BlendEquation::Maximum:
        result = std::max(src, dst);
        break;
    }
    return static_cast<std::uint8_t>(std::clamp(std::lround(std::clamp(result, 0.0, 1.0) * 255.0), 0L, 255L));
}

ColorRGBA8 blend(ColorRGBA8 source, ColorRGBA8 destination, const BlendState &state) noexcept {
    if (!state.enabled)
        return source;
    ColorRGBA8 result{};
    const std::uint8_t src[] = {source.r, source.g, source.b, source.a};
    const std::uint8_t dst[] = {destination.r, destination.g, destination.b, destination.a};
    std::uint8_t *out[] = {&result.r, &result.g, &result.b, &result.a};
    for (unsigned i = 0; i < 4; ++i) {
        const bool alpha = i == 3;
        const auto sf = alpha ? state.source_alpha : state.source_rgb;
        const auto df = alpha ? state.destination_alpha : state.destination_rgb;
        const auto equation = alpha ? state.equation_alpha : state.equation_rgb;
        *out[i] = blend_channel(src[i], dst[i],
                                blend_factor(sf, i, source, destination, state.constant),
                                blend_factor(df, i, source, destination, state.constant), equation);
    }
    return result;
}

bool alpha_pass(std::uint8_t value, const AlphaTestState &test) noexcept {
    if (!test.enabled)
        return true;
    switch (test.compare) {
    case AlphaCompare::Never:
        return false;
    case AlphaCompare::Less:
        return value < test.reference;
    case AlphaCompare::LessEqual:
        return value <= test.reference;
    case AlphaCompare::Equal:
        return value == test.reference;
    case AlphaCompare::GreaterEqual:
        return value >= test.reference;
    case AlphaCompare::Greater:
        return value > test.reference;
    case AlphaCompare::NotEqual:
        return value != test.reference;
    case AlphaCompare::Always:
        return true;
    }
    return false;
}

int address_coordinate(int coordinate, int extent, AddressMode mode) noexcept {
    if (extent <= 1)
        return 0;
    switch (mode) {
    case AddressMode::Clamp:
        return std::clamp(coordinate, 0, extent - 1);
    case AddressMode::Repeat: {
        const int wrapped = coordinate % extent;
        return wrapped < 0 ? wrapped + extent : wrapped;
    }
    case AddressMode::Mirror: {
        const int period = extent * 2;
        const int wrapped = ((coordinate % period) + period) % period;
        return wrapped < extent ? wrapped : period - 1 - wrapped;
    }
    }
    return std::clamp(coordinate, 0, extent - 1);
}

struct SampledImage {
    Extent2D size{};
    const std::vector<std::uint32_t> *pixels = nullptr;

    std::uint32_t get(int x, int y, const SamplerState &sampler) const noexcept {
        x = address_coordinate(x, static_cast<int>(size.width), sampler.address_u);
        y = address_coordinate(y, static_cast<int>(size.height), sampler.address_v);
        return (*pixels)[static_cast<std::size_t>(y) * size.width + static_cast<std::size_t>(x)];
    }

    std::uint32_t sample(double u, double v, const SamplerState &sampler) const noexcept {
        if (!pixels || size.width == 0 || size.height == 0)
            return 0xffffffffU;
        if (sampler.mag_filter == FilterMode::Nearest && sampler.min_filter == FilterMode::Nearest) {
            const auto x = static_cast<int>(std::floor(u * size.width));
            const auto y = static_cast<int>(std::floor(v * size.height));
            return get(x, y, sampler);
        }
        const double sx = u * size.width - 0.5;
        const double sy = v * size.height - 0.5;
        const int x0 = static_cast<int>(std::floor(sx));
        const int y0 = static_cast<int>(std::floor(sy));
        const double fx = sx - x0;
        const double fy = sy - y0;
        const auto p00 = unpack(get(x0, y0, sampler));
        const auto p10 = unpack(get(x0 + 1, y0, sampler));
        const auto p01 = unpack(get(x0, y0 + 1, sampler));
        const auto p11 = unpack(get(x0 + 1, y0 + 1, sampler));
        const auto lerp = [&](std::uint8_t ColorRGBA8::*member) {
            const double top = p00.*member * (1.0 - fx) + p10.*member * fx;
            const double bottom = p01.*member * (1.0 - fx) + p11.*member * fx;
            return static_cast<std::uint8_t>(std::clamp(std::lround(top * (1.0 - fy) + bottom * fy), 0L, 255L));
        };
        return pack({lerp(&ColorRGBA8::r), lerp(&ColorRGBA8::g), lerp(&ColorRGBA8::b), lerp(&ColorRGBA8::a)});
    }
};

double edge(Vec2 a, Vec2 b, Vec2 p) noexcept {
    return static_cast<double>(b.x - a.x) * (p.y - a.y) - static_cast<double>(b.y - a.y) * (p.x - a.x);
}

bool top_left(Vec2 a, Vec2 b) noexcept {
    const float dy = b.y - a.y;
    const float dx = b.x - a.x;
    return dy < 0.0F || (dy == 0.0F && dx > 0.0F);
}

} // namespace

RendererCapabilities SoftwareRenderer::capabilities() const noexcept {
    return {.hardware_accelerated = false,
            .render_targets = true,
            .texture_sampling = true,
            .readback = true,
            .max_texture_size = 16384};
}

bool SoftwareRenderer::begin_frame(const FrameInfo &frame) {
    if (frame.output_size.width == 0 || frame.output_size.height == 0 || frame.output_size.width > 16384 ||
        frame.output_size.height > 16384)
        return false;
    if (output_.size.width != frame.output_size.width || output_.size.height != frame.output_size.height) {
        const auto count = static_cast<std::uint64_t>(frame.output_size.width) * frame.output_size.height;
        if (count > std::numeric_limits<std::size_t>::max())
            return false;
        try {
            output_.pixels.assign(static_cast<std::size_t>(count), 0);
        } catch (...) {
            return false;
        }
        output_.size = frame.output_size;
    }
    frame_active_ = true;
    return true;
}

SubmitResult SoftwareRenderer::submit(const RenderCommand &command) {
    if (!frame_active_)
        return SubmitResult::Failed;
    return std::visit(
        [&](const auto &entry) -> SubmitResult {
            using T = std::decay_t<decltype(entry)>;
            auto *destination = target(entry.target.surface);
            if (!destination)
                return SubmitResult::Failed;
            if constexpr (std::is_same_v<T, ClearCommand>) {
                const int width = static_cast<int>(destination->size.width);
                const int height = static_cast<int>(destination->size.height);
                int left = 0, top = 0, right = width, bottom = height;
                if (entry.clip) {
                    const auto clip_right = static_cast<std::int64_t>(entry.clip->x) + entry.clip->width;
                    const auto clip_bottom = static_cast<std::int64_t>(entry.clip->y) + entry.clip->height;
                    left = static_cast<int>(std::clamp<std::int64_t>(entry.clip->x, 0, width));
                    top = static_cast<int>(std::clamp<std::int64_t>(entry.clip->y, 0, height));
                    right = static_cast<int>(std::clamp<std::int64_t>(clip_right, 0, width));
                    bottom = static_cast<int>(std::clamp<std::int64_t>(clip_bottom, 0, height));
                }
                if (left < right && top < bottom) {
                    const auto color = pack(entry.color);
                    for (int y = top; y < bottom; ++y) {
                        auto *row = destination->pixels.data() + static_cast<std::size_t>(y) * width;
                        std::fill(row + left, row + right, color);
                    }
                }
                return SubmitResult::Submitted;
            } else {
                return draw(entry);
            }
        },
        command);
}

bool SoftwareRenderer::end_frame() {
    if (!frame_active_)
        return false;
    frame_active_ = false;
    return true;
}

void SoftwareRenderer::present() {}

bool SoftwareRenderer::create_texture(TextureHandle handle,
                                      const TextureDesc &description,
                                      ConstPixelBufferView initial_pixels) {
    if (!handle.valid() || description.size.width == 0 || description.size.height == 0)
        return false;
    const auto count = static_cast<std::uint64_t>(description.size.width) * description.size.height;
    if (count > std::numeric_limits<std::size_t>::max())
        return false;
    Image image{description.size, {}};
    try {
        image.pixels.assign(static_cast<std::size_t>(count), 0);
    } catch (...) {
        return false;
    }
    if (!initial_pixels.bytes.empty() && !copy_into(image, {0, 0, static_cast<int>(description.size.width),
                                                           static_cast<int>(description.size.height)},
                                                    initial_pixels))
        return false;
    textures_.insert_or_assign(handle.id, std::move(image));
    return true;
}

bool SoftwareRenderer::update_texture(TextureHandle handle, RectI destination, ConstPixelBufferView pixels) {
    const auto found = textures_.find(handle.id);
    return found != textures_.end() && copy_into(found->second, destination, pixels);
}

void SoftwareRenderer::destroy_texture(TextureHandle handle) { textures_.erase(handle.id); }

bool SoftwareRenderer::create_surface(SurfaceHandle handle, const SurfaceDesc &description) {
    if (!handle.valid() || description.size.width == 0 || description.size.height == 0)
        return false;
    const auto count = static_cast<std::uint64_t>(description.size.width) * description.size.height;
    if (count > std::numeric_limits<std::size_t>::max())
        return false;
    Image image{description.size, {}};
    try {
        image.pixels.assign(static_cast<std::size_t>(count), 0);
    } catch (...) {
        return false;
    }
    surfaces_.insert_or_assign(handle.id, std::move(image));
    return true;
}

void SoftwareRenderer::destroy_surface(SurfaceHandle handle) { surfaces_.erase(handle.id); }

bool SoftwareRenderer::read_surface(SurfaceHandle handle, PixelBufferView destination) {
    const auto *image = target(handle);
    return image && copy_out(*image, destination);
}

bool SoftwareRenderer::update_surface(SurfaceHandle handle, ConstPixelBufferView pixels) {
    auto *image = target(handle);
    if (!image || pixels.size.width != image->size.width || pixels.size.height != image->size.height)
        return false;
    return copy_into(*image, {0, 0, static_cast<int>(image->size.width), static_cast<int>(image->size.height)}, pixels);
}

SoftwareRenderer::Image *SoftwareRenderer::target(SurfaceHandle handle) noexcept {
    if (!handle.valid())
        return output_.size.width == 0 ? nullptr : &output_;
    const auto found = surfaces_.find(handle.id);
    return found == surfaces_.end() ? nullptr : &found->second;
}

const SoftwareRenderer::Image *SoftwareRenderer::target(SurfaceHandle handle) const noexcept {
    if (!handle.valid())
        return output_.size.width == 0 ? nullptr : &output_;
    const auto found = surfaces_.find(handle.id);
    return found == surfaces_.end() ? nullptr : &found->second;
}

bool SoftwareRenderer::copy_into(Image &image, RectI destination, ConstPixelBufferView source) noexcept {
    if (!valid_view(source) || destination.width < 0 || destination.height < 0 ||
        source.size.width < static_cast<std::uint32_t>(destination.width) ||
        source.size.height < static_cast<std::uint32_t>(destination.height))
        return false;
    if (destination.x < 0 || destination.y < 0 ||
        static_cast<std::uint64_t>(destination.x) + destination.width > image.size.width ||
        static_cast<std::uint64_t>(destination.y) + destination.height > image.size.height)
        return false;
    const std::size_t bpp = source.format == PixelFormat::R8 || source.format == PixelFormat::A8 ? 1 : 4;
    for (int y = 0; y < destination.height; ++y) {
        const auto *source_row = source.bytes.data() + static_cast<std::size_t>(y) * source.row_pitch;
        auto *target_row = image.pixels.data() + static_cast<std::size_t>(destination.y + y) * image.size.width +
                           static_cast<std::size_t>(destination.x);
        for (int x = 0; x < destination.width; ++x)
            target_row[x] = read_pixel(source_row + static_cast<std::size_t>(x) * bpp, source.format);
    }
    return true;
}

bool SoftwareRenderer::copy_out(const Image &image, PixelBufferView destination) const noexcept {
    if (!valid_view(destination) || destination.size.width != image.size.width ||
        destination.size.height != image.size.height)
        return false;
    const std::size_t bpp = destination.format == PixelFormat::R8 || destination.format == PixelFormat::A8 ? 1 : 4;
    for (std::uint32_t y = 0; y < image.size.height; ++y) {
        auto *target_row = destination.bytes.data() + static_cast<std::size_t>(y) * destination.row_pitch;
        const auto *source_row = image.pixels.data() + static_cast<std::size_t>(y) * image.size.width;
        for (std::uint32_t x = 0; x < image.size.width; ++x)
            write_pixel(target_row + static_cast<std::size_t>(x) * bpp, destination.format, source_row[x]);
    }
    return true;
}

SubmitResult SoftwareRenderer::draw(const DrawTrianglesCommand &command) {
    auto *destination = target(command.target.surface);
    if (!destination)
        return SubmitResult::Failed;

    const Image *source_image = nullptr;
    Image source_copy{};
    if (const auto *texture = std::get_if<TextureHandle>(&command.source)) {
        const auto found = textures_.find(texture->id);
        if (found == textures_.end())
            return SubmitResult::Failed;
        source_image = &found->second;
    } else if (const auto *surface = std::get_if<SurfaceHandle>(&command.source)) {
        const auto *found = target(*surface);
        if (!found)
            return SubmitResult::Failed;
        if (*surface == command.target.surface) {
            source_copy = *found;
            source_image = &source_copy;
        } else {
            source_image = found;
        }
    }

    const std::size_t element_count = command.indices.empty() ? command.vertices.size() : command.indices.size();
    if (element_count % 3 != 0 || !std::isfinite(command.opacity))
        return SubmitResult::Failed;

    const int width = static_cast<int>(destination->size.width);
    const int height = static_cast<int>(destination->size.height);
    int clip_left = 0, clip_top = 0, clip_right = width, clip_bottom = height;
    if (command.clip) {
        clip_left = std::max(clip_left, command.clip->x);
        clip_top = std::max(clip_top, command.clip->y);
        const auto right = static_cast<std::int64_t>(command.clip->x) + command.clip->width;
        const auto bottom = static_cast<std::int64_t>(command.clip->y) + command.clip->height;
        clip_right = static_cast<int>(std::clamp<std::int64_t>(right, 0, width));
        clip_bottom = static_cast<int>(std::clamp<std::int64_t>(bottom, 0, height));
    }
    if (clip_left >= clip_right || clip_top >= clip_bottom)
        return SubmitResult::Submitted;

    const SampledImage sampled{source_image ? source_image->size : Extent2D{1, 1},
                               source_image ? &source_image->pixels : nullptr};

    for (std::size_t element = 0; element < element_count; element += 3) {
        Vertex2D vertices[3]{};
        Vec2 points[3]{};
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const auto index = command.indices.empty() ? element + corner : command.indices[element + corner];
            if (index >= command.vertices.size())
                return SubmitResult::Failed;
            vertices[corner] = command.vertices[index];
            points[corner] = {command.transform.m00 * vertices[corner].x +
                                  command.transform.m01 * vertices[corner].y + command.transform.tx,
                              command.transform.m10 * vertices[corner].x +
                                  command.transform.m11 * vertices[corner].y + command.transform.ty};
            if (!std::isfinite(points[corner].x) || !std::isfinite(points[corner].y) ||
                !std::isfinite(vertices[corner].u) || !std::isfinite(vertices[corner].v))
                return SubmitResult::Failed;
        }

        double area = edge(points[0], points[1], points[2]);
        if (std::fabs(area) < 1e-12)
            continue;
        if (area < 0.0) {
            std::swap(points[1], points[2]);
            std::swap(vertices[1], vertices[2]);
            area = -area;
        }

        const auto clamp_floor = [](float value, int low, int high) {
            return static_cast<int>(std::clamp(std::floor(static_cast<double>(value)),
                                               static_cast<double>(low), static_cast<double>(high)));
        };
        const auto clamp_ceil = [](float value, int low, int high) {
            return static_cast<int>(std::clamp(std::ceil(static_cast<double>(value)),
                                               static_cast<double>(low), static_cast<double>(high)));
        };
        const int left = clamp_floor(std::min({points[0].x, points[1].x, points[2].x}), clip_left, clip_right);
        const int right = clamp_ceil(std::max({points[0].x, points[1].x, points[2].x}), clip_left, clip_right);
        const int top = clamp_floor(std::min({points[0].y, points[1].y, points[2].y}), clip_top, clip_bottom);
        const int bottom = clamp_ceil(std::max({points[0].y, points[1].y, points[2].y}), clip_top, clip_bottom);
        const bool tl0 = top_left(points[1], points[2]);
        const bool tl1 = top_left(points[2], points[0]);
        const bool tl2 = top_left(points[0], points[1]);

        for (int y = top; y < bottom; ++y) {
            for (int x = left; x < right; ++x) {
                const Vec2 p{static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F};
                const double e0 = edge(points[1], points[2], p);
                const double e1 = edge(points[2], points[0], p);
                const double e2 = edge(points[0], points[1], p);
                if (e0 < 0 || (e0 == 0 && !tl0) || e1 < 0 || (e1 == 0 && !tl1) || e2 < 0 ||
                    (e2 == 0 && !tl2))
                    continue;
                const double w0 = e0 / area, w1 = e1 / area, w2 = e2 / area;
                const double u = w0 * vertices[0].u + w1 * vertices[1].u + w2 * vertices[2].u;
                const double v = w0 * vertices[0].v + w1 * vertices[1].v + w2 * vertices[2].v;
                const auto texel = unpack(sampled.sample(u, v, command.sampler));
                ColorRGBA8 source{};
                const double vertex_channels[4] = {
                    w0 * vertices[0].color.r + w1 * vertices[1].color.r + w2 * vertices[2].color.r,
                    w0 * vertices[0].color.g + w1 * vertices[1].color.g + w2 * vertices[2].color.g,
                    w0 * vertices[0].color.b + w1 * vertices[1].color.b + w2 * vertices[2].color.b,
                    w0 * vertices[0].color.a + w1 * vertices[1].color.a + w2 * vertices[2].color.a};
                const std::uint8_t texture_channels[4] = {texel.r, texel.g, texel.b, texel.a};
                const std::uint8_t tint_channels[4] = {command.tint.r, command.tint.g, command.tint.b,
                                                       command.tint.a};
                std::uint8_t result_channels[4]{};
                for (unsigned c = 0; c < 4; ++c) {
                    double factor = std::clamp(vertex_channels[c] / 255.0, 0.0, 1.0) * tint_channels[c] / 255.0;
                    if (c == 3)
                        factor *= std::clamp(static_cast<double>(command.opacity), 0.0, 1.0);
                    result_channels[c] = static_cast<std::uint8_t>(std::clamp(
                        std::lround(texture_channels[c] * factor), 0L, 255L));
                }
                source = {result_channels[0], result_channels[1], result_channels[2], result_channels[3]};
                if (!alpha_pass(source.a, command.alpha_test))
                    continue;

                auto &pixel = destination->pixels[static_cast<std::size_t>(y) * width + x];
                const auto output = blend(source, unpack(pixel), command.blend);
                const std::uint32_t mask = (command.color_write.red ? 0x00ff0000U : 0U) |
                                           (command.color_write.green ? 0x0000ff00U : 0U) |
                                           (command.color_write.blue ? 0x000000ffU : 0U) |
                                           (command.color_write.alpha ? 0xff000000U : 0U);
                pixel = (pack(output) & mask) | (pixel & ~mask);
            }
        }
    }
    return SubmitResult::Submitted;
}

} // namespace regml::render
