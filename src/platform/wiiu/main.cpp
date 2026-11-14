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

    const std::filesystem::path package = argc > 1 ? argv[1] : "/vol/external01/reGML/data.win";
    regml::PlayOptions options;
    options.save_directory = regml::wiiu_save_directory();
    const auto game_directory = package.parent_path();
    options.debug_overlay = std::filesystem::exists(game_directory / regml::debug_overlay_marker);
    if (std::filesystem::exists(game_directory / "regml-input.txt"))
        options.input_script = game_directory / "regml-input.txt";
    if (std::ifstream start_room_file(game_directory / "regml-start-room.txt"); start_room_file) {
        std::getline(start_room_file, options.start_room);
        if (!options.start_room.empty() && options.start_room.back() == '\r')
            options.start_room.pop_back();
        if (!options.start_room.empty())
            options.save_directory = game_directory / "regml-start-room-saves";
    }
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
