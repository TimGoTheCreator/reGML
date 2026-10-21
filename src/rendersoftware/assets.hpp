#pragma once
#include "canvas.hpp"
#include "package.hpp"
#include <array>
#include <bitset>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
namespace regml {
class GameAssets {
  public:
    explicit GameAssets(const Package &package);
    struct Sprite {
        std::string_view name;
        int width = 0, height = 0, ox = 0, oy = 0, left = 0, right = 0, top = 0, bottom = 0;
        std::span<const std::uint32_t> frames;
        bool precise = false;
        std::size_t mask_offset = 0, mask_count = 0;
    };
    const Sprite &sprite(int id) const;
    bool mask_pixel(int id, double frame, int x, int y) const;
    [[nodiscard]] std::span<const std::uint8_t> mask_frame(int id, std::size_t frame) const;
    [[nodiscard]] bool sprite_exists(int id) const noexcept {
        return id >= 0 && static_cast<std::size_t>(id) < sprites_.size();
    }
    [[nodiscard]] std::string_view sprite_name(int id) const { return sprite(id).name; }
    [[nodiscard]] std::pair<int, int> background_size(int id) const;
    void prefetch(int id);
    void draw_background(Canvas &canvas, int id, double x, double y);
    void draw_background_stretched(Canvas &canvas, int id, double x, double y, double width, double height);
    void draw_background_part_ext(Canvas &canvas,
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
                                  double alpha);
    void draw_tile(Canvas &canvas,
                   int id,
                   int source_x,
                   int source_y,
                   int width,
                   int height,
                   double x,
                   double y,
                   double sx,
                   double sy,
                   unsigned color);
    void draw_sprite(Canvas &canvas,
                     int id,
                     double frame,
                     double x,
                     double y,
                     double sx = 1,
                     double sy = 1,
                     double angle = 0,
                     unsigned color = 0xffffff,
                     double alpha = 1);
    void draw_sprite_part(Canvas &canvas,
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
                          double alpha);
    void draw_text(Canvas &canvas,
                   int font,
                   double x,
                   double y,
                   std::string_view text,
                   unsigned color,
                   double alpha,
                   int halign = 0,
                   int valign = 0,
                   double sx = 1,
                   double sy = 1,
                   double angle = 0);
    double text_width(int font, std::string_view text) const;
    double text_height(int font) const;
    void view(double x, double y, double sx, double sy, double px, double py);
    void rectangle(Canvas &canvas, double x1, double y1, double x2, double y2, bool outline) const;

  private:
    struct Page {
        int width = 0, height = 0;
        std::size_t png = 0;
        std::size_t last_used = 0;
        std::vector<std::uint32_t> pixels;
    };
    struct Region {
        int x = 0, y = 0, w = 0, h = 0, tx = 0, ty = 0, tw = 0, th = 0, page = 0;
        mutable std::int8_t alpha_mode = -1;
    };
    struct RegionEntry {
        std::uint32_t pointer = 0;
        Region region;
    };
    struct Glyph {
        std::uint16_t x = 0, y = 0, w = 0, h = 0;
        std::int16_t shift = 0, offset = 0;
        mutable std::int8_t alpha_mode = -1;
    };
    static_assert(sizeof(Glyph) <= 14, "font glyph metadata should stay compact");
    struct Font {
        std::size_t region = 0;
        int size = 0;
        std::array<Glyph, 256> glyphs{};
        std::bitset<256> has_glyph{};
        const Region *atlas = nullptr;
    };
    static_assert(sizeof(std::bitset<256>) <= 32, "glyph presence should stay bit-packed");
    const Package &package_;
    double view_x_ = 0, view_y_ = 0, view_sx_ = 1, view_sy_ = 1, port_x_ = 0, port_y_ = 0;
    std::vector<Sprite> sprites_;
    std::vector<std::uint32_t> sprite_frame_regions_;
    std::vector<std::uint8_t> mask_data_;
    std::vector<Page> pages_;
    Page scratch_page_;
    int scratch_page_id_ = -1;
    std::vector<RegionEntry> regions_;
    std::vector<const Region *> background_regions_;
    std::vector<Font> fonts_;
    std::vector<std::size_t> backgrounds_;
#if REGML_WII
    static constexpr std::size_t page_cache_limit_ = 4ULL * 1024 * 1024;
    static constexpr std::size_t region_cache_limit_ = 1ULL * 1024 * 1024;
#elif REGML_LOW_MEMORY
    static constexpr std::size_t page_cache_limit_ = 2ULL * 1024 * 1024;
    static constexpr std::size_t region_cache_limit_ = 512ULL * 1024;
#else
    static constexpr std::size_t page_cache_limit_ = 0;
    static constexpr std::size_t region_cache_limit_ = 0;
#endif
    std::size_t page_cache_bytes_ = 0, page_cache_clock_ = 0;
    std::vector<std::uint32_t> page_buffer_;
    struct RegionKeyHash {
        std::size_t operator()(const std::array<int, 5> &key) const noexcept {
            std::size_t hash = 0;
            for (const auto value : key)
                hash = (hash * 16777619U) ^ static_cast<std::size_t>(value);
            return hash;
        }
    };
    std::unordered_map<std::array<int, 5>, Page, RegionKeyHash> region_cache_;
    std::size_t region_cache_bytes_ = 0;
    const Page &region_pixels(const Region &region, int &source_x, int &source_y);
    std::vector<int> scaled_x_;
    std::vector<int> scaled_y_;
    struct ScaledRun {
        int offset, count, source_x;
        std::uint32_t rgba = 0;
    };
    std::vector<ScaledRun> scaled_runs_;
    [[nodiscard]] std::uint32_t find_region_index(std::size_t pointer) const noexcept;
    [[nodiscard]] const Region *find_region(std::size_t pointer) const noexcept;
    Page &load_page(int id);
    void release_scratch_page() noexcept;
    void blit(Canvas &canvas,
              const Region &r,
              double x,
              double y,
              double sx,
              double sy,
              double angle,
              unsigned color,
              double alpha);
};
} // namespace regml
