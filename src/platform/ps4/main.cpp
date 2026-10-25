#include "lookup.hpp"
#include "play.hpp"
#include <exception>
#include <filesystem>
#include <iostream>

int main(int argc, char **argv) {
    const std::filesystem::path package = argc > 1 ? argv[1] : "/app0/data.win";
    const auto game_directory = package.parent_path();

    regml::PlayOptions options;
    options.save_directory = "/data/reGML/saves";
    options.debug_overlay = std::filesystem::exists(game_directory / regml::debug_overlay_marker);
    if (std::filesystem::exists(game_directory / "regml-input.txt"))
        options.input_script = game_directory / "regml-input.txt";
    if (std::filesystem::exists(game_directory / "regml-trace.enabled"))
        options.trace_log = game_directory / "regml-trace.log";
    if (std::filesystem::exists(game_directory / "regml-profile.enabled"))
        options.profile_log = game_directory / "regml-profile.csv";

    try {
        const auto report = regml::play_game(package, options);
        std::cout << "reGML " << regml::version << " PS4\nFrames: " << report.frames
                  << "\nVM instructions: " << report.instructions << "\nRoom: " << report.room << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "reGML PS4 error: " << error.what() << '\n';
        return 1;
    }
}
