#include "lookup.hpp"
#include "platform/wiiu/paths.hpp"
#include "play.hpp"
#include <filesystem>
#include <iostream>
#include <string_view>

int main(int argc, char **argv) {
    const std::filesystem::path package = argc > 1 ? argv[1] : "/vol/external01/reGML/data.win";
    regml::PlayOptions options;
    options.save_directory = regml::wiiu_save_directory();
    const auto game_directory = package.parent_path();
    options.debug_overlay = std::filesystem::exists(game_directory / regml::debug_overlay_marker);
    if (std::filesystem::exists(game_directory / "regml-input.txt"))
        options.input_script = game_directory / "regml-input.txt";
    if (std::filesystem::exists(game_directory / "regml-trace.enabled"))
        options.trace_log = game_directory / "regml-trace.log";
    if (std::filesystem::exists(game_directory / "regml-profile.enabled"))
        options.profile_log = game_directory / "regml-profile.csv";
    try {
        const auto report = regml::play_game(package, options);
        std::cout << "reGML " << regml::version << " Wii U\nFrames: " << report.frames
                  << "\nVM instructions: " << report.instructions << "\nRoom: " << report.room << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "reGML Wii U error: " << error.what() << '\n';
        return 1;
    }
}
