#include "lookup.hpp"
#include "platform/wiiu/paths.hpp"
#include "play.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <whb/sdcard.h>

int main(int argc, char **argv) {
    if (!WHBMountSdCard()) {
        std::cerr << "reGML Wii U error: failed to mount the SD card\n";
        return 1;
    }
    struct SdCardUnmount {
        ~SdCardUnmount() { WHBUnmountSdCard(); }
    } sd_card_unmount;

    const std::filesystem::path package = [argc, argv]() -> std::filesystem::path {
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if (!arg.starts_with("-"))
                return arg;
        }
        return "/vol/external01/reGML/data.win";
    }();
    regml::PlayOptions options;
    options.save_directory = regml::wiiu_save_directory();
    const auto game_directory = package.parent_path();
    if (std::filesystem::exists(game_directory / "regml-gx2gl.enabled") ||
        std::filesystem::exists(game_directory / "regml-opengl.enabled"))
        options.opengl = true;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--gl" || arg == "--opengl" || arg == "--gx2gl")
            options.opengl = true;
        else if (arg == "--software")
            options.opengl = false;
        else if (arg == "--trace")
            options.trace_log = game_directory / "regml-trace.log";
        else if (arg == "--profile")
            options.profile_log = game_directory / "regml-profile.csv";
        else if (arg == "--debug-overlay")
            options.debug_overlay = true;
    }
    try {
        const auto report = regml::play_game(package, options);
        std::cout << "reGML " << regml::version << " Wii U\nFrames: " << report.frames
                  << "\nVM instructions: " << report.instructions << "\nRoom: " << report.room << '\n';
        return 0;
    } catch (const std::exception &error) {
        try {
            std::ofstream error_file(game_directory / "regml-error.log", std::ios::trunc);
            if (error_file)
                error_file << "reGML Wii U error: " << error.what() << '\n';
        } catch (...) {
        }
        std::cerr << "reGML Wii U error: " << error.what() << '\n';
        return 1;
    }
}
