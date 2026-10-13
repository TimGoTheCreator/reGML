#include "rendersoftware/assets.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>

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
        if (!assets.sprite(hole).precise || assets.sprite(hole).masks.empty())
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
                                    expected.rgba_pixel_tinted(dx, dy, source.pixel(static_cast<int>(u), static_cast<int>(v)),
                                                               0xcc6633, alpha);
                            }
                        if (actual.hash() != expected.hash())
                            throw std::runtime_error("mirrored sprite coverage or compositing differs from source sampling");
                    }
        std::cout << "Undertale hole mask preserves safe tiles and player feet bounds\n";
        std::cout << "Mirrored sprites preserve pixel coverage, tint and alpha\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
