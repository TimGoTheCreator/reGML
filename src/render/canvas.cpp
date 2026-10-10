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
    if (!std::isfinite(value)) throw Error(ErrorKind::vm, "drawing coordinate must be finite");
}
// Original 5x7 bitmap glyphs: one row per byte, most significant of five bits first.
std::array<unsigned, 7> glyph(char c) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    switch (c) {
    case 'A': return {14,17,17,31,17,17,17}; case 'B': return {30,17,17,30,17,17,30};
    case 'C': return {14,17,16,16,16,17,14}; case 'D': return {30,17,17,17,17,17,30};
    case 'E': return {31,16,16,30,16,16,31}; case 'F': return {31,16,16,30,16,16,16};
    case 'G': return {14,17,16,23,17,17,15}; case 'H': return {17,17,17,31,17,17,17};
    case 'I': return {14,4,4,4,4,4,14}; case 'J': return {7,2,2,2,2,18,12};
    case 'K': return {17,18,20,24,20,18,17}; case 'L': return {16,16,16,16,16,16,31};
    case 'M': return {17,27,21,21,17,17,17}; case 'N': return {17,25,21,19,17,17,17};
    case 'O': return {14,17,17,17,17,17,14}; case 'P': return {30,17,17,30,16,16,16};
    case 'Q': return {14,17,17,17,21,18,13}; case 'R': return {30,17,17,30,20,18,17};
    case 'S': return {15,16,16,14,1,1,30}; case 'T': return {31,4,4,4,4,4,4};
    case 'U': return {17,17,17,17,17,17,14}; case 'V': return {17,17,17,17,17,10,4};
    case 'W': return {17,17,17,21,21,21,10}; case 'X': return {17,17,10,4,10,17,17};
    case 'Y': return {17,17,10,4,4,4,4}; case 'Z': return {31,1,2,4,8,16,31};
    case '0': return {14,17,19,21,25,17,14}; case '1': return {4,12,4,4,4,4,14};
    case '2': return {14,17,1,2,4,8,31}; case '3': return {30,1,1,14,1,1,30};
    case '4': return {2,6,10,18,31,2,2}; case '5': return {31,16,16,30,1,1,30};
    case '6': return {14,16,16,30,17,17,14}; case '7': return {31,1,2,4,8,8,8};
    case '8': return {14,17,17,14,17,17,14}; case '9': return {14,17,17,15,1,1,14};
    case ':': return {0,4,4,0,4,4,0}; case '.': return {0,0,0,0,0,6,6};
    case '/': return {1,2,2,4,8,8,16}; case '-': return {0,0,0,31,0,0,0};
    case '>': return {16,8,4,2,4,8,16}; case '+': return {0,4,4,31,4,4,0};
    case ' ': return {}; default: return {14,17,1,2,4,0,4};
    }
}
}
Canvas::Canvas(int width, int height) : width_(width), height_(height) {
    if (width < 1 || height < 1 || width > 4096 || height > 4096)
        throw Error(ErrorKind::unsupported, "framebuffer dimensions must be between 1 and 4096");
    pixels_.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0xff000000);
}
std::uint32_t Canvas::pixel(int x, int y) const {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) throw Error(ErrorKind::usage, "pixel is outside framebuffer");
    return pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x)];
}
void Canvas::clear(std::uint32_t bgr) { std::fill(pixels_.begin(), pixels_.end(), 0xff000000 | to_rgb(bgr)); }
void Canvas::rgba_pixel(int x,int y,std::uint32_t rgba,std::uint32_t bgr,double opacity) {
    finite(opacity);
    rgba_pixel_tinted(x,y,rgba,to_rgb(bgr),opacity);
}
void Canvas::rgba_pixel_tinted(int x,int y,std::uint32_t rgba,unsigned tint,double opacity) noexcept {
    if(x<0 || y<0 || x>=width_ || y>=height_)return;
    const unsigned source_alpha=static_cast<unsigned>(std::lround(static_cast<double>(rgba>>24)*std::clamp(opacity,0.0,1.0)));
    if(source_alpha==0)return;
    auto& dest=pixels_[static_cast<std::size_t>(y)*static_cast<std::size_t>(width_)+static_cast<std::size_t>(x)];
    unsigned result=0xff000000;
    for(unsigned shift:{0U,8U,16U}){
        const unsigned source=((rgba>>shift)&255)*((tint>>shift)&255)/255;
        const unsigned previous=(dest>>shift)&255;
        result|=((source*source_alpha+previous*(255-source_alpha)+127)/255)<<shift;
    }
    dest=result;
}
void Canvas::color(std::uint32_t bgr) noexcept { rgb_ = to_rgb(bgr); }
void Canvas::alpha(double value) {
    finite(value); alpha_ = static_cast<unsigned>(std::lround(std::clamp(value, 0.0, 1.0) * 255));
}
void Canvas::font(int id) {
    if (id != 0 && id != 1) throw Error(ErrorKind::unsupported, "fixture font ID must be 0 or 1");
    font_scale_ = id + 1;
}
void Canvas::plot(int x, int y) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return;
    auto& dest = pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x)];
    std::uint32_t result = 0xff000000;
    for (unsigned shift : {0U, 8U, 16U}) {
        const auto source = (rgb_ >> shift) & 255;
        const auto previous = (dest >> shift) & 255;
        result |= ((source * alpha_ + previous * (255 - alpha_) + 127) / 255) << shift;
    }
    dest = result;
}
void Canvas::rectangle(double x1, double y1, double x2, double y2, bool outline) {
    finite(x1); finite(y1); finite(x2); finite(y2);
    // Half-open pixel coverage is the fixture profile, not a claim about all GM versions.
    const auto left = std::floor(std::min(x1, x2)), right = std::ceil(std::max(x1, x2));
    const auto top = std::floor(std::min(y1, y2)), bottom = std::ceil(std::max(y1, y2));
    const int l = static_cast<int>(std::clamp(left, 0.0, static_cast<double>(width_)));
    const int r = static_cast<int>(std::clamp(right, 0.0, static_cast<double>(width_)));
    const int t = static_cast<int>(std::clamp(top, 0.0, static_cast<double>(height_)));
    const int b = static_cast<int>(std::clamp(bottom, 0.0, static_cast<double>(height_)));
    for (int y = t; y < b; ++y) for (int x = l; x < r; ++x) {
        if (!outline || x == left || x == right - 1 || y == top || y == bottom - 1) plot(x, y);
    }
}
void Canvas::circle(double x, double y, double radius, bool outline) {
    finite(x); finite(y); finite(radius);
    if (radius < 0 || radius > 8192 || std::abs(x) > 1e9 || std::abs(y) > 1e9)
        throw Error(ErrorKind::unsupported, "circle is outside supported coordinate/radius limits");
    const int left = static_cast<int>(std::clamp(std::floor(x - radius), 0.0, static_cast<double>(width_)));
    const int right = static_cast<int>(std::clamp(std::ceil(x + radius + 1), 0.0, static_cast<double>(width_)));
    const int top = static_cast<int>(std::clamp(std::floor(y - radius), 0.0, static_cast<double>(height_)));
    const int bottom = static_cast<int>(std::clamp(std::ceil(y + radius + 1), 0.0, static_cast<double>(height_)));
    const auto outer = radius * radius; const auto inner = std::max(0.0, radius - 1) * std::max(0.0, radius - 1);
    for (int py = top; py < bottom; ++py) for (int px = left; px < right; ++px) {
        const auto distance = (px - x) * (px - x) + (py - y) * (py - y);
        if (distance <= outer && (!outline || distance >= inner)) plot(px, py);
    }
}
void Canvas::text(double x, double y, std::string_view value) {
    finite(x); finite(y);
    if (std::abs(x) > 1e9 || std::abs(y) > 1e9 || value.size() > 4096)
        throw Error(ErrorKind::unsupported, "text exceeds fixture drawing limits");
    const auto start = x;
    for (char c : value) {
        if (c == '\n') { x = start; y += 9 * font_scale_; continue; }
        const auto bitmap = glyph(c);
        for (int row = 0; row < 7; ++row) for (int column = 0; column < 5; ++column) {
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
    for (auto pixel : pixels_) for (unsigned shift : {0U,8U,16U,24U}) {
        value ^= (pixel >> shift) & 255; value *= 1099511628211ULL;
    }
    return value;
}
void Canvas::save_bmp(const std::filesystem::path& path) const {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) throw Error(ErrorKind::io, "cannot open screenshot: " + path.string());
    const auto stride = (static_cast<std::uint32_t>(width_) * 3 + 3) & ~3U;
    const auto bytes = stride * static_cast<std::uint32_t>(height_);
    auto write32 = [&](std::uint32_t v) { for (unsigned shift = 0; shift < 32; shift += 8) file.put(static_cast<char>((v >> shift) & 255)); };
    auto write16 = [&](unsigned v) { file.put(static_cast<char>(v & 255)); file.put(static_cast<char>((v >> 8) & 255)); };
    file.put('B'); file.put('M'); write32(54 + bytes); write32(0); write32(54);
    write32(40); write32(static_cast<std::uint32_t>(width_)); write32(static_cast<std::uint32_t>(height_));
    write16(1); write16(24); write32(0); write32(bytes); write32(2835); write32(2835); write32(0); write32(0);
    for (int y = height_ - 1; y >= 0; --y) {
        for (int x = 0; x < width_; ++x) {
            const auto p = pixel(x, y);
            file.put(static_cast<char>(p & 255)); file.put(static_cast<char>((p >> 8) & 255)); file.put(static_cast<char>((p >> 16) & 255));
        }
        for (auto padding = static_cast<unsigned>(width_) * 3; padding < stride; ++padding) file.put(0);
    }
    file.close(); if (!file) throw Error(ErrorKind::io, "screenshot write failed");
}
} // namespace regml
