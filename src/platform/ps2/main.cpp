#include "lookup.hpp"
#include "platform/ps2/frontend.hpp"
#include "play.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>

int main(int argc, char **argv) {
    try {
        regml::initialize_ps2_storage();
        std::filesystem::path package;
        if (argc > 1) {
            package = argv[1];
        } else {
            // PCSX2 can use the ELF folder as host:, so we don't need to fake a USB path
            const std::filesystem::path host_package = "host:reGML/data.win";
            std::error_code error;
            package = std::filesystem::exists(host_package, error) && !error ? host_package
                                                                             : "mass:/reGML/data.win";
        }
        regml::PlayOptions options;
        options.save_directory = "mass:/reGML/saves";
        const auto directory = package.parent_path();
        const auto exists = [](const std::filesystem::path &path) {
            std::error_code error;
            return std::filesystem::exists(path, error) && !error;
        };
        options.debug_overlay = exists(directory / regml::debug_overlay_marker);
        if (exists(directory / "regml-input.txt"))
            options.input_script = directory / "regml-input.txt";
        if (exists(directory / "regml-trace.enabled"))
            options.trace_log = directory / "regml-trace.log";
        if (exists(directory / "regml-profile.enabled"))
            options.profile_log = directory / "regml-profile.csv";

        const auto report = regml::play_game(package, options);
        std::cout << "reGML " << regml::version << " PS2\nFrames: " << report.frames
                  << "\nVM instructions: " << report.instructions << "\nRoom: " << report.room << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "reGML PS2 error: " << error.what() << '\n';
        std::ofstream host_log("host:regml-error.txt");
        host_log << "reGML PS2 error: " << error.what() << '\n';
        return 1;
    }
}
