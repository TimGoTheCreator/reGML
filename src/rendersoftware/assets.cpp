#include "assets.hpp"
#include "platform/wii/gpu.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <png.h>
namespace regml {
namespace {
std::size_t frame_index(double frame, std::size_t count) {
    if (frame >= 0 && frame < static_cast<double>(count))
        return static_cast<std::size_t>(frame);
    double wrapped = std::fmod(std::floor(frame), static_cast<double>(count));
    if (wrapped < 0)
        wrapped += static_cast<double>(count);
    return static_cast<std::size_t>(wrapped);
}
} // namespace
GameAssets::GameAssets(const Package &package) : package_(package) {
    std::unordered_map<std::string, std::vector<std::uint8_t>> chunk_data;
    auto data_for = [&](const Chunk &c) -> const std::vector<std::uint8_t> & {
        const auto key = std::string(c.tag.data(), c.tag.size());
        auto [it, inserted] = chunk_data.try_emplace(key);
        if (inserted) {
            auto size = c.size;
            if (c.name() == "TXTR") {
                const auto prefix = package.read_bytes(c.data_offset, 4);
                const auto count = BinaryView(prefix, c.data_offset).u32(0);
                if (count > (c.size - 4) / 4)
                    throw Error(ErrorKind::malformed, "texture table range", c.data_offset);
                const auto table = package.read_bytes(c.data_offset, 4 + static_cast<std::size_t>(count) * 4);
                const BinaryView view(table, c.data_offset);
                size = table.size();
                for (std::size_t i = 0; i < count; ++i) {
                    const auto at = view.u32(4 + i * 4);
                    if (at < c.data_offset || at - c.data_offset > c.size || c.size - (at - c.data_offset) < 8)
                        throw Error(ErrorKind::malformed, "texture record range", at);
                    size = std::max(size, at - c.data_offset + 8);
                }
            }
            it->second = package.read_bytes(c.data_offset, size);
        }
        return it->second;
    };
    auto view_for = [&](const Chunk &c) { return BinaryView(data_for(c), c.data_offset); };
    auto list = [&](std::size_t at, const Chunk &c) {
        if (at < c.data_offset || at > c.data_offset + c.size || c.data_offset + c.size - at < 4)
            throw Error(ErrorKind::malformed, "asset list outside chunk", at);
        const auto data = view_for(c);
        const auto local = at - c.data_offset;
        const auto count = static_cast<std::size_t>(data.u32(local));
        if (count > 1'000'000 || count > (c.data_offset + c.size - at - 4) / 4)
            throw Error(ErrorKind::malformed, "asset list count", at);
        std::vector<std::size_t> out;
        for (std::size_t i = 0; i < count; ++i) {
            const auto p = data.u32(local + 4 + i * 4);
            if (p && (p < c.data_offset || p >= c.data_offset + c.size))
                throw Error(ErrorKind::malformed, "asset record outside chunk", p);
            out.push_back(p);
        }
        return out;
    };
    auto rec = [&](std::size_t at, const Chunk &c, std::size_t bytes) {
        if (at < c.data_offset || at > c.data_offset + c.size || bytes > c.data_offset + c.size - at)
            throw Error(ErrorKind::malformed, "asset record range", at);
        return view_for(c).slice(at - c.data_offset, bytes);
    };
    const auto *tx = package.find_chunk("TXTR");
    const auto *tp = package.find_chunk("TPAG");
    const auto *sp = package.find_chunk("SPRT");
    const auto *fo = package.find_chunk("FONT");
    if (!tx || !tp || !sp || !fo)
        throw Error(ErrorKind::unsupported, "graphics resources missing");
    for (auto p : list(tx->data_offset, *tx)) {
        const auto r = rec(p, *tx, 8);
        const auto image = r.u32(4);
        if (image < tx->data_offset || image >= tx->data_offset + tx->size)
            throw Error(ErrorKind::malformed, "PNG outside TXTR", p + 4);
        pages_.push_back({0, 0, image, {}});
    }
    for (auto p : list(tp->data_offset, *tp)) {
        const auto r = rec(p, *tp, 22);
        Region region{r.u16(0), r.u16(2), r.u16(4), r.u16(6), r.u16(8), r.u16(10), r.u16(12), r.u16(14), r.i16(20)};
        if (region.page < 0 || static_cast<std::size_t>(region.page) >= pages_.size())
            throw Error(ErrorKind::malformed, "invalid texture page ID", p + 20);
        regions_.emplace(p, region);
    }
    for (auto p : list(sp->data_offset, *sp)) {
        const auto r = rec(p, *sp, 60);
        Sprite sprite;
        sprite.name = std::string(package.string_pointer(r.u32(0)));
        sprite.width = r.i32(4);
        sprite.height = r.i32(8);
        sprite.ox = r.i32(48);
        sprite.oy = r.i32(52);
        const auto count = r.u32(56);
        sprite.left = r.i32(12);
        sprite.right = r.i32(16);
        sprite.bottom = r.i32(20);
        sprite.top = r.i32(24);
        if (count > 10000)
            throw Error(ErrorKind::unsupported, "special sprite profile is unsupported", p + 56);
        const auto frames = rec(p + 60, *sp, static_cast<std::size_t>(count) * 4);
        for (std::size_t i = 0; i < count; ++i) {
            const auto region = frames.u32(i * 4);
            if (region && !regions_.contains(region))
                throw Error(ErrorKind::malformed, "sprite TPAG reference invalid", p + 60 + i * 4);
            sprite.frames.push_back(region);
        }
        sprite.precise = r.u32(44) != 0;
        const auto mask_at = p + 60 + static_cast<std::size_t>(count) * 4;
        const auto mask_count = rec(mask_at, *sp, 4).u32(0);
        if (sprite.width < 0 || sprite.height < 0 || mask_count > 10000)
            throw Error(ErrorKind::malformed, "invalid sprite collision mask dimensions", mask_at);
        const auto mask_size =
            ((static_cast<std::size_t>(sprite.width) + 7) / 8) * static_cast<std::size_t>(sprite.height);
        if (mask_size > 64ULL * 1024 * 1024 || (mask_size && mask_count > sp->size / mask_size))
            throw Error(ErrorKind::malformed, "sprite collision mask size exceeds chunk", mask_at);
        const auto masks = rec(mask_at + 4, *sp, mask_size * mask_count).bytes();
        for (std::size_t i = 0; i < mask_count; ++i)
            sprite.masks.emplace_back(masks.begin() + static_cast<std::ptrdiff_t>(i * mask_size),
                                      masks.begin() + static_cast<std::ptrdiff_t>((i + 1) * mask_size));
        sprites_.push_back(std::move(sprite));
    }
    if (const auto *bg = package.find_chunk("BGND"))
        for (auto p : list(bg->data_offset, *bg)) {
            const auto r = rec(p, *bg, 20);
            const auto ptr = r.u32(16);
            if (ptr && !regions_.contains(ptr))
                throw Error(ErrorKind::malformed, "background TPAG reference invalid", p + 16);
            backgrounds_.push_back(ptr);
        }
    for (auto p : list(fo->data_offset, *fo)) {
        const auto r = rec(p, *fo, 44);
        Font font;
        font.size = r.i32(8);
        font.region = r.u32(28);
        if (!regions_.contains(font.region))
            throw Error(ErrorKind::malformed, "font TPAG reference invalid", p + 28);
        for (auto q : list(p + 40, *fo)) {
            const auto g = rec(q, *fo, 16);
            const auto character = g.u16(0);
            if (character < font.glyphs.size()) {
                font.glyphs[character] = {g.u16(2), g.u16(4), g.u16(6), g.u16(8), g.i16(10), g.i16(12)};
                font.has_glyph[character] = true;
            }
        }
        fonts_.push_back(std::move(font));
    }
    sprite_regions_.reserve(sprites_.size());
    for (const auto &sprite : sprites_) {
        auto &frame_regions = sprite_regions_.emplace_back();
        frame_regions.reserve(sprite.frames.size());
        for (const auto pointer : sprite.frames)
            frame_regions.push_back(pointer ? &regions_.at(pointer) : nullptr);
    }
    background_regions_.reserve(backgrounds_.size());
    for (const auto pointer : backgrounds_)
        background_regions_.push_back(pointer ? &regions_.at(pointer) : nullptr);
    for (auto &font : fonts_)
        font.atlas = &regions_.at(font.region);
}
const GameAssets::Sprite &GameAssets::sprite(int id) const {
    if (id < 0 || static_cast<std::size_t>(id) >= sprites_.size())
        throw Error(ErrorKind::vm, "sprite ID out of range");
    return sprites_[static_cast<std::size_t>(id)];
}
bool GameAssets::mask_pixel(int id, double frame, int x, int y) const {
    const auto &s = sprite(id);
    if (x < s.left || x > s.right || y < s.top || y > s.bottom || x < 0 || y < 0 || x >= s.width || y >= s.height)
        return false;
    if (!s.precise || s.masks.empty())
        return true;
    const auto index = static_cast<std::size_t>(std::fmod(
        std::fmod(std::floor(frame), static_cast<double>(s.masks.size())) + static_cast<double>(s.masks.size()),
        static_cast<double>(s.masks.size())));
    const auto stride = (static_cast<std::size_t>(s.width) + 7) / 8;
    const auto byte = s.masks[index][static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) / 8];
    return (byte & (0x80U >> (x & 7))) != 0;
}
std::pair<int, int> GameAssets::background_size(int id) const {
    if (id < 0 || static_cast<std::size_t>(id) >= backgrounds_.size())
        throw Error(ErrorKind::vm, "background ID out of range");
    const auto ptr = backgrounds_[static_cast<std::size_t>(id)];
    if (!ptr)
        return {0, 0};
    const auto *region = background_regions_[static_cast<std::size_t>(id)];
    if (!region)
        return {0, 0};
    const auto &r = *region;
    return {r.tw, r.th};
}
void GameAssets::load_page(int id) {
    auto &page = pages_.at(static_cast<std::size_t>(id));
    if (!page.pixels.empty()) {
        page.last_used = ++page_cache_clock_;
        return;
    }
    const auto *chunk = package_.find_chunk("TXTR");
    const auto png_header = package_.read_bytes(page.png, 8);
    if (png_header[0] != 0x89 || png_header[1] != 'P' || png_header[2] != 'N' || png_header[3] != 'G' ||
        png_header[4] != 0x0d || png_header[5] != 0x0a || png_header[6] != 0x1a || png_header[7] != 0x0a)
        throw Error(ErrorKind::malformed, "texture PNG header invalid", page.png);
    std::size_t png_size = 8;
    bool found_end = false;
    const auto chunk_end = chunk->data_offset + chunk->size;
    while (page.png <= chunk_end && png_size <= chunk_end - page.png && chunk_end - page.png - png_size >= 12) {
        const auto record = package_.read_bytes(page.png + png_size, 8);
        const auto length = (static_cast<std::size_t>(record[0]) << 24) | (static_cast<std::size_t>(record[1]) << 16) |
                            (static_cast<std::size_t>(record[2]) << 8) | record[3];
        if (length > chunk_end - page.png - png_size - 12)
            throw Error(ErrorKind::malformed, "texture PNG chunk exceeds TXTR", page.png + png_size);
        found_end = record[4] == 'I' && record[5] == 'E' && record[6] == 'N' && record[7] == 'D';
        png_size += 12 + length;
        if (found_end)
            break;
    }
    if (!found_end)
        throw Error(ErrorKind::malformed, "texture PNG has no IEND chunk", page.png);
    const auto bytes = package_.read_bytes(page.png, png_size);
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, bytes.data(), bytes.size()))
        throw Error(ErrorKind::malformed, "texture PNG header invalid", page.png);
    struct Guard {
        png_image &image;
        ~Guard() { png_image_free(&image); }
    } guard{image};
    if (image.width > 8192 || image.height > 8192 || !image.width || !image.height)
        throw Error(ErrorKind::unsupported, "texture dimensions exceed limit", page.png);
    if constexpr (std::endian::native == std::endian::little)
        image.format = PNG_FORMAT_BGRA;
    else
        image.format = PNG_FORMAT_ARGB;
    const auto width = static_cast<std::size_t>(image.width), height = static_cast<std::size_t>(image.height);
    const auto decoded_size = width * height * sizeof(std::uint32_t);
    if (decoded_size > 16ULL * 1024 * 1024)
        throw Error(ErrorKind::unsupported, "texture page exceeds Wii decode limit", page.png);
    std::vector<std::uint32_t> pixels;
    pixels.swap(page_buffer_);
    if constexpr (page_cache_limit_ != 0) {
        while (page_cache_bytes_ + decoded_size > page_cache_limit_) {
            auto oldest = pages_.end();
            for (auto candidate = pages_.begin(); candidate != pages_.end(); ++candidate) {
                if (&*candidate == &page || candidate->pixels.empty())
                    continue;
                if (oldest == pages_.end() || candidate->last_used < oldest->last_used)
                    oldest = candidate;
            }
            if (oldest == pages_.end())
                break;
            page_cache_bytes_ -= oldest->pixels.capacity() * sizeof(std::uint32_t);
            if (oldest->pixels.capacity() >= width * height && pixels.capacity() < width * height) {
                pixels.swap(oldest->pixels);
                pixels.clear();
            } else {
                std::vector<std::uint32_t>().swap(oldest->pixels);
            }
        }
    }
    pixels.resize(width * height);
    if (!png_image_finish_read(&image, nullptr, reinterpret_cast<unsigned char *>(pixels.data()), 0, nullptr))
        throw Error(ErrorKind::malformed, "texture PNG decode failed", page.png);
    page.width = static_cast<int>(width);
    page.height = static_cast<int>(height);
    page.pixels = std::move(pixels);
    page.last_used = ++page_cache_clock_;
    page_cache_bytes_ += page.pixels.capacity() * sizeof(std::uint32_t);
}
void GameAssets::prefetch(int id) {
    (void)sprite(id);
    if constexpr (page_cache_limit_ != 0)
        return;
    for (const auto *region : sprite_regions_[static_cast<std::size_t>(id)])
        if (region)
            load_page(region->page);
}
void GameAssets::draw_background(Canvas &canvas, int id, double x, double y) {
    if (id < 0 || static_cast<std::size_t>(id) >= backgrounds_.size())
        throw Error(ErrorKind::vm, "background ID out of range");
    const auto ptr = backgrounds_[static_cast<std::size_t>(id)];
    if (!ptr)
        return;
    const auto *region = background_regions_[static_cast<std::size_t>(id)];
    if (!region)
        return;
    const auto &r = *region;
    blit(canvas, r, x + r.tx, y + r.ty, 1, 1, 0, 0xffffff, 1);
}
void GameAssets::draw_background_stretched(Canvas &canvas, int id, double x, double y, double width, double height) {
    if (id < 0 || static_cast<std::size_t>(id) >= backgrounds_.size())
        throw Error(ErrorKind::vm, "background ID out of range");
    const auto ptr = backgrounds_[static_cast<std::size_t>(id)];
    if (!ptr)
        return;
    const auto *region = background_regions_[static_cast<std::size_t>(id)];
    if (!region)
        return;
    const auto &r = *region;
    if (r.tw > 0 && r.th > 0)
        blit(canvas, r, x, y, width / r.tw, height / r.th, 0, 0xffffff, 1);
}
void GameAssets::draw_background_part_ext(Canvas &canvas, int id, double left, double top, double width, double height,
                                          double x, double y, double sx, double sy, unsigned color, double alpha) {
    if (id < 0 || static_cast<std::size_t>(id) >= backgrounds_.size())
        throw Error(ErrorKind::vm, "background ID out of range");
    const auto ptr = backgrounds_[static_cast<std::size_t>(id)];
    if (!ptr)
        return;
    const auto *region = background_regions_[static_cast<std::size_t>(id)];
    if (!region)
        return;
    const auto &r = *region;
    const double l = std::max(0.0, left), t = std::max(0.0, top);
    const double rr = std::min(static_cast<double>(r.tw), left + width),
                 bb = std::min(static_cast<double>(r.th), top + height);
    if (rr <= l || bb <= t)
        return;
    const int ix = static_cast<int>(std::floor(l)), iy = static_cast<int>(std::floor(t));
    const int ir = static_cast<int>(std::ceil(rr)), ib = static_cast<int>(std::ceil(bb));
    Region crop{r.x + static_cast<int>(ix * r.w / r.tw),
                r.y + static_cast<int>(iy * r.h / r.th),
                static_cast<int>((ir - ix) * r.w / r.tw),
                static_cast<int>((ib - iy) * r.h / r.th),
                0,
                0,
                ir - ix,
                ib - iy,
                r.page};
    crop.alpha_mode = r.alpha_mode;
    blit(canvas, crop, x + ix * sx, y + iy * sy, sx, sy, 0, color, alpha);
}
void GameAssets::draw_tile(Canvas &canvas, int id, int source_x, int source_y, int width, int height, double x,
                           double y, double sx, double sy, unsigned color) {
    if (id < 0 || static_cast<std::size_t>(id) >= backgrounds_.size())
        throw Error(ErrorKind::vm, "tile background ID out of range");
    const auto ptr = backgrounds_[static_cast<std::size_t>(id)];
    if (!ptr)
        return;
    const auto *region = background_regions_[static_cast<std::size_t>(id)];
    if (!region)
        return;
    const auto &r = *region;
    const int left = std::max(source_x, r.tx), top = std::max(source_y, r.ty),
              right = std::min(source_x + width, r.tx + r.tw), bottom = std::min(source_y + height, r.ty + r.th);
    if (right <= left || bottom <= top)
        return;
    Region cut{r.x + (left - r.tx), r.y + (top - r.ty), right - left, bottom - top, 0, 0,
               right - left,        bottom - top,       r.page};
    cut.alpha_mode = r.alpha_mode;
    blit(canvas, cut, x + (left - source_x) * sx, y + (top - source_y) * sy, sx, sy, 0, color & 0xffffff,
         static_cast<double>(color >> 24) / 255);
}
void GameAssets::view(double x, double y, double sx, double sy, double px, double py) {
    if (!std::isfinite(x + y + sx + sy + px + py))
        throw Error(ErrorKind::vm, "nonfinite view transform");
    view_x_ = x;
    view_y_ = y;
    view_sx_ = sx;
    view_sy_ = sy;
    port_x_ = px;
    port_y_ = py;
}
void GameAssets::rectangle(Canvas &canvas, double x1, double y1, double x2, double y2, bool outline) const {
    canvas.rectangle((x1 - view_x_) * view_sx_ + port_x_, (y1 - view_y_) * view_sy_ + port_y_,
                     (x2 + 1 - view_x_) * view_sx_ + port_x_, (y2 + 1 - view_y_) * view_sy_ + port_y_, outline);
}
const GameAssets::Page &GameAssets::region_pixels(const Region &r, int &source_x, int &source_y) {
    source_x = r.x;
    source_y = r.y;
    constexpr std::size_t budget = 1ULL * 1024 * 1024;
    const auto bytes = static_cast<std::size_t>(r.w) * static_cast<std::size_t>(r.h) * sizeof(std::uint32_t);
    const std::array<int, 5> key{r.page, r.x, r.y, r.w, r.h};
    if constexpr (page_cache_limit_ != 0) {
        if (const auto found = region_cache_.find(key); found != region_cache_.end()) {
            found->second.last_used = ++page_cache_clock_;
            source_x = source_y = 0;
            return found->second;
        }
    }
    load_page(r.page);
    const auto &source = pages_.at(static_cast<std::size_t>(r.page));
    if (r.x < 0 || r.y < 0 || r.w < 0 || r.h < 0 || r.x + r.w > source.width || r.y + r.h > source.height)
        throw Error(ErrorKind::malformed, "texture region outside page");
    if constexpr (page_cache_limit_ != 0) {
        if (bytes <= budget) {
            while (region_cache_bytes_ + bytes > budget && !region_cache_.empty()) {
                const auto oldest = std::min_element(
                    region_cache_.begin(), region_cache_.end(),
                    [](const auto &left, const auto &right) { return left.second.last_used < right.second.last_used; });
                region_cache_bytes_ -= oldest->second.pixels.size() * sizeof(std::uint32_t);
                region_cache_.erase(oldest);
            }
            Page cached;
            cached.width = r.w;
            cached.height = r.h;
            cached.last_used = ++page_cache_clock_;
            cached.pixels.resize(bytes / sizeof(std::uint32_t));
            for (int row = 0; row < r.h; ++row)
                std::copy_n(source.pixels.begin() + static_cast<std::ptrdiff_t>(r.y + row) * source.width + r.x, r.w,
                            cached.pixels.begin() + static_cast<std::ptrdiff_t>(row) * r.w);
            auto [it, inserted] = region_cache_.emplace(key, std::move(cached));
            region_cache_bytes_ += bytes;
            source_x = source_y = 0;
            return it->second;
        }
    }
    return source;
}
void GameAssets::blit(Canvas &canvas, const Region &r, double x, double y, double sx, double sy, double angle,
                      unsigned color, double alpha) {
    x = (x - view_x_) * view_sx_ + port_x_;
    y = (y - view_y_) * view_sy_ + port_y_;
    x *= canvas.render_scale();
    y *= canvas.render_scale();
    sx *= view_sx_ * canvas.render_scale();
    sy *= view_sy_ * canvas.render_scale();
    if (!std::isfinite(x + y + sx + sy + angle + alpha))
        throw Error(ErrorKind::vm, "nonfinite sprite transform");
    if (sx == 0 || sy == 0 || r.w == 0 || r.h == 0 || alpha <= 0)
        return;
    int source_x = 0, source_y = 0;
    const unsigned tint = ((color & 0xff) << 16) | (color & 0xff00) | ((color >> 16) & 0xff);
#if REGML_WII
    if (canvas.gpu_target()) {
        const double w = r.tw * sx, h = r.th * sy;
        if (angle == 0) {
            const double right = x + w, bottom = y + h;
            if (std::max(x, right) <= 0 || std::max(y, bottom) <= 0 || std::min(x, right) >= canvas.width() ||
                std::min(y, bottom) >= canvas.height())
                return;
        } else {
            const double radians = angle * 3.141592653589793 / 180;
            const double cs = std::cos(radians), sn = std::sin(radians);
            const double xx[] = {x, x + w * cs, x + h * sn, x + w * cs + h * sn};
            const double yy[] = {y, y - w * sn, y + h * cs, y - w * sn + h * cs};
            if (*std::max_element(xx, xx + 4) <= 0 || *std::max_element(yy, yy + 4) <= 0 ||
                *std::min_element(xx, xx + 4) >= canvas.width() || *std::min_element(yy, yy + 4) >= canvas.height())
                return;
        }
        std::uint64_t key = 1469598103934665603ULL;
        for (int value : {r.page, r.x, r.y, r.w, r.h})
            key = (key ^ static_cast<unsigned>(value)) * 1099511628211ULL;
        key = key ? key : 1;
        if (wii_gpu_cached_image(r.w, r.h, x, y, r.tw * sx, r.th * sy, angle, tint, alpha, key))
            return;
        const auto &page = region_pixels(r, source_x, source_y);
        wii_gpu_image(page.pixels, page.width, source_x, source_y, r.w, r.h, x, y, r.tw * sx, r.th * sy, angle, tint,
                      alpha, key);
        return;
    }
#endif
    const auto &page = region_pixels(r, source_x, source_y);
#if REGML_240P
    if (canvas.render_scale() < 1 && (std::abs(sx) * r.tw < r.w || std::abs(sy) * r.th < r.h)) {
        const double rad = angle * 3.141592653589793 / 180, cs = std::cos(rad), sn = std::sin(rad);
        const double w = r.tw * sx, h = r.th * sy;
        const double xx[] = {x, x + w * cs, x + h * sn, x + w * cs + h * sn};
        const double yy[] = {y, y - w * sn, y + h * cs, y - w * sn + h * cs};
        const int left =
            static_cast<int>(std::clamp(std::floor(*std::min_element(xx, xx + 4)), 0.0, double(canvas.width())));
        const int right =
            static_cast<int>(std::clamp(std::ceil(*std::max_element(xx, xx + 4)), 0.0, double(canvas.width())));
        const int top =
            static_cast<int>(std::clamp(std::floor(*std::min_element(yy, yy + 4)), 0.0, double(canvas.height())));
        const int bottom =
            static_cast<int>(std::clamp(std::ceil(*std::max_element(yy, yy + 4)), 0.0, double(canvas.height())));
        for (int dy = top; dy < bottom; ++dy)
            for (int dx = left; dx < right; ++dx) {
                const double px = dx + 0.5 - x, py = dy + 0.5 - y;
                const double tx = (px * cs - py * sn) / sx, ty = (px * sn + py * cs) / sy;

                const double u = tx * r.w / r.tw - 0.5, v = ty * r.h / r.th - 0.5;
                const int ix = static_cast<int>(std::floor(u)), iy = static_cast<int>(std::floor(v));
                const double fx = u - ix, fy = v - iy;
                std::uint32_t rgba = 0;
                unsigned best = 0;
                for (int yy = 0; yy < 2; ++yy)
                    for (int xx = 0; xx < 2; ++xx) {
                        const int ux = ix + xx, uy = iy + yy;
                        if (ux < 0 || uy < 0 || ux >= r.w || uy >= r.h)
                            continue;
                        const double weight = (xx ? fx : 1 - fx) * (yy ? fy : 1 - fy);
                        if (weight <= 0)
                            continue;
                        const auto p =
                            page.pixels[static_cast<std::size_t>(source_y + uy) * page.width + source_x + ux];
                        // Keep a real source color; prefer visible strokes over empty or dark samples.
                        const unsigned score = (p >> 24) * (1 + (p >> 16 & 255) + (p >> 8 & 255) + (p & 255));
                        if (score > best) {
                            best = score;
                            rgba = p;
                        }
                    }
                if (!best)
                    continue;
                canvas.rgba_pixel_tinted(dx, dy, rgba, tint, alpha);
            }
        return;
    }
#endif
    std::int8_t source_alpha_mode = 0;
    if (tint == 0xffffff && alpha == 1 && angle == 0 && r.tw == r.w && r.th == r.h) {
        if (r.alpha_mode < 0) {
            r.alpha_mode = 2;
            for (int row = 0; row < r.h && r.alpha_mode != 0; ++row) {
                const auto at = static_cast<std::size_t>(source_y + row) * static_cast<std::size_t>(page.width) +
                                static_cast<std::size_t>(source_x);
                for (int column = 0; column < r.w; ++column) {
                    const auto pixel_alpha = page.pixels[at + static_cast<std::size_t>(column)] >> 24;
                    if (pixel_alpha == 0)
                        r.alpha_mode = 1;
                    else if (pixel_alpha != 255) {
                        r.alpha_mode = 0;
                        break;
                    }
                }
            }
        }
        source_alpha_mode = r.alpha_mode;
    }
    if (angle == 0 && sx == 1 && sy == 1 && r.tw == r.w && r.th == r.h) {
        const auto left = std::clamp(std::ceil(x - 0.5), 0.0, static_cast<double>(canvas.width()));
        const auto top = std::clamp(std::ceil(y - 0.5), 0.0, static_cast<double>(canvas.height()));
        const auto right = std::clamp(std::ceil(x + r.tw - 0.5), 0.0, static_cast<double>(canvas.width()));
        const auto bottom = std::clamp(std::ceil(y + r.th - 0.5), 0.0, static_cast<double>(canvas.height()));
        const int dx = static_cast<int>(left), dy = static_cast<int>(top);
        const int width = static_cast<int>(right) - dx, height = static_cast<int>(bottom) - dy;
        if (width <= 0 || height <= 0)
            return;
        const int crop_x = static_cast<int>(std::floor(dx + 0.5 - x));
        const int crop_y = static_cast<int>(std::floor(dy + 0.5 - y));
        canvas.blit_rgba(page.pixels, page.width, source_x + crop_x, source_y + crop_y, dx, dy, width, height, tint,
                         alpha, source_alpha_mode);
        return;
    }
    if (angle == 0) {
        const double w = r.tw * sx, h = r.th * sy;
        // A reflected axis includes the origin edge and excludes the far edge.
        const double x_begin = sx > 0 ? std::ceil(x - 0.5) : std::floor(x + w - 0.5) + 1;
        const double x_end = sx > 0 ? std::ceil(x + w - 0.5) : std::floor(x - 0.5) + 1;
        const double y_begin = sy > 0 ? std::ceil(y - 0.5) : std::floor(y + h - 0.5) + 1;
        const double y_end = sy > 0 ? std::ceil(y + h - 0.5) : std::floor(y - 0.5) + 1;
        const int left = static_cast<int>(std::clamp(x_begin, 0.0, static_cast<double>(canvas.width())));
        const int top = static_cast<int>(std::clamp(y_begin, 0.0, static_cast<double>(canvas.height())));
        const int right = static_cast<int>(std::clamp(x_end, 0.0, static_cast<double>(canvas.width())));
        const int bottom = static_cast<int>(std::clamp(y_end, 0.0, static_cast<double>(canvas.height())));
        if (left >= right || top >= bottom)
            return;
        auto destination = canvas.pixels();
        const auto sample_width = static_cast<std::size_t>(right - left);
        const auto sample_height = static_cast<std::size_t>(bottom - top);
        if (scaled_x_.size() < sample_width)
            scaled_x_.resize(sample_width);
        if (scaled_y_.size() < sample_height)
            scaled_y_.resize(sample_height);
        for (int dx = left; dx < right; ++dx) {
            const double tx = (dx + 0.5 - x) / sx;
            scaled_x_[static_cast<std::size_t>(dx - left)] = source_x + static_cast<int>(tx * r.w / r.tw);
        }
        for (int dy = top; dy < bottom; ++dy) {
            const double ty = (dy + 0.5 - y) / sy;
            scaled_y_[static_cast<std::size_t>(dy - top)] = source_y + static_cast<int>(ty * r.h / r.th);
        }
        const bool spans = std::abs(sx) >= 2 && !(tint == 0xffffff && alpha == 1 && source_alpha_mode != 0);
        if (spans) {
            scaled_runs_.clear();
            for (int dx = left; dx < right;) {
                const int ux = scaled_x_[static_cast<std::size_t>(dx - left)];
                int end = dx + 1;
                while (end < right && scaled_x_[static_cast<std::size_t>(end - left)] == ux)
                    ++end;
                scaled_runs_.push_back({dx - left, end - dx, ux});
                dx = end;
            }
        }
        int previous_source_y = -1;
        const double opacity = std::clamp(alpha, 0.0, 1.0);
        for (int dy = top; dy < bottom; ++dy) {
            const int uy = scaled_y_[static_cast<std::size_t>(dy - top)];
            const auto row = static_cast<std::size_t>(uy) * static_cast<std::size_t>(page.width);
            if (spans) {
                if (uy != previous_source_y) {
                    for (auto &run : scaled_runs_) {
                        const auto pixel = page.pixels[row + static_cast<std::size_t>(run.source_x)];
                        const unsigned a =
                            alpha == 1 ? pixel >> 24
                                       : static_cast<unsigned>(static_cast<double>(pixel >> 24) * opacity + 0.5);
                        const unsigned red = ((pixel >> 16) & 255) * ((tint >> 16) & 255) / 255;
                        const unsigned green = ((pixel >> 8) & 255) * ((tint >> 8) & 255) / 255;
                        const unsigned blue = (pixel & 255) * (tint & 255) / 255;
                        run.rgba = (a << 24) | (red << 16) | (green << 8) | blue;
                    }
                    previous_source_y = uy;
                }
                for (const auto &run : scaled_runs_)
                    canvas.rgba_span(left + run.offset, dy, run.count, run.rgba, 0xffffff, 1);
                continue;
            }
            for (int dx = left; dx < right; ++dx) {
                const int ux = scaled_x_[static_cast<std::size_t>(dx - left)];
                const auto pixel = page.pixels[row + static_cast<std::size_t>(ux)];
                const auto destination_index = static_cast<std::size_t>(dy) * static_cast<std::size_t>(canvas.width()) +
                                               static_cast<std::size_t>(dx);
                if (source_alpha_mode == 2) {
                    destination[destination_index] = pixel;
                } else if (source_alpha_mode == 1) {
                    if ((pixel >> 24) == 255)
                        destination[destination_index] = pixel;
                } else if (tint == 0xffffff && alpha == 1) {
                    const auto source_alpha = pixel >> 24;
                    if (source_alpha == 255) {
                        destination[destination_index] = pixel;
                    } else if (source_alpha != 0 && (destination[destination_index] >> 24) == 255) {
                        const auto previous = destination[destination_index];
                        const auto inverse = 255 - source_alpha;
                        unsigned result = 0xff000000;
                        for (unsigned shift : {0U, 8U, 16U})
                            result |=
                                (((pixel >> shift & 255) * source_alpha + (previous >> shift & 255) * inverse + 127) /
                                 255)
                                << shift;
                        destination[destination_index] = result;
                    } else if (source_alpha != 0) {
                        canvas.rgba_pixel_tinted(dx, dy, pixel, tint, alpha);
                    }
                } else {
                    canvas.rgba_pixel_tinted(dx, dy, pixel, tint, alpha);
                }
            }
        }
        return;
    }
    const auto radians = angle * 3.141592653589793 / 180;
    const auto c = std::cos(radians), s = std::sin(radians);
    const double w = r.tw * sx, h = r.th * sy;
    const double xx[] = {x, x + w * c, x + h * s, x + w * c + h * s};
    const double yy[] = {y, y - w * s, y + h * c, y - w * s + h * c};
    const auto left = static_cast<int>(
        std::clamp(std::floor(*std::min_element(xx, xx + 4)), 0.0, static_cast<double>(canvas.width())));
    const auto right = static_cast<int>(
        std::clamp(std::ceil(*std::max_element(xx, xx + 4)), 0.0, static_cast<double>(canvas.width())));
    const auto top = static_cast<int>(
        std::clamp(std::floor(*std::min_element(yy, yy + 4)), 0.0, static_cast<double>(canvas.height())));
    const auto bottom = static_cast<int>(
        std::clamp(std::ceil(*std::max_element(yy, yy + 4)), 0.0, static_cast<double>(canvas.height())));
    for (int dy = top; dy < bottom; ++dy)
        for (int dx = left; dx < right; ++dx) {
            const auto px = dx + 0.5 - x, py = dy + 0.5 - y;
            const auto tx = (px * c - py * s) / sx, ty = (px * s + py * c) / sy;
            if (tx < 0 || ty < 0 || tx >= r.tw || ty >= r.th)
                continue;
            const int ux = source_x + static_cast<int>(tx * r.w / r.tw),
                      uy = source_y + static_cast<int>(ty * r.h / r.th);
            canvas.rgba_pixel_tinted(dx, dy,
                                     page.pixels[static_cast<std::size_t>(uy) * static_cast<std::size_t>(page.width) +
                                                 static_cast<std::size_t>(ux)],
                                     tint, alpha);
        }
}
void GameAssets::draw_sprite(Canvas &canvas, int id, double frame, double x, double y, double sx, double sy,
                             double angle, unsigned color, double alpha) {
    const auto &spr = sprite(id);
    if (spr.frames.empty())
        return;
    if (!std::isfinite(frame))
        throw Error(ErrorKind::vm, "nonfinite sprite frame");
    const auto index = frame_index(frame, spr.frames.size());
    const auto *region = sprite_regions_[static_cast<std::size_t>(id)][index];
    if (!region)
        return;
    const auto &r = *region;
    const double ox = (r.tx - spr.ox) * sx, oy = (r.ty - spr.oy) * sy;
    if (angle == 0) {
        blit(canvas, r, x + ox, y + oy, sx, sy, 0, color, alpha);
        return;
    }
    const double rad = angle * 3.141592653589793 / 180;
    blit(canvas, r, x + ox * std::cos(rad) + oy * std::sin(rad), y - ox * std::sin(rad) + oy * std::cos(rad), sx, sy,
         angle, color, alpha);
}
void GameAssets::draw_sprite_part(Canvas &canvas, int id, double frame, double left, double top, double width,
                                  double height, double x, double y, double sx, double sy, unsigned color,
                                  double alpha) {
    const auto &spr = sprite(id);
    if (spr.frames.empty())
        return;
    if (!std::isfinite(frame + left + top + width + height + x + y + sx + sy + alpha))
        throw Error(ErrorKind::vm, "nonfinite sprite-part argument");
    if (width <= 0 || height <= 0 || sx == 0 || sy == 0)
        return;
    const auto index = frame_index(frame, spr.frames.size());
    const auto *region = sprite_regions_[static_cast<std::size_t>(id)][index];
    if (!region)
        return;
    const auto &r = *region;
    const double l = std::max({left, 0.0, static_cast<double>(r.tx)}),
                 t = std::max({top, 0.0, static_cast<double>(r.ty)});
    const double rr = std::min({left + width, static_cast<double>(spr.width), static_cast<double>(r.tx + r.tw)}),
                 bb = std::min({top + height, static_cast<double>(spr.height), static_cast<double>(r.ty + r.th)});
    if (rr <= l || bb <= t)
        return;
    const int ix = static_cast<int>(std::floor(l)), iy = static_cast<int>(std::floor(t)),
              ir = static_cast<int>(std::ceil(rr)), ib = static_cast<int>(std::ceil(bb));
    Region crop{r.x + static_cast<int>((ix - r.tx) * r.w / r.tw),
                r.y + static_cast<int>((iy - r.ty) * r.h / r.th),
                static_cast<int>((ir - ix) * r.w / r.tw),
                static_cast<int>((ib - iy) * r.h / r.th),
                0,
                0,
                ir - ix,
                ib - iy,
                r.page};
    crop.alpha_mode = r.alpha_mode;
    // draw_sprite_part positions the cropped region by its own top-left; sprite origins do not apply.
    blit(canvas, crop, x + (ix - left) * sx, y + (iy - top) * sy, sx, sy, 0, color, alpha);
}
double GameAssets::text_width(int font, std::string_view text) const {
    if (font < 0 || static_cast<std::size_t>(font) >= fonts_.size())
        return static_cast<double>(text.size() * 6);
    const auto &f = fonts_[static_cast<std::size_t>(font)];
    double line = 0, max = 0;
    for (char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        if (c == '\n' || c == '#') {
            max = std::max(max, line);
            line = 0;
        } else if (f.has_glyph[c])
            line += f.glyphs[c].shift;
    }
    return std::max(line, max);
}
double GameAssets::text_height(int font) const {
    return font < 0 || static_cast<std::size_t>(font) >= fonts_.size() ? 8
                                                                       : fonts_[static_cast<std::size_t>(font)].size;
}
void GameAssets::draw_text(Canvas &canvas, int font, double x, double y, std::string_view text, unsigned color,
                           double alpha, int halign, int valign, double sx, double sy, double angle) {
    if (font < 0 || static_cast<std::size_t>(font) >= fonts_.size()) {
        canvas.color(color);
        canvas.alpha(alpha);
        canvas.text(x, y, text);
        return;
    }
    const auto &f = fonts_[static_cast<std::size_t>(font)];
    const auto &atlas = *f.atlas;
    const double start = -text_width(font, text) * halign / 2.0;
    double lx = start, ly = -text_height(font) * valign / 2.0;
    const auto rad = angle * 3.141592653589793 / 180;
    const auto c = angle == 0 ? 1.0 : std::cos(rad), s = angle == 0 ? 0.0 : std::sin(rad);
    for (char raw : text) {
        const auto ch = static_cast<unsigned char>(raw);
        if (ch == '\n' || ch == '#') {
            lx = start;
            ly += text_height(font) + 2;
            continue;
        }
        if (!f.has_glyph[ch])
            continue;
        const auto &g = f.glyphs[ch];
        Region r{atlas.x + g.x, atlas.y + g.y, g.w, g.h, 0, 0, g.w, g.h, atlas.page};
        r.alpha_mode = g.alpha_mode;
        const auto ox = (lx + g.offset) * sx, oy = ly * sy;
        blit(canvas, r, x + ox * c + oy * s, y - ox * s + oy * c, sx, sy, angle, color, alpha);
        g.alpha_mode = r.alpha_mode;
        lx += g.shift;
    }
}
} // namespace regml
