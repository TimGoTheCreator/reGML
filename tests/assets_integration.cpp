#include "regml/render/assets.hpp"
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
        std::cout << "Undertale hole mask preserves safe tiles and player feet bounds\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
