#include "lookup.hpp"
#include "platform/wii/host.hpp"
#include "play.hpp"
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {
void wii_log(std::string_view message) {
    static bool started = false;
    auto *file = std::fopen("sd:/reGML/regml-boot.log", started ? "a" : "w");
    if (!file)
        return;
    std::fwrite(message.data(), 1, message.size(), file);
    std::fwrite("\n", 1, 1, file);
    std::fflush(file);
    std::fclose(file);
    started = true;
}
} // namespace

// SDL's Wii entry point initializes the input queues before handing control over.
extern "C" int SDL_main(int argc, char **argv) {
    try {
        regml::initialize_wii_platform();
        wii_log("FAT mounted");
        const std::filesystem::path package = argc > 1 ? argv[1] : "sd:/reGML/data.win";
        wii_log("starting package: " + package.string());
        regml::PlayOptions options;
        options.save_directory = "sd:/reGML/saves";
        const auto game_directory = package.parent_path();
        options.debug_overlay = std::filesystem::exists(game_directory / regml::debug_overlay_marker);
        if (std::filesystem::exists(game_directory / "regml-input.txt"))
            options.input_script = game_directory / "regml-input.txt";
        if (std::filesystem::exists(game_directory / "regml-trace.enabled"))
            options.trace_log = game_directory / "regml-trace.log";
        if (std::filesystem::exists(game_directory / "regml-profile.enabled"))
            options.profile_log = game_directory / "regml-profile.csv";
        wii_log("entering play_game");
        const auto report = regml::play_game(package, options);
        wii_log("play_game returned");
        std::cout << "reGML " << regml::version << " Wii\nFrames: " << report.frames
                  << "\nVM instructions: " << report.instructions << "\nRoom: " << report.room << '\n';
        return 0;
    } catch (const std::exception &error) {
        wii_log(std::string("error: ") + error.what());
        std::cerr << "reGML Wii error: " << error.what() << '\n';
        return 1;
    }
}
