#include "rendersoftware/assets.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <unordered_set>

int main(int argc, char **argv) {
    try {
        if (argc != 2)
            throw std::runtime_error("expected an Undertale data.win path");
        const auto package = regml::Package::load(argv[1]);
        regml::GameAssets assets(package);
        auto find_sprite = [&](std::string_view name) {
            for (int id = 0; assets.sprite_exists(id); ++id)
                if (assets.sprite_name(id) == name)
                    return id;
            throw std::runtime_error("required collision sprite missing: " + std::string(name));
        };
        const int hole = find_sprite("spr_holemask");
        if (!assets.sprite(hole).precise || assets.sprite(hole).mask_count == 0)
            throw std::runtime_error("hole bitmap mask was not loaded");
        if (assets.mask_pixel(hole, 0, 100, 10))
            throw std::runtime_error("safe route must be empty inside the hole's outer bounds");
        if (!assets.mask_pixel(hole, 0, 20, 10))
            throw std::runtime_error("unsafe tiles must remain collidable");
        if (assets.mask_pixel(hole, 0, -1, 10) || assets.mask_pixel(hole, 0, 301, 10))
            throw std::runtime_error("mask lookup must reject pixels outside the sprite");
        const int player = find_sprite("spr_maincharad");
        if (!assets.mask_pixel(player, 0, 10, 19) || assets.mask_pixel(player, 0, 10, 18))
            throw std::runtime_error("player collisions must use the feet bounding box");
        const auto &sprite = assets.sprite(player);
        regml::Canvas source(sprite.width, sprite.height);
        source.clear_transparent();
        assets.draw_sprite(source, player, 0, sprite.ox, sprite.oy);
        for (double sx : {-2.0, -1.0, -0.5, 0.5, 1.0, 2.0})
            for (double sy : {-2.0, -1.0, -0.5, 0.5, 1.0, 2.0})
                for (double offset : {0.0, 0.5, 0.7})
                    for (double alpha : {0.0, -0.2, 0.5, 1.0}) {
                        regml::Canvas actual(80, 80), expected(80, 80);
                        actual.clear(0x123456);
                        expected.clear(0x123456);
                        const double x = 40 + offset, y = 40 + offset;
                        assets.draw_sprite(actual, player, 0, x, y, sx, sy, 0, 0x3366cc, alpha);
                        for (int dy = 0; dy < 80; ++dy)
                            for (int dx = 0; dx < 80; ++dx) {
                                const double u = (dx + 0.5 - x) / sx + sprite.ox;
                                const double v = (dy + 0.5 - y) / sy + sprite.oy;
                                if (u >= 0 && v >= 0 && u < sprite.width && v < sprite.height)
                                    expected.rgba_pixel_tinted(
                                        dx, dy, source.pixel(static_cast<int>(u), static_cast<int>(v)),
                                        0xcc6633, alpha);
                            }
                        if (actual.hash() != expected.hash())
                            throw std::runtime_error(
                                "mirrored sprite coverage or compositing differs from source sampling");
                    }
        std::cout << "Undertale hole mask preserves safe tiles and player feet bounds\n";
        std::cout << "Mirrored sprites preserve pixel coverage, tint and alpha\n";
#if REGML_240P
        const int grid = find_sprite("spr_battlebg");
        const auto &grid_sprite = assets.sprite(grid);
        for (int frame = 0; frame < static_cast<int>(grid_sprite.frames.size()); ++frame) {
            regml::Canvas full(640, 480), reduced(640, 480, true);
            full.clear(0);
            reduced.clear(0);
            assets.draw_sprite(full, grid, frame, grid_sprite.ox, grid_sprite.oy);
            assets.draw_sprite(reduced, grid, frame, grid_sprite.ox, grid_sprite.oy);
            for (int y = 0; y < 240; ++y)
                for (int x = 0; x < 320; ++x) {
                    unsigned green = 0;
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx)
                            green += (full.pixel(x * 2 + dx, y * 2 + dy) >> 8) & 255;
                    if (green >= 4 && ((reduced.pixel(x, y) >> 8) & 255) == 0)
                        throw std::runtime_error("240p drops a battle-grid stroke: frame " +
                                                 std::to_string(frame) + " x " + std::to_string(x) + " y " +
                                                 std::to_string(y));
                }
        }
        std::cout << "240p preserves thin grid strokes in both battle-background frames\n";
        for (const auto name : {"spr_fightbt", "spr_itembt", "spr_actbt_center"}) {
            const int id = find_sprite(name);
            const auto &button = assets.sprite(id);
            regml::Canvas full(640, 480), reduced(640, 480, true);
            full.clear(0);
            reduced.clear(0);
            assets.draw_sprite(full, id, 0, button.ox, button.oy);
            assets.draw_sprite(reduced, id, 0, button.ox, button.oy);
            std::unordered_set<std::uint32_t> palette(full.pixels().begin(), full.pixels().end());
            for (const auto pixel : reduced.pixels())
                if (!palette.contains(pixel))
                    throw std::runtime_error("240p blurs a menu button by introducing interpolated colors");
        }
        std::cout << "240p menu buttons keep their original color palette\n";
#endif
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
