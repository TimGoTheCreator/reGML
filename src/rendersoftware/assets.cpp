#include "assets.hpp"
#include "../gmrt_project.hpp"
#include "platform/ps2/gpu.hpp"
#include "platform/wii/gpu.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <png.h>
#if REGML_HAS_BZIP2
#include <bzlib.h>
#endif
namespace regml {
namespace {
std::uint32_t next_asset_cache_generation = 0;
struct DecodedQoi {
    int width = 0, height = 0;
    std::vector<std::uint32_t> pixels;
};
std::uint16_t read_le16(const std::uint8_t *bytes) {
    return static_cast<std::uint16_t>(bytes[0] | (static_cast<unsigned>(bytes[1]) << 8));
}
std::uint32_t read_le32(const std::uint8_t *bytes) {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}
DecodedQoi decode_qoi(std::span<const std::uint8_t> bytes, bool compressed, std::size_t offset) {
    std::vector<std::uint8_t> decoded;
    if (compressed) {
        if (bytes.size() < 12 || std::memcmp(bytes.data(), "2zoq", 4) != 0)
            throw Error(ErrorKind::malformed, "invalid BZ2-QOI texture header", offset);
#if REGML_HAS_BZIP2
        const auto payload_size = static_cast<std::size_t>(read_le32(bytes.data() + 8));
        if (payload_size > 64ULL * 1024 * 1024 || payload_size > std::numeric_limits<unsigned int>::max() ||
            bytes.size() - 12 > std::numeric_limits<unsigned int>::max())
            throw Error(ErrorKind::unsupported, "BZ2-QOI texture exceeds decode limit", offset);
        decoded.resize(payload_size);
        unsigned int decoded_size = static_cast<unsigned int>(decoded.size());
        const auto status = BZ2_bzBuffToBuffDecompress(
            reinterpret_cast<char *>(decoded.data()), &decoded_size,
            reinterpret_cast<char *>(const_cast<std::uint8_t *>(bytes.data() + 12)),
            static_cast<unsigned int>(bytes.size() - 12), 0, 0);
        if (status != BZ_OK || decoded_size != decoded.size())
            throw Error(ErrorKind::malformed, "BZ2-QOI texture decompression failed", offset);
        bytes = decoded;
#else
        throw Error(ErrorKind::unsupported, "BZ2-QOI textures require BZip2 support", offset);
#endif
    }
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "fioq", 4) != 0)
        throw Error(ErrorKind::malformed, "invalid QOI texture header", offset);
    DecodedQoi result;
    result.width = read_le16(bytes.data() + 4);
    result.height = read_le16(bytes.data() + 6);
    const auto encoded_size = static_cast<std::size_t>(read_le32(bytes.data() + 8));
    if (!result.width || !result.height || result.width > 8192 || result.height > 8192 ||
        encoded_size > bytes.size() - 12)
        throw Error(ErrorKind::malformed, "invalid QOI texture dimensions or data size", offset);
    const auto pixel_count = static_cast<std::size_t>(result.width) * result.height;
    if (pixel_count > 4ULL * 1024 * 1024)
        throw Error(ErrorKind::unsupported, "QOI texture exceeds decode limit", offset);
    const auto encoded = bytes.subspan(12, encoded_size);
    std::array<std::array<std::uint8_t, 4>, 64> index{};
    std::array<std::uint8_t, 4> pixel{0, 0, 0, 255};
    std::size_t position = 0, output = 0;
    result.pixels.resize(pixel_count);
    auto require = [&](std::size_t count) {
        if (position > encoded.size() || count > encoded.size() - position)
            throw Error(ErrorKind::malformed, "truncated FIOQ pixel stream", offset + 12 + position);
    };
    auto hash_pixel = [&] {
        return static_cast<std::size_t>((pixel[0] ^ pixel[1] ^ pixel[2] ^ pixel[3]) & 0x3f);
    };
    auto emit = [&] {
        result.pixels[output++] = (static_cast<std::uint32_t>(pixel[3]) << 24) |
                                  (static_cast<std::uint32_t>(pixel[0]) << 16) |
                                  (static_cast<std::uint32_t>(pixel[1]) << 8) | pixel[2];
    };
    auto signed_bits = [](unsigned value, unsigned bits) {
        const unsigned sign = 1U << (bits - 1);
        return (value & sign) ? static_cast<int>(value) - static_cast<int>(1U << bits)
                              : static_cast<int>(value);
    };
    auto update_diff = [&](int dr, int dg, int db, int da = 0) {
        pixel[0] = static_cast<std::uint8_t>(static_cast<int>(pixel[0]) + dr);
        pixel[1] = static_cast<std::uint8_t>(static_cast<int>(pixel[1]) + dg);
        pixel[2] = static_cast<std::uint8_t>(static_cast<int>(pixel[2]) + db);
        pixel[3] = static_cast<std::uint8_t>(static_cast<int>(pixel[3]) + da);
        index[hash_pixel()] = pixel;
        emit();
    };
    while (output < pixel_count) {
        require(1);
        const auto op = encoded[position++];
        if ((op & 0xc0) == 0x00) {
            pixel = index[op & 0x3f];
            emit();
            continue;
        }
        if ((op & 0xc0) == 0x40) {
            std::size_t run = 0;
            if ((op & 0xe0) == 0x60) {
                require(1);
                run = (static_cast<std::size_t>(op & 0x1f) << 8) + encoded[position++] + 33;
            } else {
                run = static_cast<std::size_t>(op & 0x1f) + 1;
            }
            if (run > pixel_count - output)
                throw Error(ErrorKind::malformed, "FIOQ run exceeds image dimensions", offset + 12 + position);
            while (run--)
                emit();
            continue;
        }
        if ((op & 0xc0) == 0x80) {
            update_diff(signed_bits((op >> 4) & 3, 2), signed_bits((op >> 2) & 3, 2),
                        signed_bits(op & 3, 2));
            continue;
        }
        if ((op & 0xf0) == 0xc0 || (op & 0xf0) == 0xd0) {
            require(1);
            const auto second = encoded[position++];
            const int dr = (op & 0xf0) == 0xd0 ? static_cast<int>(op & 0x0f) - 16 : op & 0x0f;
            update_diff(dr, signed_bits(second >> 4, 4), signed_bits(second & 0x0f, 4));
            continue;
        }
        if ((op & 0xf0) == 0xe0) {
            require(2);
            const std::uint32_t packed = (static_cast<std::uint32_t>(op & 0x0f) << 16) |
                                         (static_cast<std::uint32_t>(encoded[position]) << 8) |
                                         encoded[position + 1];
            position += 2;
            update_diff(signed_bits((packed >> 15) & 0x1f, 5), signed_bits((packed >> 10) & 0x1f, 5),
                        signed_bits((packed >> 5) & 0x1f, 5), signed_bits(packed & 0x1f, 5));
            continue;
        }
        if ((op & 0xf0) == 0xf0) {
            for (int channel = 0; channel < 4; ++channel)
                if (op & (0x08 >> channel)) {
                    require(1);
                    pixel[static_cast<std::size_t>(channel)] = encoded[position++];
                }
            index[hash_pixel()] = pixel;
            emit();
            continue;
        }
        throw Error(ErrorKind::malformed, "unknown FIOQ operation", offset + 11 + position);
    }
    return result;
}
#if REGML_LOW_MEMORY
std::vector<std::uint32_t>
read_png_region(const Package &package, std::size_t offset, int x, int y, int w, int h) {
    struct Input {
        const Package *package;
        std::size_t position, end;
    } input{&package, offset, package.find_chunk("TXTR")->data_offset + package.find_chunk("TXTR")->size};
    auto *png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png)
        throw Error(ErrorKind::io, "PNG reader allocation failed");
    auto *info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        throw Error(ErrorKind::io, "PNG metadata allocation failed");
    }
    struct Guard {
        png_structp &png;
        png_infop &info;
        ~Guard() { png_destroy_read_struct(&png, &info, nullptr); }
    } guard{png, info};
    std::vector<std::uint32_t> pixels;
    std::vector<unsigned char> row;
    if (setjmp(png_jmpbuf(png)))
        throw Error(ErrorKind::malformed, "streamed texture PNG decode failed", offset);
    png_set_read_fn(png, &input, [](png_structp decoder, png_bytep output, png_size_t length) {
        auto &source = *static_cast<Input *>(png_get_io_ptr(decoder));
        bool failed = source.position > source.end || length > source.end - source.position;
        if (!failed) {
            try {
                source.package->read_into(source.position, {output, length});
                source.position += length;
            } catch (...) {
                failed = true;
            }
        }
        if (failed)
            png_error(decoder, "texture read exceeds package or failed");
    });
    png_read_info(png, info);
    const auto width = png_get_image_width(png, info), height = png_get_image_height(png, info);
    const int color = png_get_color_type(png, info), bits = png_get_bit_depth(png, info);
    if (!width || !height || width > 8192 || height > 8192 || x < 0 || y < 0 || w < 0 || h < 0 ||
        static_cast<std::size_t>(x) + w > width || static_cast<std::size_t>(y) + h > height)
        throw Error(ErrorKind::malformed, "texture region outside page", offset);
    if (bits == 16)
        png_set_strip_16(png);
    if (color == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);
    if (color == PNG_COLOR_TYPE_GRAY && bits < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    const bool transparent = png_get_valid(png, info, PNG_INFO_tRNS);
    if (transparent)
        png_set_tRNS_to_alpha(png);
    if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    if (!(color & PNG_COLOR_MASK_ALPHA) && !transparent)
        png_set_add_alpha(png, 255, PNG_FILLER_AFTER);
    if constexpr (std::endian::native == std::endian::little)
        png_set_bgr(png);
    else
        png_set_swap_alpha(png);
    const int passes = png_set_interlace_handling(png);
    png_read_update_info(png, info);
    row.resize(png_get_rowbytes(png, info));
    pixels.resize(static_cast<std::size_t>(w) * h);
    // unpack the atlas a row at a time and keep only the sprite we need
    for (int pass = 0; pass < passes; ++pass)
        for (png_uint_32 iy = 0; iy < height; ++iy) {
            const bool selected = iy >= static_cast<unsigned>(y) && iy - y < static_cast<unsigned>(h);
            auto *destination = selected ? pixels.data() + static_cast<std::size_t>(iy - y) * w : nullptr;
            if (passes > 1) {
                std::fill(row.begin(), row.end(), 0);
                if (selected)
                    std::copy_n(reinterpret_cast<const unsigned char *>(destination),
                                static_cast<std::size_t>(w) * 4,
                                row.data() + static_cast<std::size_t>(x) * 4);
            }
            png_read_row(png, row.data(), nullptr);
            if (selected)
                std::copy_n(row.data() + static_cast<std::size_t>(x) * 4, static_cast<std::size_t>(w) * 4,
                            reinterpret_cast<unsigned char *>(destination));
        }
    png_read_end(png, info);
    return pixels;
}
#endif
std::size_t frame_index(double frame, std::size_t count) {
    if (frame >= 0 && frame < static_cast<double>(count))
        return static_cast<std::size_t>(frame);
    double wrapped = std::fmod(std::floor(frame), static_cast<double>(count));
    if (wrapped < 0)
        wrapped += static_cast<double>(count);
    return static_cast<std::size_t>(wrapped);
}
} // namespace
GameAssets::GameAssets(const Package &package)
    : package_(package), cache_generation_(++next_asset_cache_generation) {
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
                const bool bytecode17 = package.general_info() &&
                                        package.general_info()->bytecode_version == 17;
                size = table.size();
                for (std::size_t i = 0; i < count; ++i) {
                    const auto at = view.u32(4 + i * 4);
                    const auto record_size = bytecode17 ? std::size_t{28} : std::size_t{8};
                    if (at < c.data_offset || at - c.data_offset > c.size ||
                        c.size - (at - c.data_offset) < record_size)
                        throw Error(ErrorKind::malformed, "texture record range", at);
                    size = std::max<std::size_t>(size, static_cast<std::size_t>(at - c.data_offset) + record_size);
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
    const bool bytecode17 = package.general_info() && package.general_info()->bytecode_version == 17;
    auto sprite_frames = [&](std::size_t p) {
        const auto marker = rec(p, *sp, 60).u32(56);
        if (marker != 0xffffffffU)
            return std::pair{p + 56, static_cast<std::size_t>(marker)};
        const auto header = rec(p, *sp, 68);
        const auto version = header.u32(60);
        const auto type = header.u32(64);
        if (!bytecode17 || type != 0 || version == 0)
            throw Error(ErrorKind::unsupported, "special sprite type is unsupported", p + 56);
        const auto list_at = p + 76 + (version >= 2 ? 4 : 0) + (version >= 3 ? 4 : 0);
        const auto count = rec(list_at, *sp, 4).u32(0);
        return std::pair{list_at, static_cast<std::size_t>(count)};
    };
    const auto texture_records = list(tx->data_offset, *tx);
    pages_.reserve(texture_records.size() + max_dynamic_sprites_);
    for (auto p : texture_records) {
        if (!bytecode17) {
            const auto r = rec(p, *tx, 8);
            const auto image = r.u32(4);
            if (image < tx->data_offset || image >= tx->data_offset + tx->size)
                throw Error(ErrorKind::malformed, "PNG outside TXTR", p + 4);
            pages_.push_back({0, 0, image});
            continue;
        }
        const auto r = rec(p, *tx, 28);
        std::size_t image = 0;
        std::size_t image_field = 0;
        TextureFormat format = TextureFormat::png;
        for (const std::size_t field : {24U, 12U, 8U, 4U}) {
            const auto pointer = r.u32(field);
            if (pointer < tx->data_offset || pointer > tx->data_offset + tx->size ||
                tx->data_offset + tx->size - pointer < 4)
                continue;
            const auto signature = package.read_bytes(pointer, 4);
            if (signature[0] == 0x89 && signature[1] == 'P' && signature[2] == 'N' && signature[3] == 'G')
                format = TextureFormat::png;
            else if (signature[0] == '2' && signature[1] == 'z' && signature[2] == 'o' && signature[3] == 'q')
                format = TextureFormat::bzip2_qoi;
            else if (signature[0] == 'f' && signature[1] == 'i' && signature[2] == 'o' && signature[3] == 'q')
                format = TextureFormat::qoi;
            else
                continue;
            image = pointer;
            image_field = field;
            break;
        }
        if (!image)
            throw Error(ErrorKind::unsupported, "external or unknown v17 texture format", p);
        const auto block_size = image_field == 24 || image_field == 12 ? r.u32(8) : 0;
        pages_.push_back({0, 0, image, block_size, format});
    }
    const auto texture_pages = list(tp->data_offset, *tp);
    regions_.reserve(texture_pages.size() + max_dynamic_sprites_);
    for (auto p : texture_pages) {
        const auto r = rec(p, *tp, 22);
        Region region{r.u16(0),  r.u16(2),  r.u16(4),  r.u16(6), r.u16(8),
                      r.u16(10), r.u16(12), r.u16(14), r.i16(20)};
        if (region.page < 0 || static_cast<std::size_t>(region.page) >= pages_.size())
            throw Error(ErrorKind::malformed, "invalid texture page ID", p + 20);
        regions_.push_back({static_cast<std::uint32_t>(p), region});
    }
    std::sort(regions_.begin(), regions_.end(),
              [](const RegionEntry &left, const RegionEntry &right) { return left.pointer < right.pointer; });
    for (std::size_t i = 1; i < regions_.size(); ++i)
        if (regions_[i - 1].pointer == regions_[i].pointer)
            throw Error(ErrorKind::malformed, "duplicate texture page pointer", regions_[i].pointer);
    const auto sprite_records = list(sp->data_offset, *sp);
    std::size_t total_sprite_frames = 0;
    for (const auto p : sprite_records) {
        const auto [list_at, count] = sprite_frames(p);
        (void)list_at;
        if (count > 10000 || count > std::numeric_limits<std::size_t>::max() - total_sprite_frames)
            throw Error(ErrorKind::unsupported, "sprite frame count exceeds limit", p + 56);
        total_sprite_frames += count;
    }
    sprite_frame_regions_.reserve(total_sprite_frames + max_dynamic_sprites_);
    std::vector<std::pair<std::size_t, std::size_t>> sprite_frame_ranges;
    sprite_frame_ranges.reserve(sprite_records.size());
    sprites_.reserve(sprite_records.size() + max_dynamic_sprites_);
    for (auto p : sprite_records) {
        const auto [list_at, count] = sprite_frames(p);
        const auto r = rec(p, *sp, 60);
        Sprite sprite;
        sprite.name = package.string_pointer(r.u32(0));
        sprite.width = r.i32(4);
        sprite.height = r.i32(8);
        sprite.ox = r.i32(48);
        sprite.oy = r.i32(52);
        sprite.left = r.i32(12);
        sprite.right = r.i32(16);
        sprite.bottom = r.i32(20);
        sprite.top = r.i32(24);
        if (count > 10000)
            throw Error(ErrorKind::unsupported, "sprite frame count exceeds limit", p + 56);
        const auto frames_at = list_at + 4;
        const auto frames = rec(frames_at, *sp, count * 4);
        for (std::size_t i = 0; i < count; ++i) {
            const auto region = frames.u32(i * 4);
            const auto index = region ? find_region_index(region) : std::numeric_limits<std::uint32_t>::max();
            if (region && index == std::numeric_limits<std::uint32_t>::max())
                throw Error(ErrorKind::malformed, "sprite TPAG reference invalid", frames_at + i * 4);
            sprite_frame_regions_.push_back(index);
        }
        sprite_frame_ranges.emplace_back(sprite_frame_regions_.size() - count, count);
        sprite.precise = r.u32(44) != 0;
        const auto mask_at = frames_at + count * 4;
        const auto mask_count = rec(mask_at, *sp, 4).u32(0);
        if (sprite.width < 0 || sprite.height < 0 || mask_count > 10000)
            throw Error(ErrorKind::malformed, "invalid sprite collision mask dimensions", mask_at);
        const auto mask_size =
            ((static_cast<std::size_t>(sprite.width) + 7) / 8) * static_cast<std::size_t>(sprite.height);
        if (mask_size > 64ULL * 1024 * 1024 || (mask_size && mask_count > sp->size / mask_size))
            throw Error(ErrorKind::malformed, "sprite collision mask size exceeds chunk", mask_at);
        const auto masks = rec(mask_at + 4, *sp, mask_size * mask_count).bytes();
        sprite.mask_offset = mask_data_.size();
        sprite.mask_count = mask_count;
        mask_data_.insert(mask_data_.end(), masks.begin(), masks.end());
        sprites_.push_back(std::move(sprite));
    }
    for (std::size_t i = 0; i < sprites_.size(); ++i) {
        const auto [offset, count] = sprite_frame_ranges[i];
        sprites_[i].frames = std::span<const std::uint32_t>(sprite_frame_regions_).subspan(offset, count);
    }
    if (const auto *bg = package.find_chunk("BGND"))
        for (auto p : list(bg->data_offset, *bg)) {
            const auto r = rec(p, *bg, 20);
            const auto ptr = r.u32(16);
            if (ptr && !find_region(ptr))
                throw Error(ErrorKind::malformed, "background TPAG reference invalid", p + 16);
            backgrounds_.push_back(ptr);
            Tileset tileset;
            if (package.general_info() && package.general_info()->reported_version[0] >= 2 &&
                p <= bg->data_offset + bg->size && bg->data_offset + bg->size - p >= 64) {
                const auto metadata = rec(p, *bg, 64);
                tileset.width = static_cast<int>(metadata.u32(24));
                tileset.height = static_cast<int>(metadata.u32(28));
                tileset.border_x = static_cast<int>(metadata.u32(32));
                tileset.border_y = static_cast<int>(metadata.u32(36));
                tileset.columns = static_cast<int>(metadata.u32(40));
                tileset.frames_per_tile = static_cast<int>(metadata.u32(44));
                tileset.count = static_cast<int>(metadata.u32(48));
                tileset.frame_length_us = metadata.u64(56);
                if (tileset.width <= 0 || tileset.height <= 0 || tileset.width > 8192 ||
                    tileset.height > 8192 || tileset.border_x < 0 || tileset.border_x > 256 ||
                    tileset.border_y < 0 || tileset.border_y > 256 || tileset.columns <= 0 ||
                    tileset.columns > 8192 || tileset.frames_per_tile < 1 || tileset.frames_per_tile > 1024 ||
                    tileset.count < 0 || tileset.count > 1'000'000 ||
                    static_cast<std::uint64_t>(tileset.count) * tileset.frames_per_tile > 4'000'000 ||
                    static_cast<std::uint64_t>(tileset.count) * tileset.frames_per_tile * 4 >
                        bg->data_offset + bg->size - p - 64)
                    tileset = {};
                else {
                    const auto ids = rec(p, *bg, 64 +
                                                     static_cast<std::size_t>(tileset.count) *
                                                         static_cast<std::size_t>(tileset.frames_per_tile) * 4);
                    tileset.tile_ids.reserve(static_cast<std::size_t>(tileset.count) * tileset.frames_per_tile);
                    for (std::size_t i = 0; i < static_cast<std::size_t>(tileset.count) * tileset.frames_per_tile;
                         ++i)
                        tileset.tile_ids.push_back(ids.u32(64 + i * 4));
                }
            }
            tilesets_.push_back(tileset);
        }
    auto font_glyphs = [&](std::size_t p) {
        std::size_t best_offset = bytecode17 ? 44 : 40;
        std::size_t best_count = 0;
        const auto try_offset = [&](std::size_t offset) {
            const auto at = p + offset;
            if (at < fo->data_offset || at > fo->data_offset + fo->size ||
                fo->data_offset + fo->size - at < 4)
                return;
            const auto count = static_cast<std::size_t>(rec(at, *fo, 4).u32(0));
            if (count > 65536 || count > (fo->data_offset + fo->size - at - 4) / 4)
                return;
            const auto pointers = rec(at + 4, *fo, count * 4);
            for (std::size_t i = 0; i < count; ++i) {
                const auto glyph = pointers.u32(i * 4);
                if (!glyph || glyph < fo->data_offset || glyph > fo->data_offset + fo->size ||
                    fo->data_offset + fo->size - glyph < 16)
                    return;
            }
            if (count > best_count) {
                best_count = count;
                best_offset = offset;
            }
        };
        if (bytecode17) {
            for (const std::size_t offset : {44U, 48U, 52U, 56U})
                try_offset(offset);
        } else
            try_offset(40);
        return list(p + best_offset, *fo);
    };
    for (auto p : list(fo->data_offset, *fo)) {
        const auto r = rec(p, *fo, 44);
        Font font;
        const auto encoded_size = r.u32(8);
        const auto em_size = (encoded_size & 0x80000000U) != 0
                                 ? -std::bit_cast<float>(encoded_size)
                                 : static_cast<float>(encoded_size);
        if (!std::isfinite(em_size) || em_size < 0 || em_size > 8192)
            throw Error(ErrorKind::malformed, "invalid font size", p + 8);
        font.size = static_cast<int>(em_size);
        font.region = r.u32(28);
        if (!find_region(font.region))
            throw Error(ErrorKind::malformed, "font TPAG reference invalid", p + 28);
        for (auto q : font_glyphs(p)) {
            const auto g = rec(q, *fo, 16);
            const auto character = g.u16(0);
            if (character < font.glyphs.size()) {
                font.glyphs[character] = {g.u16(2), g.u16(4), g.u16(6), g.u16(8), g.i16(10), g.i16(12)};
                font.has_glyph[character] = true;
            }
        }
        fonts_.push_back(std::move(font));
    }
    background_regions_.reserve(backgrounds_.size());
    for (const auto pointer : backgrounds_)
        background_regions_.push_back(pointer ? find_region(pointer) : nullptr);
    for (auto &font : fonts_)
        font.atlas = find_region(font.region);
}
int GameAssets::add_sprite_font(int sprite_id,
                                std::string_view string_map,
                                bool proportional,
                                int separation) {
    if (!sprite_exists(sprite_id))
        throw Error(ErrorKind::vm, "font_add_sprite_ext received an invalid sprite");
    if (fonts_.size() >= static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw Error(ErrorKind::vm, "font limit reached");
    const auto &source = sprite(sprite_id);
    Font font;
    font.size = std::max(source.height, 1);
    font.page_per_glyph = true;
    for (std::size_t i = 0; i < source.frames.size() && i < string_map.size(); ++i) {
        const auto region_index = source.frames[i];
        if (region_index >= regions_.size())
            continue;
        const auto &region = regions_[region_index].region;
        if (region.x < 0 || region.y < 0 || region.w < 0 || region.h < 0 || region.x > 65535 ||
            region.y > 65535 || region.w > 65535 || region.h > 65535 || region.page < 0 ||
            region.page > 65535)
            throw Error(ErrorKind::unsupported, "sprite font glyph exceeds supported bounds");
        const int advance = (proportional ? region.w : source.width) + separation;
        if (advance < std::numeric_limits<std::int16_t>::min() ||
            advance > std::numeric_limits<std::int16_t>::max())
            throw Error(ErrorKind::unsupported, "sprite font advance exceeds supported bounds");
        const auto character = static_cast<unsigned char>(string_map[i]);
        font.glyphs[character] = {static_cast<std::uint16_t>(region.x),
                                  static_cast<std::uint16_t>(region.y),
                                  static_cast<std::uint16_t>(region.w),
                                  static_cast<std::uint16_t>(region.h),
                                  static_cast<std::int16_t>(advance),
                                  0,
                                  static_cast<std::uint16_t>(region.page)};
        font.has_glyph[character] = true;
    }
    auto atlas = std::make_unique<Region>();
    font.atlas = atlas.get();
    dynamic_font_atlases_.push_back(std::move(atlas));
    fonts_.push_back(std::move(font));
    return static_cast<int>(fonts_.size() - 1);
}
std::uint32_t GameAssets::find_region_index(std::size_t pointer) const noexcept {
    if (pointer > std::numeric_limits<std::uint32_t>::max())
        return std::numeric_limits<std::uint32_t>::max();
    const auto key = static_cast<std::uint32_t>(pointer);
    const auto found =
        std::lower_bound(regions_.begin(), regions_.end(), key,
                         [](const RegionEntry &entry, std::uint32_t value) { return entry.pointer < value; });
    if (found == regions_.end() || found->pointer != key ||
        static_cast<std::size_t>(found - regions_.begin()) >= std::numeric_limits<std::uint32_t>::max())
        return std::numeric_limits<std::uint32_t>::max();
    return static_cast<std::uint32_t>(found - regions_.begin());
}
const GameAssets::Region *GameAssets::find_region(std::size_t pointer) const noexcept {
    const auto index = find_region_index(pointer);
    return index == std::numeric_limits<std::uint32_t>::max() ? nullptr : &regions_[index].region;
}
std::uint64_t GameAssets::image_cache_key(const Region &region) const noexcept {
    std::uint64_t key = 1469598103934665603ULL;
    key = (key ^ cache_generation_) * 1099511628211ULL;
    for (const int value : {region.page, region.x, region.y, region.w, region.h})
        key = (key ^ static_cast<unsigned>(value)) * 1099511628211ULL;
    return key ? key : 1;
}
GameAssets::GameAssets(const Package &package, const GmrtProject &project)
    : package_(package), cache_generation_(++next_asset_cache_generation) {
    const auto &resources = project.sprites();
    std::size_t frame_count = 0;
    for (const auto &resource : resources)
        frame_count += resource.frames.size();
    sprites_.reserve(resources.size() + max_dynamic_sprites_);
    pages_.reserve(frame_count + max_dynamic_sprites_);
    regions_.reserve(frame_count + max_dynamic_sprites_);
    sprite_frame_regions_.reserve(frame_count + max_dynamic_sprites_);
    sprites_.resize(resources.size());
    std::vector<std::pair<std::size_t, std::size_t>> ranges(resources.size());
    for (std::size_t id = 0; id < resources.size(); ++id) {
        const auto &source = resources[id];
        auto &sprite = sprites_[id];
        if (source.id < 0)
            continue;
        sprite.name = source.name;
        sprite.width = source.width;
        sprite.height = source.height;
        sprite.ox = source.origin_x;
        sprite.oy = source.origin_y;
        sprite.left = source.left;
        sprite.right = source.right;
        sprite.top = source.top;
        sprite.bottom = source.bottom;
        ranges[id].first = sprite_frame_regions_.size();
        for (const auto &image_path : source.frames) {
            Page page;
            page.external_file = image_path.is_absolute() ? image_path : project.index_path().parent_path() / image_path;
            const auto page_id = static_cast<int>(pages_.size());
            pages_.push_back(std::move(page));
            Region region;
            region.w = source.width;
            region.h = source.height;
            region.tw = source.width;
            region.th = source.height;
            region.page = page_id;
            const auto region_id = static_cast<std::uint32_t>(regions_.size());
            regions_.push_back({0, region});
            sprite_frame_regions_.push_back(region_id);
        }
        ranges[id].second = sprite_frame_regions_.size() - ranges[id].first;
    }
    for (std::size_t id = 0; id < sprites_.size(); ++id)
        sprites_[id].frames = std::span<const std::uint32_t>(sprite_frame_regions_)
                                  .subspan(ranges[id].first, ranges[id].second);
}

const GameAssets::Sprite &GameAssets::sprite(int id) const {
    id = unpack_sprite_reference(id);
    if (id < 0 || static_cast<std::size_t>(id) >= sprites_.size())
        throw Error(ErrorKind::vm, "sprite ID out of range");
    return sprites_[static_cast<std::size_t>(id)];
}
bool GameAssets::set_sprite_offset(int id, int x, int y) {
    id = unpack_sprite_reference(id);
    if (id < 0 || static_cast<std::size_t>(id) >= sprites_.size())
        return false;
    auto &target = sprites_[static_cast<std::size_t>(id)];
    target.ox = x;
    target.oy = y;
    return true;
}
int GameAssets::create_sprite_from_surface(const Canvas &surface,
                                          int x,
                                          int y,
                                          int width,
                                          int height,
                                          bool remove_background,
                                          bool smooth,
                                          int origin_x,
                                          int origin_y) {
    if (width <= 0 || height <= 0 || dynamic_sprite_count_ >= max_dynamic_sprites_ ||
        sprites_.size() >= static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        regions_.size() >= std::numeric_limits<std::uint32_t>::max() ||
        pages_.size() >= static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        package_.size() > std::numeric_limits<std::uint32_t>::max() - max_dynamic_sprites_ - 1)
        return -1;
    const auto pixel_count = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    const auto byte_count = pixel_count * sizeof(std::uint32_t);
    if (pixel_count > std::numeric_limits<std::size_t>::max() ||
        byte_count > max_dynamic_sprite_bytes_ - dynamic_sprite_bytes_)
        return -1;

    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(pixel_count));
    const auto source = surface.pixels();
    for (int row = 0; row < height; ++row) {
        const auto source_y = static_cast<std::int64_t>(y) + row;
        if (source_y < 0 || source_y >= surface.height())
            continue;
        for (int column = 0; column < width; ++column) {
            const auto source_x = static_cast<std::int64_t>(x) + column;
            if (source_x >= 0 && source_x < surface.width())
                pixels[static_cast<std::size_t>(row) * width + column] =
                    source[static_cast<std::size_t>(source_y) * surface.width() + source_x];
        }
    }

    if (remove_background) {
        const auto background = pixels[static_cast<std::size_t>(height - 1) * width] & 0x00ffffffU;
        for (auto &pixel : pixels) {
            if ((pixel & 0x00ffffffU) == background)
                pixel &= 0x00ffffffU;
            else
                pixel |= 0xff000000U;
        }
        if (smooth) {
            auto smoothed = pixels;
            for (int row = 0; row < height; ++row)
                for (int column = 0; column < width; ++column) {
                    const auto index = static_cast<std::size_t>(row) * width + column;
                    if ((pixels[index] >> 24) == 0)
                        continue;
                    bool edge = false;
                    for (int dy = -1; dy <= 1 && !edge; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int nx = column + dx, ny = row + dy;
                            if (nx >= 0 && ny >= 0 && nx < width && ny < height &&
                                (pixels[static_cast<std::size_t>(ny) * width + nx] >> 24) == 0) {
                                edge = true;
                                break;
                            }
                        }
                    if (edge)
                        smoothed[index] = (smoothed[index] & 0x00ffffffU) | 0x80000000U;
                }
            pixels.swap(smoothed);
        }
    }

    int left = width, right = -1, top = height, bottom = -1;
    for (int row = 0; row < height; ++row)
        for (int column = 0; column < width; ++column)
            if ((pixels[static_cast<std::size_t>(row) * width + column] >> 24) != 0) {
                left = std::min(left, column);
                right = std::max(right, column);
                top = std::min(top, row);
                bottom = std::max(bottom, row);
            }
    if (right < 0)
        left = right = top = bottom = 0;

    const int id = static_cast<int>(sprites_.size());
    const int page_id = static_cast<int>(pages_.size());
    const auto region_id = static_cast<std::uint32_t>(regions_.size());
    const auto pointer = static_cast<std::uint32_t>(package_.size() + dynamic_sprite_count_ + 1);

    Page page;
    page.width = width;
    page.height = height;
    page.last_used = ++page_cache_clock_;
    page.format = TextureFormat::qoi;
    page.pixels = std::move(pixels);
    pages_.push_back(std::move(page));

    Region region;
    region.w = region.tw = width;
    region.h = region.th = height;
    region.page = page_id;
    regions_.push_back({pointer, region});
    const auto frame_start = sprite_frame_regions_.size();
    sprite_frame_regions_.push_back(region_id);

    Sprite sprite;
    sprite.width = width;
    sprite.height = height;
    sprite.ox = origin_x;
    sprite.oy = origin_y;
    sprite.left = left;
    sprite.right = right;
    sprite.top = top;
    sprite.bottom = bottom;
    sprite.frames = std::span<const std::uint32_t>(sprite_frame_regions_).subspan(frame_start, 1);
    sprites_.push_back(std::move(sprite));
    ++dynamic_sprite_count_;
    dynamic_sprite_bytes_ += static_cast<std::size_t>(byte_count);
    return id;
}
bool GameAssets::mask_pixel(int id, double frame, int x, int y) const {
    const auto &s = sprite(id);
    if (x < s.left || x > s.right || y < s.top || y > s.bottom || x < 0 || y < 0 || x >= s.width ||
        y >= s.height)
        return false;
    if (!s.precise || s.mask_count == 0)
        return true;
    const auto index = static_cast<std::size_t>(std::fmod(
        std::fmod(std::floor(frame), static_cast<double>(s.mask_count)) + static_cast<double>(s.mask_count),
        static_cast<double>(s.mask_count)));
    const auto stride = (static_cast<std::size_t>(s.width) + 7) / 8;
    const auto mask = mask_frame(id, index);
    const auto byte = mask[static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) / 8];
    return (byte & (0x80U >> (x & 7))) != 0;
}
std::span<const std::uint8_t> GameAssets::mask_frame(int id, std::size_t frame) const {
    const auto &s = sprite(id);
    if (frame >= s.mask_count)
        throw Error(ErrorKind::vm, "sprite mask frame out of range");
    const auto mask_size = ((static_cast<std::size_t>(s.width) + 7) / 8) * static_cast<std::size_t>(s.height);
    const auto offset = s.mask_offset + frame * mask_size;
    return std::span<const std::uint8_t>(mask_data_).subspan(offset, mask_size);
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
std::optional<GameAssets::TileSource> GameAssets::background_tile_source(int id,
                                                                         std::uint32_t tile,
                                                                         std::uint64_t elapsed_us) const {
    if (id < 0 || static_cast<std::size_t>(id) >= backgrounds_.size() ||
        static_cast<std::size_t>(id) >= tilesets_.size())
        return std::nullopt;
    const auto &tileset = tilesets_[static_cast<std::size_t>(id)];
    if (tile >= static_cast<std::uint32_t>(tileset.count) || tileset.width <= 0 || tileset.height <= 0 ||
        tileset.columns <= 0 || tileset.frames_per_tile <= 0)
        return std::nullopt;
    const auto frame = tileset.frame_length_us
                           ? static_cast<std::size_t>((elapsed_us / tileset.frame_length_us) %
                                                      static_cast<std::uint64_t>(tileset.frames_per_tile))
                           : 0;
    const auto tile_id_at = static_cast<std::size_t>(tile) * tileset.frames_per_tile + frame;
    if (tile_id_at >= tileset.tile_ids.size())
        return std::nullopt;
    tile = tileset.tile_ids[tile_id_at];
    if (tile >= static_cast<std::uint32_t>(tileset.count))
        return std::nullopt;
    const auto *region = background_regions_[static_cast<std::size_t>(id)];
    if (!region)
        return std::nullopt;
    const auto stride_x = tileset.width + 2 * tileset.border_x;
    const auto stride_y = tileset.height + 2 * tileset.border_y;
    const auto x = static_cast<int>(tile % static_cast<std::uint32_t>(tileset.columns)) * stride_x +
                   tileset.border_x;
    const auto y = static_cast<int>(tile / static_cast<std::uint32_t>(tileset.columns)) * stride_y +
                   tileset.border_y;
    if (x < 0 || y < 0 || x > region->tw - tileset.width || y > region->th - tileset.height)
        return std::nullopt;
    return TileSource{x, y, tileset.width, tileset.height};
}
GameAssets::Page &GameAssets::load_page(int id) {
    auto &page = pages_.at(static_cast<std::size_t>(id));
    if (!page.pixels.empty()) {
        page.last_used = ++page_cache_clock_;
        return page;
    }
    if constexpr (page_cache_limit_ != 0) {
        if (scratch_page_id_ == id && !scratch_page_.pixels.empty())
            return scratch_page_;
        if (!scratch_page_.pixels.empty())
            release_scratch_page();
    }
    png_image image{};
    struct Guard {
        png_image &image;
        bool active = false;
        ~Guard() {
            if (active)
                png_image_free(&image);
        }
    } guard{image};
    DecodedQoi qoi;
    std::vector<std::uint8_t> png_bytes;
    std::size_t width = 0, height = 0;
    const bool is_png = page.format == TextureFormat::png;
    if (is_png) {
        if (!page.external_file.empty()) {
            image.version = PNG_IMAGE_VERSION;
            const auto path = page.external_file.string();
            if (!png_image_begin_read_from_file(&image, path.c_str()))
                throw Error(ErrorKind::io, "cannot open sprite frame: " + path);
            guard.active = true;
            if (image.width > 8192 || image.height > 8192 || !image.width || !image.height)
                throw Error(ErrorKind::unsupported, "sprite frame dimensions exceed limit");
            if constexpr (std::endian::native == std::endian::little)
                image.format = PNG_FORMAT_BGRA;
            else
                image.format = PNG_FORMAT_ARGB;
            width = image.width;
            height = image.height;
        } else {
        const auto *chunk = package_.find_chunk("TXTR");
        const auto png_header = package_.read_bytes(page.png, 8);
        if (png_header[0] != 0x89 || png_header[1] != 'P' || png_header[2] != 'N' || png_header[3] != 'G' ||
            png_header[4] != 0x0d || png_header[5] != 0x0a || png_header[6] != 0x1a || png_header[7] != 0x0a)
            throw Error(ErrorKind::malformed, "texture PNG header invalid", page.png);
        std::size_t png_size = 8;
        bool found_end = false;
        const auto chunk_end = chunk->data_offset + chunk->size;
        while (page.png <= chunk_end && png_size <= chunk_end - page.png &&
               chunk_end - page.png - png_size >= 12) {
            const auto record = package_.read_bytes(page.png + png_size, 8);
            const auto length = (static_cast<std::size_t>(record[0]) << 24) |
                                (static_cast<std::size_t>(record[1]) << 16) |
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
        png_bytes = package_.read_bytes(page.png, png_size);
        image.version = PNG_IMAGE_VERSION;
        if (!png_image_begin_read_from_memory(&image, png_bytes.data(), png_bytes.size()))
            throw Error(ErrorKind::malformed, "texture PNG header invalid", page.png);
        guard.active = true;
        if (image.width > 8192 || image.height > 8192 || !image.width || !image.height)
            throw Error(ErrorKind::unsupported, "texture dimensions exceed limit", page.png);
        if constexpr (std::endian::native == std::endian::little)
            image.format = PNG_FORMAT_BGRA;
        else
            image.format = PNG_FORMAT_ARGB;
        width = image.width;
        height = image.height;
        }
    } else {
        std::size_t texture_size = page.data_size;
        if (!texture_size && page.format == TextureFormat::qoi) {
            const auto header = package_.read_bytes(page.png, 12);
            texture_size = 12 + static_cast<std::size_t>(read_le32(header.data() + 8));
        }
        const auto *chunk = package_.find_chunk("TXTR");
        const auto chunk_end = chunk->data_offset + chunk->size;
        if (texture_size < 12 || page.png < chunk->data_offset || page.png > chunk_end ||
            texture_size > chunk_end - page.png)
            throw Error(ErrorKind::malformed, "QOI texture block leaves TXTR", page.png);
        const auto bytes = package_.read_bytes(page.png, texture_size);
        qoi = decode_qoi(bytes, page.format == TextureFormat::bzip2_qoi, page.png);
        width = static_cast<std::size_t>(qoi.width);
        height = static_cast<std::size_t>(qoi.height);
    }
    const auto decoded_size = width * height * sizeof(std::uint32_t);
    if (decoded_size > 16ULL * 1024 * 1024)
        throw Error(ErrorKind::unsupported, "texture page exceeds Wii decode limit", page.png);
    const bool oversized_for_cache = page_cache_limit_ != 0 && decoded_size > page_cache_limit_;
    std::vector<std::uint32_t> pixels;
    if (oversized_for_cache) {
        // if a page is bigger than the whole cache, keep it around just for this draw
        // blit() frees that scratch memory on every exit
        scratch_page_ = {};
        scratch_page_id_ = id;
        pixels.swap(scratch_page_.pixels);
    } else {
        pixels.swap(page_buffer_);
    }
    if constexpr (page_cache_limit_ != 0) {
        if (!oversized_for_cache) {
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
        } else {
            // keep one oversized page ready. Undertale has lots of 8-16 MiB
            // pages, and tossing it after each sprite would make us decode
            // the same PNG over and over in one room
            for (auto &candidate : pages_) {
                if (!candidate.pixels.empty()) {
                    page_cache_bytes_ -= candidate.pixels.capacity() * sizeof(std::uint32_t);
                    std::vector<std::uint32_t>().swap(candidate.pixels);
                }
            }
        }
    }
    pixels.resize(width * height);
    if (is_png) {
        if (!png_image_finish_read(&image, nullptr, reinterpret_cast<unsigned char *>(pixels.data()), 0, nullptr))
            throw Error(ErrorKind::malformed,
                        std::string("texture PNG decode failed") +
                            (image.message[0] ? ": " + std::string(image.message) : ""),
                        page.png);
    } else
        std::copy(qoi.pixels.begin(), qoi.pixels.end(), pixels.begin());
    if (oversized_for_cache) {
        scratch_page_.width = static_cast<int>(width);
        scratch_page_.height = static_cast<int>(height);
        scratch_page_.pixels = std::move(pixels);
        scratch_region_valid_ = false;
        return scratch_page_;
    }
    const auto allocation_bytes = pixels.capacity() * sizeof(std::uint32_t);
    if constexpr (page_cache_limit_ != 0) {
        if (page_cache_bytes_ + allocation_bytes > page_cache_limit_) {
            for (auto &candidate : pages_) {
                if (!candidate.pixels.empty()) {
                    page_cache_bytes_ -= candidate.pixels.capacity() * sizeof(std::uint32_t);
                    std::vector<std::uint32_t>().swap(candidate.pixels);
                }
            }
            scratch_page_ = {};
            scratch_page_id_ = id;
            scratch_region_valid_ = false;
            scratch_page_.width = static_cast<int>(width);
            scratch_page_.height = static_cast<int>(height);
            scratch_page_.pixels = std::move(pixels);
            return scratch_page_;
        }
    }
    page.width = static_cast<int>(width);
    page.height = static_cast<int>(height);
    page.pixels = std::move(pixels);
    page.last_used = ++page_cache_clock_;
    page_cache_bytes_ += allocation_bytes;
    return page;
}
void GameAssets::release_scratch_page() noexcept {
    std::vector<std::uint32_t>().swap(scratch_page_.pixels);
    scratch_page_.width = scratch_page_.height = 0;
    scratch_page_id_ = -1;
    scratch_region_valid_ = false;
}
void GameAssets::prefetch(int id) {
    const auto &spr = sprite(id);
    if constexpr (page_cache_limit_ != 0)
        return;
    for (const auto region : spr.frames)
        if (region != std::numeric_limits<std::uint32_t>::max())
            load_page(regions_[region].region.page);
}
void GameAssets::prefetch(int id, double frame) {
    const auto &spr = sprite(id);
    if (spr.frames.empty() || !std::isfinite(frame))
        return;
    const auto region_index = spr.frames[frame_index(frame, spr.frames.size())];
    if (region_index == std::numeric_limits<std::uint32_t>::max())
        return;
    const auto &region = regions_[region_index].region;
    if (region.page < 0 || static_cast<std::size_t>(region.page) >= pages_.size())
        return;
    int source_x = 0, source_y = 0;
    const auto &page = region_pixels(region, source_x, source_y);
#if REGML_PS2
    (void)ps2_gpu_prefetch_image(page.pixels, page.width, source_x, source_y, region.w, region.h,
                                 image_cache_key(region));
    if (&page == &scratch_page_)
        release_scratch_page();
#else
    (void)page;
    (void)source_x;
    (void)source_y;
#endif
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
void GameAssets::draw_background_stretched(
    Canvas &canvas, int id, double x, double y, double width, double height) {
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
void GameAssets::draw_background_part_ext(Canvas &canvas,
                                          int id,
                                          double left,
                                          double top,
                                          double width,
                                          double height,
                                          double x,
                                          double y,
                                          double sx,
                                          double sy,
                                          unsigned color,
                                          double alpha) {
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
void GameAssets::draw_tile(Canvas &canvas,
                           int id,
                           int source_x,
                           int source_y,
                           int width,
                           int height,
                           double x,
                           double y,
                           double sx,
                           double sy,
                           unsigned color,
                           std::uint32_t tile_data) {
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
              right = std::min(source_x + width, r.tx + r.tw),
              bottom = std::min(source_y + height, r.ty + r.th);
    if (right <= left || bottom <= top)
        return;
    Region cut{r.x + (left - r.tx), r.y + (top - r.ty), right - left, bottom - top, 0, 0,
               right - left,        bottom - top,       r.page};
    cut.alpha_mode = r.alpha_mode;
    const auto draw_x = x + (left - source_x) * sx;
    const auto draw_y = y + (top - source_y) * sy;
    constexpr std::uint32_t mirror_mask = 1U << 28;
    constexpr std::uint32_t flip_mask = 1U << 29;
    constexpr std::uint32_t rotate_mask = 1U << 30;
    const auto transform = tile_data & (mirror_mask | flip_mask | rotate_mask);
    if (!transform) {
        blit(canvas, cut, draw_x, draw_y, sx, sy, 0, color & 0xffffff,
             static_cast<double>(color >> 24) / 255);
        return;
    }

    double target_x = (draw_x - view_x_) * view_sx_ + port_x_;
    double target_y = (draw_y - view_y_) * view_sy_ + port_y_;
    double target_w = (right - left) * sx * view_sx_;
    double target_h = (bottom - top) * sy * view_sy_;
    bool mirror = (transform & mirror_mask) != 0;
    bool flip = (transform & flip_mask) != 0;
    if (target_w < 0) {
        target_x += target_w;
        target_w = -target_w;
        mirror = !mirror;
    }
    if (target_h < 0) {
        target_y += target_h;
        target_h = -target_h;
        flip = !flip;
    }
    const auto render_scale = canvas.render_scale();
    target_x *= render_scale;
    target_y *= render_scale;
    target_w *= render_scale;
    target_h *= render_scale;
    if (target_w <= 0 || target_h <= 0)
        return;

    const int dest_left = static_cast<int>(std::clamp(std::ceil(target_x - 0.5), 0.0,
                                                       static_cast<double>(canvas.width())));
    const int dest_top = static_cast<int>(std::clamp(std::ceil(target_y - 0.5), 0.0,
                                                     static_cast<double>(canvas.height())));
    const int dest_right = static_cast<int>(std::clamp(std::ceil(target_x + target_w - 0.5), 0.0,
                                                        static_cast<double>(canvas.width())));
    const int dest_bottom = static_cast<int>(std::clamp(std::ceil(target_y + target_h - 0.5), 0.0,
                                                         static_cast<double>(canvas.height())));
    if (dest_left >= dest_right || dest_top >= dest_bottom)
        return;

    int page_x = 0, page_y = 0;
    const auto &page = region_pixels(cut, page_x, page_y);
    const unsigned tint = ((color & 0xff) << 16) | (color & 0xff00) | ((color >> 16) & 0xff);
    const double alpha = static_cast<double>(color >> 24) / 255;
    for (int dy = dest_top; dy < dest_bottom; ++dy) {
        for (int dx = dest_left; dx < dest_right; ++dx) {
            double u = (dx + 0.5 - target_x) / target_w;
            double v = (dy + 0.5 - target_y) / target_h;
            if (transform & rotate_mask) {
                const auto old_u = u;
                u = v;
                v = 1 - old_u;
            }
            if (flip)
                v = 1 - v;
            if (mirror)
                u = 1 - u;
            const int sample_x = left + static_cast<int>(u * (right - left));
            const int sample_y = top + static_cast<int>(v * (bottom - top));
            if (sample_x < left || sample_x >= right || sample_y < top || sample_y >= bottom)
                continue;
            const int pixel_x = page_x + (sample_x - left) * cut.w / cut.tw;
            const int pixel_y = page_y + (sample_y - top) * cut.h / cut.th;
            const auto rgba = page.pixels[static_cast<std::size_t>(pixel_y) * page.width + pixel_x];
            canvas.rgba_pixel_tinted(dx, dy, rgba, tint, alpha);
        }
    }
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
                     (x2 + 1 - view_x_) * view_sx_ + port_x_, (y2 + 1 - view_y_) * view_sy_ + port_y_,
                     outline);
}
const GameAssets::Page &GameAssets::region_pixels(const Region &r, int &source_x, int &source_y) {
    source_x = r.x;
    source_y = r.y;
    constexpr std::size_t budget = region_cache_limit_;
    const auto bytes = static_cast<std::size_t>(r.w) * static_cast<std::size_t>(r.h) * sizeof(std::uint32_t);
    const std::array<int, 5> key{r.page, r.x, r.y, r.w, r.h};
    if constexpr (page_cache_limit_ != 0) {
        if (scratch_region_valid_ && scratch_region_key_ == key && !scratch_page_.pixels.empty()) {
            source_x = source_y = 0;
            return scratch_page_;
        }
        if (const auto found = region_cache_.find(key); found != region_cache_.end()) {
            found->second.last_used = ++page_cache_clock_;
            source_x = source_y = 0;
            return found->second;
        }
    }
#if REGML_LOW_MEMORY
    while (region_cache_bytes_ + bytes > budget && !region_cache_.empty()) {
        const auto oldest = std::min_element(region_cache_.begin(), region_cache_.end(),
                                             [](const auto &left, const auto &right) {
                                                 return left.second.last_used < right.second.last_used;
                                             });
        region_cache_bytes_ -= oldest->second.pixels.capacity() * sizeof(std::uint32_t);
        region_cache_.erase(oldest);
    }
    Page cropped;
    cropped.width = r.w;
    cropped.height = r.h;
    cropped.last_used = ++page_cache_clock_;
    const auto &page = pages_.at(static_cast<std::size_t>(r.page));
    if (page.format == TextureFormat::png && page.external_file.empty()) {
        if (!scratch_page_.pixels.empty())
            release_scratch_page();
        cropped.pixels = read_png_region(package_, page.png, r.x, r.y, r.w, r.h);
    } else {
        const auto &source = load_page(r.page);
        if (r.x < 0 || r.y < 0 || r.w < 0 || r.h < 0 || r.x + r.w > source.width ||
            r.y + r.h > source.height)
            throw Error(ErrorKind::malformed, "texture region outside page");
        cropped.pixels.resize(static_cast<std::size_t>(r.w) * r.h);
        for (int row = 0; row < r.h; ++row)
            std::copy_n(source.pixels.begin() + static_cast<std::ptrdiff_t>(r.y + row) * source.width + r.x,
                        r.w, cropped.pixels.begin() + static_cast<std::ptrdiff_t>(row) * r.w);
        if (&source == &scratch_page_)
            release_scratch_page();
    }
    source_x = source_y = 0;
    if (bytes <= budget) {
        auto [entry, inserted] = region_cache_.emplace(key, std::move(cropped));
        region_cache_bytes_ += entry->second.pixels.capacity() * sizeof(std::uint32_t);
        return entry->second;
    }
    scratch_page_ = std::move(cropped);
    scratch_page_id_ = -1;
    scratch_region_key_ = key;
    scratch_region_valid_ = true;
    return scratch_page_;
#else
    const auto &source = load_page(r.page);
    if (r.x < 0 || r.y < 0 || r.w < 0 || r.h < 0 || r.x + r.w > source.width || r.y + r.h > source.height)
        throw Error(ErrorKind::malformed, "texture region outside page");
    if constexpr (page_cache_limit_ != 0) {
        if (bytes <= budget) {
            Page cached;
            cached.width = r.w;
            cached.height = r.h;
            cached.last_used = ++page_cache_clock_;
            cached.pixels.resize(bytes / sizeof(std::uint32_t));
            for (int row = 0; row < r.h; ++row)
                std::copy_n(source.pixels.begin() + static_cast<std::ptrdiff_t>(r.y + row) * source.width +
                                r.x,
                            r.w, cached.pixels.begin() + static_cast<std::ptrdiff_t>(row) * r.w);
            const auto allocation_bytes = cached.pixels.capacity() * sizeof(std::uint32_t);
            while (region_cache_bytes_ + allocation_bytes > budget && !region_cache_.empty()) {
                const auto oldest = std::min_element(
                    region_cache_.begin(), region_cache_.end(), [](const auto &left, const auto &right) {
                        return left.second.last_used < right.second.last_used;
                    });
                region_cache_bytes_ -= oldest->second.pixels.capacity() * sizeof(std::uint32_t);
                region_cache_.erase(oldest);
            }
            if (region_cache_bytes_ + allocation_bytes <= budget) {
                auto [it, inserted] = region_cache_.emplace(key, std::move(cached));
                region_cache_bytes_ += allocation_bytes;
                source_x = source_y = 0;
                return it->second;
            }
        }
    }
    return source;
#endif
}
void GameAssets::blit(Canvas &canvas,
                      const Region &r,
                      double x,
                      double y,
                      double sx,
                      double sy,
                      double angle,
                      unsigned color,
                      double alpha) {
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
#if REGML_WII || REGML_PS2
    if (canvas.gpu_target()) {
#if REGML_WII
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
                *std::min_element(xx, xx + 4) >= canvas.width() ||
                *std::min_element(yy, yy + 4) >= canvas.height())
                return;
        }
#endif
        const auto key = image_cache_key(r);
#if REGML_WII
        if (wii_gpu_cached_image(r.w, r.h, x, y, r.tw * sx, r.th * sy, angle, tint, alpha, key))
            return;
        const auto &page = region_pixels(r, source_x, source_y);
        wii_gpu_image(page.pixels, page.width, source_x, source_y, r.w, r.h, x, y, r.tw * sx, r.th * sy,
                      angle, tint, alpha, key);
#elif REGML_PS2
        if (ps2_gpu_cached_image(r.w, r.h, x, y, r.tw * sx, r.th * sy, angle, tint, alpha, key))
            return;
        const auto &page = region_pixels(r, source_x, source_y);
        ps2_gpu_image(page.pixels, page.width, source_x, source_y, r.w, r.h, x, y, r.tw * sx, r.th * sy,
                      angle, tint, alpha, key);
        if (&page == &scratch_page_)
            release_scratch_page();
#endif
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
        const int left = static_cast<int>(
            std::clamp(std::floor(*std::min_element(xx, xx + 4)), 0.0, double(canvas.width())));
        const int right = static_cast<int>(
            std::clamp(std::ceil(*std::max_element(xx, xx + 4)), 0.0, double(canvas.width())));
        const int top = static_cast<int>(
            std::clamp(std::floor(*std::min_element(yy, yy + 4)), 0.0, double(canvas.height())));
        const int bottom = static_cast<int>(
            std::clamp(std::ceil(*std::max_element(yy, yy + 4)), 0.0, double(canvas.height())));
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
                        // use an actual source color; pick visible strokes over blank or dark pixels
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
                const auto at =
                    static_cast<std::size_t>(source_y + row) * static_cast<std::size_t>(page.width) +
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
        canvas.blit_rgba(page.pixels, page.width, source_x + crop_x, source_y + crop_y, dx, dy, width, height,
                         tint, alpha, source_alpha_mode);
        return;
    }
    if (angle == 0) {
        const double w = r.tw * sx, h = r.th * sy;
        // the flipped axis starts at the origin edge and stops before the far edge
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
                            alpha == 1
                                ? pixel >> 24
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
                const auto destination_index =
                    static_cast<std::size_t>(dy) * static_cast<std::size_t>(canvas.width()) +
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
                            result |= (((pixel >> shift & 255) * source_alpha +
                                        (previous >> shift & 255) * inverse + 127) /
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
            canvas.rgba_pixel_tinted(
                dx, dy,
                page.pixels[static_cast<std::size_t>(uy) * static_cast<std::size_t>(page.width) +
                            static_cast<std::size_t>(ux)],
                tint, alpha);
        }
}
void GameAssets::draw_sprite(Canvas &canvas,
                             int id,
                             double frame,
                             double x,
                             double y,
                             double sx,
                             double sy,
                             double angle,
                             unsigned color,
                             double alpha) {
    const auto &spr = sprite(id);
    if (spr.frames.empty())
        return;
    if (!std::isfinite(frame))
        throw Error(ErrorKind::vm, "nonfinite sprite frame");
    const auto index = frame_index(frame, spr.frames.size());
    const auto region_index = spr.frames[index];
    if (region_index == std::numeric_limits<std::uint32_t>::max())
        return;
    const auto &r = regions_[region_index].region;
    const double ox = (r.tx - spr.ox) * sx, oy = (r.ty - spr.oy) * sy;
    if (angle == 0) {
        blit(canvas, r, x + ox, y + oy, sx, sy, 0, color, alpha);
        return;
    }
    const double rad = angle * 3.141592653589793 / 180;
    blit(canvas, r, x + ox * std::cos(rad) + oy * std::sin(rad), y - ox * std::sin(rad) + oy * std::cos(rad),
         sx, sy, angle, color, alpha);
}
std::uint32_t GameAssets::sample_sprite(int id, double frame, double u, double v, bool repeat) {
    const auto &spr = sprite(id);
    if (spr.frames.empty())
        return 0;
    if (!std::isfinite(frame + u + v))
        throw Error(ErrorKind::vm, "nonfinite texture sample coordinate");
    const auto region_index = spr.frames[frame_index(frame, spr.frames.size())];
    if (region_index == std::numeric_limits<std::uint32_t>::max())
        return 0;
    const auto &r = regions_[region_index].region;
    if (spr.width <= 0 || spr.height <= 0 || r.tw <= 0 || r.th <= 0 || r.w <= 0 || r.h <= 0)
        return 0;

    auto wrap = [](double value) { return value - std::floor(value); };
    if (repeat) {
        u = wrap(u);
        v = wrap(v);
    } else {
        u = std::clamp(u, 0.0, 1.0);
        v = std::clamp(v, 0.0, 1.0);
    }
    const double pixel_x = u * spr.width - 0.5, pixel_y = v * spr.height - 0.5;
    const int x0 = static_cast<int>(std::floor(pixel_x)), y0 = static_cast<int>(std::floor(pixel_y));
    const double fx = pixel_x - x0, fy = pixel_y - y0;
    int source_x = 0, source_y = 0;
    const auto &page = region_pixels(r, source_x, source_y);
    auto texel = [&](int x, int y) -> std::uint32_t {
        if (repeat) {
            x = (x % spr.width + spr.width) % spr.width;
            y = (y % spr.height + spr.height) % spr.height;
        } else {
            x = std::clamp(x, 0, spr.width - 1);
            y = std::clamp(y, 0, spr.height - 1);
        }
        const double local_x = x + 0.5 - r.tx, local_y = y + 0.5 - r.ty;
        if (local_x < 0 || local_y < 0 || local_x >= r.tw || local_y >= r.th)
            return 0;
        const int sx = source_x + static_cast<int>(local_x * r.w / r.tw);
        const int sy = source_y + static_cast<int>(local_y * r.h / r.th);
        if (sx < 0 || sy < 0 || sx >= page.width || sy >= page.height)
            return 0;
        return page.pixels[static_cast<std::size_t>(sy) * static_cast<std::size_t>(page.width) + sx];
    };
    const std::array<std::uint32_t, 4> colors = {
        texel(x0, y0), texel(x0 + 1, y0), texel(x0, y0 + 1), texel(x0 + 1, y0 + 1)};
    const std::array<double, 4> weights = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
    std::uint32_t result = 0;
    for (unsigned shift : {0U, 8U, 16U, 24U}) {
        double channel = 0;
        for (std::size_t i = 0; i < colors.size(); ++i)
            channel += ((colors[i] >> shift) & 255) * weights[i];
        result |= static_cast<std::uint32_t>(std::lround(channel)) << shift;
    }
    return result;
}
void GameAssets::draw_sprite_part(Canvas &canvas,
                                  int id,
                                  double frame,
                                  double left,
                                  double top,
                                  double width,
                                  double height,
                                  double x,
                                  double y,
                                  double sx,
                                  double sy,
                                  unsigned color,
                                  double alpha) {
    const auto &spr = sprite(id);
    if (spr.frames.empty())
        return;
    if (!std::isfinite(frame + left + top + width + height + x + y + sx + sy + alpha))
        throw Error(ErrorKind::vm, "nonfinite sprite-part argument");
    if (width <= 0 || height <= 0 || sx == 0 || sy == 0)
        return;
    const auto index = frame_index(frame, spr.frames.size());
    const auto region_index = spr.frames[index];
    if (region_index == std::numeric_limits<std::uint32_t>::max())
        return;
    const auto &r = regions_[region_index].region;
    const double l = std::max({left, 0.0, static_cast<double>(r.tx)}),
                 t = std::max({top, 0.0, static_cast<double>(r.ty)});
    const double rr = std::min(
                     {left + width, static_cast<double>(spr.width), static_cast<double>(r.tx + r.tw)}),
                 bb = std::min(
                     {top + height, static_cast<double>(spr.height), static_cast<double>(r.ty + r.th)});
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
    // draw_sprite_part uses the cropped region's top-left, so the sprite origin doesn't matter
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
double GameAssets::text_character_width(int font, unsigned char character) const noexcept {
    if (font < 0 || static_cast<std::size_t>(font) >= fonts_.size())
        return 6;
    const auto &f = fonts_[static_cast<std::size_t>(font)];
    return f.has_glyph[character] ? f.glyphs[character].shift : 0;
}
double GameAssets::text_height(int font) const {
    return font < 0 || static_cast<std::size_t>(font) >= fonts_.size()
               ? 8
               : fonts_[static_cast<std::size_t>(font)].size;
}
void GameAssets::draw_text(Canvas &canvas,
                           int font,
                           double x,
                           double y,
                           std::string_view text,
                           unsigned color,
                           double alpha,
                           int halign,
                           int valign,
                           double sx,
                           double sy,
                           double angle) {
    if (font < 0 || static_cast<std::size_t>(font) >= fonts_.size()) {
        canvas.color(color);
        canvas.alpha(alpha);
        canvas.text((x - view_x_) * view_sx_ + port_x_,
                    (y - view_y_) * view_sy_ + port_y_,
                    text,
                    sx * view_sx_,
                    sy * view_sy_,
                    angle,
                    halign,
                    valign);
        return;
    }
    const auto &f = fonts_[static_cast<std::size_t>(font)];
    const auto &atlas = *f.atlas;
    const double start = -text_width(font, text) * halign / 2.0;
    double lx = start, ly = -text_height(font) * valign / 2.0;
#if REGML_PS2 && REGML_LOW_MEMORY
    if (canvas.gpu_target() && !f.page_per_glyph && angle == 0 && sx > 0 && sy > 0 && view_sx_ > 0 &&
        view_sy_ > 0 && alpha > 0 && !text.empty() && atlas.page >= 0 &&
        static_cast<std::size_t>(atlas.page) < pages_.size()) {
        auto &page = pages_[static_cast<std::size_t>(atlas.page)];
        if (page.format == TextureFormat::png && page.external_file.empty()) {
            if (page.width <= 0 || page.height <= 0) {
                std::array<std::uint8_t, 24> header{};
                package_.read_into(page.png, header);
                if (header[0] != 0x89 || header[1] != 'P' || header[2] != 'N' || header[3] != 'G' ||
                    header[12] != 'I' || header[13] != 'H' || header[14] != 'D' || header[15] != 'R')
                    throw Error(ErrorKind::malformed, "texture PNG has no IHDR", page.png);
                page.width = static_cast<int>((static_cast<std::uint32_t>(header[16]) << 24) |
                                              (static_cast<std::uint32_t>(header[17]) << 16) |
                                              (static_cast<std::uint32_t>(header[18]) << 8) | header[19]);
                page.height = static_cast<int>((static_cast<std::uint32_t>(header[20]) << 24) |
                                               (static_cast<std::uint32_t>(header[21]) << 16) |
                                               (static_cast<std::uint32_t>(header[22]) << 8) | header[23]);
            }

            constexpr std::size_t glyphs_per_batch = 64;
            struct PendingGlyph {
                Ps2FontGlyph gpu;
                Region region;
                double x, y;
            };
            std::array<PendingGlyph, glyphs_per_batch> pending{};
            std::array<Ps2FontGlyph, glyphs_per_batch> gpu_glyphs{};
            std::size_t count = 0;
            int batch_tile_x = -1, batch_tile_y = -1;
            const double render_scale = canvas.render_scale();
            const auto page_key = (static_cast<std::uint64_t>(cache_generation_) << 32) |
                                  (static_cast<std::uint32_t>(atlas.page) + 1U);
            const unsigned tint = ((color & 0xff) << 16) | (color & 0xff00) | ((color >> 16) & 0xff);

            auto flush = [&] {
                if (count == 0)
                    return;
                for (std::size_t i = 0; i < count; ++i)
                    gpu_glyphs[i] = pending[i].gpu;
                auto group = std::span<const Ps2FontGlyph>(gpu_glyphs.data(), count);
                bool drawn = ps2_gpu_font_glyphs({}, 0, 0, 0, batch_tile_x, batch_tile_y, page_key,
                                                 group, tint, alpha);
                if (!drawn && batch_tile_x >= 0 && batch_tile_y >= 0 &&
                    batch_tile_x < page.width && batch_tile_y < page.height) {
                    const int tile_width = std::min(128, page.width - batch_tile_x);
                    const int tile_height = std::min(128, page.height - batch_tile_y);
                    auto pixels = read_png_region(package_, page.png, batch_tile_x, batch_tile_y,
                                                  tile_width, tile_height);
                    drawn = ps2_gpu_font_glyphs(pixels, tile_width, 0, 0, batch_tile_x,
                                                batch_tile_y, page_key, group, tint, alpha);
                }
                if (!drawn)
                    for (std::size_t i = 0; i < count; ++i)
                        blit(canvas, pending[i].region, pending[i].x, pending[i].y, sx, sy,
                             0, color, alpha);
                count = 0;
                batch_tile_x = batch_tile_y = -1;
            };

            for (char raw : text) {
                const auto ch = static_cast<unsigned char>(raw);
                if (ch == '\n' || ch == '#') {
                    flush();
                    lx = start;
                    ly += text_height(font) + 2;
                    continue;
                }
                if (!f.has_glyph[ch])
                    continue;
                const auto &g = f.glyphs[ch];
                if (g.w > 0 && g.h > 0) {
                    Region region{atlas.x + g.x, atlas.y + g.y, g.w, g.h, 0, 0, g.w, g.h, atlas.page};
                    region.alpha_mode = g.alpha_mode;
                    const int tile_x = region.x / 128 * 128;
                    const int tile_y = region.y / 128 * 128;
                    if (region.x >= 0 && region.y >= 0 && region.x + region.w <= page.width &&
                        region.y + region.h <= page.height && region.w <= 128 && region.h <= 128 &&
                        region.x + region.w <= tile_x + 128 && region.y + region.h <= tile_y + 128) {
                        if (count && (batch_tile_x != tile_x || batch_tile_y != tile_y ||
                                      count == glyphs_per_batch))
                            flush();
                        batch_tile_x = tile_x;
                        batch_tile_y = tile_y;
                        const double ox = (lx + g.offset) * sx;
                        const double oy = ly * sy;
                        const double draw_x = ((x + ox - view_x_) * view_sx_ + port_x_) * render_scale;
                        const double draw_y = ((y + oy - view_y_) * view_sy_ + port_y_) * render_scale;
                        pending[count++] = {
                            {region.x, region.y, region.w, region.h,
                             static_cast<float>(draw_x), static_cast<float>(draw_y),
                             static_cast<float>(g.w * sx * view_sx_ * render_scale),
                             static_cast<float>(g.h * sy * view_sy_ * render_scale)},
                            region, x + ox, y + oy};
                    } else {
                        flush();
                        const double ox = (lx + g.offset) * sx;
                        const double oy = ly * sy;
                        blit(canvas, region, x + ox, y + oy, sx, sy, 0, color, alpha);
                    }
                }
                lx += g.shift;
            }
            flush();
            return;
        }
    }
#endif
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
        Region r{atlas.x + g.x, atlas.y + g.y, g.w, g.h, 0, 0, g.w, g.h,
                 f.page_per_glyph ? static_cast<int>(g.page) : atlas.page};
        r.alpha_mode = g.alpha_mode;
        const auto ox = (lx + g.offset) * sx, oy = ly * sy;
        blit(canvas, r, x + ox * c + oy * s, y - ox * s + oy * c, sx, sy, angle, color, alpha);
        g.alpha_mode = r.alpha_mode;
        lx += g.shift;
    }
}
} // namespace regml
