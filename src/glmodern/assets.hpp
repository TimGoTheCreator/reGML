#pragma once

#include "batcher.hpp"
#include "canvas.hpp"
#include "gl_common.hpp"
#include "gl_core.hpp"
#include "texture.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace regml {
class Package;
class GmrtProject;
class GameAssets;
} // namespace regml

namespace regml::glmodern {

class GLModernCanvas;

class GLModernAssets {
public:
    struct Sprite {
        std::string_view name;
        int width = 0;
        int height = 0;
        int ox = 0;
        int oy = 0;
        int left = 0;
        int right = 0;
        int top = 0;
        int bottom = 0;
        int bbox_mode = 0;
        int auto_left = 0;
        int auto_right = 0;
        int auto_top = 0;
        int auto_bottom = 0;
        std::span<const std::uint32_t> frames;
        bool precise = false;
        bool dynamic = false;
        bool valid = true;
        std::size_t mask_offset = 0;
        std::size_t mask_count = 0;
    };

    struct SpriteFrameInfo {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        int texture = -1;
        int original_width = 0;
        int original_height = 0;
        int crop_width = 0;
        int crop_height = 0;
        int x_offset = 0;
        int y_offset = 0;
    };

    struct Region {
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        int tx = 0;
        int ty = 0;
        int tw = 0;
        int th = 0;
        int page = 0;
        mutable std::int8_t alpha_mode = -1;
    };

    struct Page {
        int width = 0;
        int height = 0;
        GLuint texture_id = 0;
        std::vector<std::uint32_t> pixels;
        bool owned_texture = true;
    };

    struct Glyph {
        std::uint16_t x = 0;
        std::uint16_t y = 0;
        std::uint16_t w = 0;
        std::uint16_t h = 0;
        std::int16_t shift = 0;
        std::int16_t offset = 0;
        std::uint16_t page = 0;
    };

    struct Font {
        int size = 0;
        std::array<Glyph, 256> glyphs{};
        std::array<bool, 256> has_glyph{};
    };

    GLModernAssets() noexcept;
    explicit GLModernAssets(const regml::Package &package);
    GLModernAssets(const regml::Package &package, const regml::GmrtProject &project);
    explicit GLModernAssets(const regml::GameAssets &source);
    ~GLModernAssets();

    GLModernAssets(const GLModernAssets &) = delete;
    GLModernAssets &operator=(const GLModernAssets &) = delete;

    GLModernAssets(GLModernAssets &&other) noexcept;
    GLModernAssets &operator=(GLModernAssets &&other) noexcept;

    [[nodiscard]] const Sprite &sprite(int id) const;
    [[nodiscard]] bool sprite_exists(int id) const noexcept;
    [[nodiscard]] std::string_view sprite_name(int id) const;
    [[nodiscard]] std::size_t sprite_count() const noexcept;
    [[nodiscard]] std::optional<SpriteFrameInfo> sprite_frame_info(int id, std::size_t frame) const;
    [[nodiscard]] std::array<double, 8> sprite_uvs(int id, double frame) const;
    void texture_texel_size(int id, double frame, double &width, double &height) const;
    [[nodiscard]] std::pair<int, int> background_size(int id) const;

    void draw_sprite(GLModernCanvas &canvas,
                     int id,
                     double frame,
                     double x,
                     double y,
                     double sx = 1.0,
                     double sy = 1.0,
                     double angle = 0.0,
                     unsigned color = 0xffffff,
                     double alpha = 1.0);

    void draw_sprite_ext(GLModernCanvas &canvas,
                         int id,
                         double frame,
                         double x,
                         double y,
                         double sx,
                         double sy,
                         double angle,
                         unsigned color,
                         double alpha);

    void draw_sprite_part(GLModernCanvas &canvas,
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

    void draw_sprite_pos(GLModernCanvas &canvas,
                         int id,
                         double frame,
                         const std::array<double, 8> &corners,
                         double alpha);

    void draw_sprite_general(GLModernCanvas &canvas,
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
                             double angle,
                             const std::array<unsigned, 4> &colors,
                             double alpha);

    void draw_sprite_stretched(GLModernCanvas &canvas,
                               int id,
                               double frame,
                               double x,
                               double y,
                               double width,
                               double height,
                               unsigned color = 0xffffff,
                               double alpha = 1.0);

    void draw_background(GLModernCanvas &canvas, int id, double x, double y);
    void draw_background_stretched(GLModernCanvas &canvas, int id, double x, double y, double width, double height);
    void draw_background_part_ext(GLModernCanvas &canvas,
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

    void draw_tile(GLModernCanvas &canvas,
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
                   std::uint32_t tile_data = 0);

    void draw_text(GLModernCanvas &canvas,
                   int font,
                   double x,
                   double y,
                   std::string_view text,
                   unsigned color,
                   double alpha,
                   int halign = 0,
                   int valign = 0,
                   double sx = 1.0,
                   double sy = 1.0,
                   double angle = 0.0);

    void view(double x, double y, double sx, double sy, double px, double py) noexcept;
    [[nodiscard]] std::array<double, 2> transform_point(double x, double y) const noexcept;
    [[nodiscard]] double view_scale_x() const noexcept { return view_sx_; }
    [[nodiscard]] double view_scale_y() const noexcept { return view_sy_; }

    int add_sprite(Sprite spr, std::vector<std::uint32_t> frame_regions = {});
    int add_region(Region region);
    int add_page(int width, int height, std::span<const std::uint32_t> pixels = {}, GLuint texture_id = 0);
    int add_background(int region_index);

    GLuint page_texture(int page_index);
    void release_gl_resources() noexcept;

    static void set_active(GLModernAssets *assets) noexcept { s_active_ = assets; }
    [[nodiscard]] static GLModernAssets *active() noexcept;

private:
    void blit(GLModernCanvas &canvas,
              const Region &r,
              double x,
              double y,
              double sx,
              double sy,
              double angle,
              unsigned color,
              double alpha);

    [[nodiscard]] static std::size_t frame_index(double frame, std::size_t count) noexcept;

    double view_x_ = 0.0;
    double view_y_ = 0.0;
    double view_sx_ = 1.0;
    double view_sy_ = 1.0;
    double port_x_ = 0.0;
    double port_y_ = 0.0;

    std::vector<Sprite> sprites_;
    std::vector<std::vector<std::uint32_t>> sprite_frames_storage_;
    std::vector<Region> regions_;
    std::vector<Page> pages_;
    std::vector<int> backgrounds_;
    std::vector<Font> fonts_;

    std::unique_ptr<regml::GameAssets> owned_source_assets_;
    const regml::GameAssets *source_assets_ = nullptr;

    static GLModernAssets *s_active_;
};

using GameAssets = GLModernAssets;
using Assets = GLModernAssets;

void draw_sprite(GLModernCanvas &canvas,
                 int id,
                 double frame,
                 double x,
                 double y,
                 double sx = 1.0,
                 double sy = 1.0,
                 double angle = 0.0,
                 unsigned color = 0xffffff,
                 double alpha = 1.0);

void draw_sprite_ext(GLModernCanvas &canvas,
                     int id,
                     double frame,
                     double x,
                     double y,
                     double sx,
                     double sy,
                     double angle,
                     unsigned color,
                     double alpha);

void draw_sprite_part(GLModernCanvas &canvas,
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

void draw_sprite_pos(GLModernCanvas &canvas,
                     int id,
                     double frame,
                     const std::array<double, 8> &corners,
                     double alpha);

void draw_sprite_general(GLModernCanvas &canvas,
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
                         double angle,
                         const std::array<unsigned, 4> &colors,
                         double alpha);

void draw_sprite_stretched(GLModernCanvas &canvas,
                           int id,
                           double frame,
                           double x,
                           double y,
                           double width,
                           double height,
                           unsigned color = 0xffffff,
                           double alpha = 1.0);

void draw_background(GLModernCanvas &canvas, int id, double x, double y);
void draw_background_stretched(GLModernCanvas &canvas, int id, double x, double y, double width, double height);
void draw_background_part_ext(GLModernCanvas &canvas,
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

void draw_tile(GLModernCanvas &canvas,
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
               std::uint32_t tile_data = 0);

} // namespace regml::glmodern
