#include "regml/render/canvas.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
namespace regml {
namespace {
std::uint32_t to_rgb(std::uint32_t bgr) {
    return ((bgr & 0xff) << 16) | (bgr & 0xff00) | ((bgr >> 16) & 0xff);
}
void finite(double value) {
    if (!std::isfinite(value))
        throw Error(ErrorKind::vm, "drawing coordinate must be finite");
}
// 5x7 glyphs, one row per byte, most significant bit first.
std::array<unsigned, 7> glyph(char c) {
    if (c >= 'a' && c <= 'z')
        c = static_cast<char>(c - 'a' + 'A');
    switch (c) {
    case 'A':
        return {14, 17, 17, 31, 17, 17, 17};
    case 'B':
        return {30, 17, 17, 30, 17, 17, 30};
    case 'C':
        return {14, 17, 16, 16, 16, 17, 14};
    case 'D':
        return {30, 17, 17, 17, 17, 17, 30};
    case 'E':
        return {31, 16, 16, 30, 16, 16, 31};
    case 'F':
        return {31, 16, 16, 30, 16, 16, 16};
    case 'G':
        return {14, 17, 16, 23, 17, 17, 15};
    case 'H':
        return {17, 17, 17, 31, 17, 17, 17};
    case 'I':
        return {14, 4, 4, 4, 4, 4, 14};
    case 'J':
        return {7, 2, 2, 2, 2, 18, 12};
    case 'K':
        return {17, 18, 20, 24, 20, 18, 17};
    case 'L':
        return {16, 16, 16, 16, 16, 16, 31};
    case 'M':
        return {17, 27, 21, 21, 17, 17, 17};
    case 'N':
        return {17, 25, 21, 19, 17, 17, 17};
    case 'O':
        return {14, 17, 17, 17, 17, 17, 14};
    case 'P':
        return {30, 17, 17, 30, 16, 16, 16};
    case 'Q':
        return {14, 17, 17, 17, 21, 18, 13};
    case 'R':
        return {30, 17, 17, 30, 20, 18, 17};
    case 'S':
        return {15, 16, 16, 14, 1, 1, 30};
    case 'T':
        return {31, 4, 4, 4, 4, 4, 4};
    case 'U':
        return {17, 17, 17, 17, 17, 17, 14};
    case 'V':
        return {17, 17, 17, 17, 17, 10, 4};
    case 'W':
        return {17, 17, 17, 21, 21, 21, 10};
    case 'X':
        return {17, 17, 10, 4, 10, 17, 17};
    case 'Y':
        return {17, 17, 10, 4, 4, 4, 4};
    case 'Z':
        return {31, 1, 2, 4, 8, 16, 31};
    case '0':
        return {14, 17, 19, 21, 25, 17, 14};
    case '1':
        return {4, 12, 4, 4, 4, 4, 14};
    case '2':
        return {14, 17, 1, 2, 4, 8, 31};
    case '3':
        return {30, 1, 1, 14, 1, 1, 30};
    case '4':
        return {2, 6, 10, 18, 31, 2, 2};
    case '5':
        return {31, 16, 16, 30, 1, 1, 30};
    case '6':
        return {14, 16, 16, 30, 17, 17, 14};
    case '7':
        return {31, 1, 2, 4, 8, 8, 8};
    case '8':
        return {14, 17, 17, 14, 17, 17, 14};
    case '9':
        return {14, 17, 17, 15, 1, 1, 14};
    case ':':
        return {0, 4, 4, 0, 4, 4, 0};
    case '.':
        return {0, 0, 0, 0, 0, 6, 6};
    case '/':
        return {1, 2, 2, 4, 8, 8, 16};
    case '-':
        return {0, 0, 0, 31, 0, 0, 0};
    case '>':
        return {16, 8, 4, 2, 4, 8, 16};
    case '+':
        return {0, 4, 4, 31, 4, 4, 0};
    case ' ':
        return {};
    default:
        return {14, 17, 1, 2, 4, 0, 4};
    }
}
} // namespace
Canvas::Canvas(int width, int height) : width_(width), height_(height) {
    if (width < 1 || height < 1 || width > 4096 || height > 4096)
        throw Error(ErrorKind::unsupported, "framebuffer dimensions must be between 1 and 4096");
    pixels_.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0xff000000);
}
std::uint32_t Canvas::pixel(int x, int y) const {
    if (x < 0 || y < 0 || x >= width_ || y >= height_)
        throw Error(ErrorKind::usage, "pixel is outside framebuffer");
    return pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x)];
}
void Canvas::clear(std::uint32_t bgr) {
    std::fill(pixels_.begin(), pixels_.end(), 0xff000000 | to_rgb(bgr));
}
void Canvas::clear_transparent() noexcept {
    std::fill(pixels_.begin(), pixels_.end(), 0);
}
void Canvas::rgba_pixel(int x, int y, std::uint32_t rgba, std::uint32_t bgr, double opacity) {
    finite(opacity);
    rgba_pixel_tinted(x, y, rgba, to_rgb(bgr), opacity);
}
void Canvas::rgba_pixel_tinted(int x, int y, std::uint32_t rgba, unsigned tint, double opacity) noexcept {
    if (x < 0 || y < 0 || x >= width_ || y >= height_)
        return;
    const unsigned source_alpha =
        opacity == 1 ? rgba >> 24
                     : static_cast<unsigned>(static_cast<double>(rgba >> 24) * std::clamp(opacity, 0.0, 1.0) + 0.5);
    if (source_alpha == 0)
        return;
    auto &dest = pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x)];
    if (source_alpha == 255 && tint == 0xffffff) {
        dest = rgba;
        return;
    }
    const unsigned destination_alpha = dest >> 24;
    if (destination_alpha == 255) {
        const unsigned inverse = 255 - source_alpha;
        const unsigned red = ((rgba >> 16) & 255) * ((tint >> 16) & 255) / 255;
        const unsigned green = ((rgba >> 8) & 255) * ((tint >> 8) & 255) / 255;
        const unsigned blue = (rgba & 255) * (tint & 255) / 255;
        const unsigned previous_red = (dest >> 16) & 255;
        const unsigned previous_green = (dest >> 8) & 255;
        const unsigned previous_blue = dest & 255;
        dest = 0xff000000 | ((red * source_alpha + previous_red * inverse + 127) / 255 << 16) |
               ((green * source_alpha + previous_green * inverse + 127) / 255 << 8) |
               ((blue * source_alpha + previous_blue * inverse + 127) / 255);
        return;
    }
    const unsigned output_alpha = source_alpha + (destination_alpha * (255 - source_alpha) + 127) / 255;
    unsigned result = output_alpha << 24;
    for (unsigned shift : {0U, 8U, 16U}) {
        const unsigned source = ((rgba >> shift) & 255) * ((tint >> shift) & 255) / 255;
        const unsigned previous = (dest >> shift) & 255;
        const unsigned premultiplied =
            source * source_alpha + (previous * destination_alpha * (255 - source_alpha) + 127) / 255;
        result |= ((premultiplied + output_alpha / 2) / output_alpha) << shift;
    }
    dest = result;
}
void Canvas::blit_rgba(std::span<const std::uint32_t> source, int source_width, int source_x, int source_y,
                       int destination_x, int destination_y, int width, int height, unsigned tint,
                       double opacity, std::int8_t source_alpha_mode) noexcept {
    const bool plain = tint == 0xffffff && opacity == 1;
    for (int row = 0; row < height; ++row) {
        const auto *src = source.data() + static_cast<std::size_t>(source_y + row) *
                                              static_cast<std::size_t>(source_width) +
                          static_cast<std::size_t>(source_x);
        auto *dst = pixels_.data() + static_cast<std::size_t>(destination_y + row) *
                                        static_cast<std::size_t>(width_) +
                    static_cast<std::size_t>(destination_x);
        if (plain && source_alpha_mode == 2) {
            std::copy_n(src, width, dst);
            continue;
        }
        if (plain && source_alpha_mode == 1) {
            int column = 0;
            while (column < width) {
                const int start = column;
                if ((src[column] >> 24) == 255) {
                    do {
                        ++column;
                    } while (column < width && (src[column] >> 24) == 255);
                    std::copy_n(src + start, column - start, dst + start);
                } else {
                    do {
                        ++column;
                    } while (column < width && (src[column] >> 24) == 0);
                }
            }
            continue;
        }
        for (int column = 0; column < width; ++column) {
            const auto pixel = src[column];
            const auto alpha = pixel >> 24;
            if (alpha == 0)
                continue;
            if (plain && alpha == 255) {
                dst[column] = pixel;
                continue;
            }
            if (plain && (dst[column] >> 24) == 255) {
                const unsigned inverse = 255 - alpha;
                const auto previous = dst[column];
                unsigned result = 0xff000000;
                for (unsigned shift : {0U, 8U, 16U})
                    result |= (((pixel >> shift & 255) * alpha + (previous >> shift & 255) * inverse + 127) /
                               255)
                              << shift;
                dst[column] = result;
                continue;
            }
            rgba_pixel_tinted(destination_x + column, destination_y + row, pixel, tint, opacity);
        }
    }
}
void Canvas::draw_surface(const Canvas &source, double x, double y, double sx, double sy, double angle,
                          std::uint32_t tint, double opacity) {
    finite(x);
    finite(y);
    finite(sx);
    finite(sy);
    finite(angle);
    finite(opacity);
    if (sx == 0 || sy == 0 || source.width_ == 0 || source.height_ == 0)
        return;
    // Overlapping self-blits need the generic path.
    if (this != &source && sx == 1 && sy == 1 && angle == 0 && tint == 0xffffff && opacity == 1) {
        // Cull before narrowing coordinates to int.
        if (x < -static_cast<double>(source.width_) - 1 || x > width_ + 1.0 ||
            y < -static_cast<double>(source.height_) - 1 || y > height_ + 1.0)
            return;
        const auto origin_x = static_cast<int>(std::lround(x));
        const auto origin_y = static_cast<int>(std::lround(y));
        const int source_x = std::max(0, -origin_x);
        const int source_y = std::max(0, -origin_y);
        const int dest_x = std::max(0, origin_x);
        const int dest_y = std::max(0, origin_y);
        const int copy_width = std::min(source.width_ - source_x, width_ - dest_x);
        const int copy_height = std::min(source.height_ - source_y, height_ - dest_y);
        if (copy_width <= 0 || copy_height <= 0)
            return;

        for (int row = 0; row < copy_height; ++row) {
            const auto *src =
                source.pixels_.data() + static_cast<std::size_t>(source_y + row) * source.width_ + source_x;
            auto *dst = pixels_.data() + static_cast<std::size_t>(dest_y + row) * width_ + dest_x;
            for (int column = 0; column < copy_width; ++column) {
                const auto rgba = src[column];
                const auto alpha = rgba >> 24;
                if (alpha == 255) {
                    dst[column] = rgba;
                } else if (alpha != 0) {
                    const auto previous = dst[column];
                    if ((previous >> 24) != 255) {
                        rgba_pixel_tinted(dest_x + column, dest_y + row, rgba, 0xffffff, 1);
                        continue;
                    }
                    const auto inverse_alpha = 255 - alpha;
                    unsigned result = 0xff000000;
                    for (unsigned shift : {0U, 8U, 16U}) {
                        const auto source_channel = (rgba >> shift) & 255;
                        const auto dest_channel = (previous >> shift) & 255;
                        result |= ((source_channel * alpha + dest_channel * inverse_alpha + 127) / 255) << shift;
                    }
                    dst[column] = result;
                }
            }
        }
        return;
    }
    const double radians = angle * 3.14159265358979323846 / 180.0;
    const double cs = std::cos(radians), sn = std::sin(radians);
    const unsigned rgb = to_rgb(tint);
    for (int py = 0; py < source.height_; ++py) {
        for (int px = 0; px < source.width_; ++px) {
            const auto rgba = source.pixels_[static_cast<std::size_t>(py) * source.width_ + px];
            if ((rgba >> 24) == 0)
                continue;
            const double u = px * sx, v = py * sy;
            const int dx = static_cast<int>(std::lround(x + u * cs + v * sn));
            const int dy = static_cast<int>(std::lround(y - u * sn + v * cs));
            rgba_pixel_tinted(dx, dy, rgba, rgb, opacity);
        }
    }
}
void Canvas::color(std::uint32_t bgr) noexcept {
    rgb_ = to_rgb(bgr);
}
void Canvas::alpha(double value) {
    finite(value);
    alpha_ = static_cast<unsigned>(std::lround(std::clamp(value, 0.0, 1.0) * 255));
}
void Canvas::font(int id) {
    if (id != 0 && id != 1)
        throw Error(ErrorKind::unsupported, "fixture font ID must be 0 or 1");
    font_scale_ = id + 1;
}
void Canvas::plot(int x, int y) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_)
        return;
    auto &dest = pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x)];
    if (alpha_ == 255) {
        dest = 0xff000000 | rgb_;
        return;
    }
    std::uint32_t result = 0xff000000;
    for (unsigned shift : {0U, 8U, 16U}) {
        const auto source = (rgb_ >> shift) & 255;
        const auto previous = (dest >> shift) & 255;
        result |= ((source * alpha_ + previous * (255 - alpha_) + 127) / 255) << shift;
    }
    dest = result;
}
void Canvas::rectangle(double x1, double y1, double x2, double y2, bool outline) {
    finite(x1);
    finite(y1);
    finite(x2);
    finite(y2);
    const auto left = std::floor(std::min(x1, x2)), right = std::ceil(std::max(x1, x2));
    const auto top = std::floor(std::min(y1, y2)), bottom = std::ceil(std::max(y1, y2));
    const int l = static_cast<int>(std::clamp(left, 0.0, static_cast<double>(width_)));
    const int r = static_cast<int>(std::clamp(right, 0.0, static_cast<double>(width_)));
    const int t = static_cast<int>(std::clamp(top, 0.0, static_cast<double>(height_)));
    const int b = static_cast<int>(std::clamp(bottom, 0.0, static_cast<double>(height_)));
    if (l >= r || t >= b)
        return;
    const auto span = [&](int y, int first, int last) {
        auto *row = pixels_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width_);
        if (alpha_ == 255) {
            std::fill(row + first, row + last, 0xff000000 | rgb_);
            return;
        }
        const unsigned inverse = 255 - alpha_;
        const unsigned red = (rgb_ >> 16 & 255) * alpha_,
                       green = (rgb_ >> 8 & 255) * alpha_, blue = (rgb_ & 255) * alpha_;
        for (int x = first; x < last; ++x) {
            const auto previous = row[x];
            row[x] = 0xff000000 | (((red + (previous >> 16 & 255) * inverse + 127) / 255) << 16) |
                     (((green + (previous >> 8 & 255) * inverse + 127) / 255) << 8) |
                     ((blue + (previous & 255) * inverse + 127) / 255);
        }
    };
    for (int y = t; y < b; ++y) {
        if (!outline || y == top || y == bottom - 1) {
            span(y, l, r);
        } else {
            if (l == left)
                span(y, l, l + 1);
            if (r - 1 == right - 1 && r - 1 != left)
                span(y, r - 1, r);
        }
    }
}
void Canvas::circle(double x, double y, double radius, bool outline) {
    finite(x);
    finite(y);
    finite(radius);
    if (radius < 0 || radius > 8192 || std::abs(x) > 1e9 || std::abs(y) > 1e9)
        throw Error(ErrorKind::unsupported, "circle is outside supported coordinate/radius limits");
    const int left = static_cast<int>(std::clamp(std::floor(x - radius), 0.0, static_cast<double>(width_)));
    const int right = static_cast<int>(std::clamp(std::ceil(x + radius + 1), 0.0, static_cast<double>(width_)));
    const int top = static_cast<int>(std::clamp(std::floor(y - radius), 0.0, static_cast<double>(height_)));
    const int bottom = static_cast<int>(std::clamp(std::ceil(y + radius + 1), 0.0, static_cast<double>(height_)));
    const auto outer = radius * radius;
    const auto inner = std::max(0.0, radius - 1) * std::max(0.0, radius - 1);
    for (int py = top; py < bottom; ++py)
        for (int px = left; px < right; ++px) {
            const auto distance = (px - x) * (px - x) + (py - y) * (py - y);
            if (distance <= outer && (!outline || distance >= inner))
                plot(px, py);
        }
}
void Canvas::line(double x1, double y1, double x2, double y2, double width) {
    finite(x1);
    finite(y1);
    finite(x2);
    finite(y2);
    finite(width);
    if (width <= 0 || width > 2048 || std::max({std::abs(x1), std::abs(y1), std::abs(x2), std::abs(y2)}) > 1e9)
        throw Error(ErrorKind::unsupported, "line is outside supported limits");
    const double dx = x2 - x1, dy = y2 - y1;
    const auto steps = static_cast<std::size_t>(std::ceil(std::max(std::abs(dx), std::abs(dy))));
    if (steps > 1'000'000)
        throw Error(ErrorKind::unsupported, "line exceeds rasterization limit");
    const double radius = (width - 1) / 2;
    for (std::size_t i = 0; i <= steps; ++i) {
        const double t = steps ? static_cast<double>(i) / steps : 0;
        const double x = x1 + dx * t, y = y1 + dy * t;
        if (radius < 0.5)
            plot(static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)));
        else
            circle(x, y, radius, false);
    }
}
void Canvas::triangle(double x1, double y1, double x2, double y2, double x3, double y3, bool outline) {
    finite(x1);
    finite(y1);
    finite(x2);
    finite(y2);
    finite(x3);
    finite(y3);
    if (outline) {
        line(x1, y1, x2, y2);
        line(x2, y2, x3, y3);
        line(x3, y3, x1, y1);
        return;
    }
    const double area = (x2 - x1) * (y3 - y1) - (y2 - y1) * (x3 - x1);
    if (std::abs(area) < 1e-12)
        return;
    const int left = static_cast<int>(std::clamp(std::floor(std::min({x1, x2, x3})), 0.0, static_cast<double>(width_)));
    const int right = static_cast<int>(std::clamp(std::ceil(std::max({x1, x2, x3})), 0.0, static_cast<double>(width_)));
    const int top = static_cast<int>(std::clamp(std::floor(std::min({y1, y2, y3})), 0.0, static_cast<double>(height_)));
    const int bottom =
        static_cast<int>(std::clamp(std::ceil(std::max({y1, y2, y3})), 0.0, static_cast<double>(height_)));
    for (int y = top; y < bottom; ++y)
        for (int x = left; x < right; ++x) {
            const double px = x + 0.5, py = y + 0.5;
            const double a = (x2 - px) * (y3 - py) - (y2 - py) * (x3 - px);
            const double b = (x3 - px) * (y1 - py) - (y3 - py) * (x1 - px);
            const double c = (x1 - px) * (y2 - py) - (y1 - py) * (x2 - px);
            if ((a >= 0 && b >= 0 && c >= 0) || (a <= 0 && b <= 0 && c <= 0))
                plot(x, y);
        }
}
void Canvas::text(double x, double y, std::string_view value) {
    finite(x);
    finite(y);
    if (std::abs(x) > 1e9 || std::abs(y) > 1e9 || value.size() > 4096)
        throw Error(ErrorKind::unsupported, "text exceeds fixture drawing limits");
    const auto start = x;
    for (char c : value) {
        if (c == '\n') {
            x = start;
            y += 9 * font_scale_;
            continue;
        }
        const auto bitmap = glyph(c);
        for (int row = 0; row < 7; ++row)
            for (int column = 0; column < 5; ++column) {
                if ((bitmap[static_cast<std::size_t>(row)] & (1U << static_cast<unsigned>(4 - column))) != 0) {
                    const auto px = std::floor(x) + column * font_scale_, py = std::floor(y) + row * font_scale_;
                    rectangle(px, py, px + font_scale_, py + font_scale_, false);
                }
            }
        x += 6 * font_scale_;
    }
}
std::uint64_t Canvas::hash() const noexcept {
    std::uint64_t value = 14695981039346656037ULL;
    for (auto pixel : pixels_)
        for (unsigned shift : {0U, 8U, 16U, 24U}) {
            value ^= (pixel >> shift) & 255;
            value *= 1099511628211ULL;
        }
    return value;
}
void Canvas::save_bmp(const std::filesystem::path &path) const {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
        throw Error(ErrorKind::io, "cannot open screenshot: " + path.string());
    const auto stride = (static_cast<std::uint32_t>(width_) * 3 + 3) & ~3U;
    const auto bytes = stride * static_cast<std::uint32_t>(height_);
    auto write32 = [&](std::uint32_t v) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            file.put(static_cast<char>((v >> shift) & 255));
    };
    auto write16 = [&](unsigned v) {
        file.put(static_cast<char>(v & 255));
        file.put(static_cast<char>((v >> 8) & 255));
    };
    file.put('B');
    file.put('M');
    write32(54 + bytes);
    write32(0);
    write32(54);
    write32(40);
    write32(static_cast<std::uint32_t>(width_));
    write32(static_cast<std::uint32_t>(height_));
    write16(1);
    write16(24);
    write32(0);
    write32(bytes);
    write32(2835);
    write32(2835);
    write32(0);
    write32(0);
    for (int y = height_ - 1; y >= 0; --y) {
        for (int x = 0; x < width_; ++x) {
            const auto p = pixel(x, y);
            file.put(static_cast<char>(p & 255));
            file.put(static_cast<char>((p >> 8) & 255));
            file.put(static_cast<char>((p >> 16) & 255));
        }
        for (auto padding = static_cast<unsigned>(width_) * 3; padding < stride; ++padding)
            file.put(0);
    }
    file.close();
    if (!file)
        throw Error(ErrorKind::io, "screenshot write failed");
}
} // namespace regml
