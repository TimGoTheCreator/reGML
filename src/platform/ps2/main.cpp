#include "lookup.hpp"
#include "platform/ps2/frontend.hpp"
#include "play.hpp"
#include <cxxabi.h>
#include <cstdint>
#include <filesystem>
#include <exception>
#include <cstdio>
#include <cstdlib>
#include <iostream>

extern "C" {
extern volatile std::uint32_t regml_ps2_active_pc;
extern volatile std::uint32_t regml_ps2_active_opcode;
extern volatile std::uint32_t regml_ps2_active_symbol;
extern const char *regml_ps2_active_code_name;
extern volatile std::uint32_t regml_ps2_active_code_name_length;
}

int main(int argc, char **argv) {
    std::set_terminate([] {
        if (auto *log = std::fopen("host:reGML/regml-terminate.log", "a")) {
            std::setvbuf(log, nullptr, _IONBF, 0);
            const auto *type = abi::__cxa_current_exception_type();
            std::fprintf(log, "terminate type=%s code=%.*s pc=%u opcode=%u symbol=%u\n",
                         type ? type->name() : "none",
                         static_cast<int>(regml_ps2_active_code_name_length), regml_ps2_active_code_name,
                         static_cast<unsigned>(regml_ps2_active_pc),
                         static_cast<unsigned>(regml_ps2_active_opcode),
                         static_cast<unsigned>(regml_ps2_active_symbol));
            std::fclose(log);
        }
        std::_Exit(1);
    });
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
        options.uncapped = exists(directory / "regml-uncapped.enabled");
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
        std::fputs("reGML PS2 error: ", stderr);
        std::fputs(error.what(), stderr);
        std::fputc('\n', stderr);
        if (auto *log = std::fopen("host:reGML/regml-error.log", "a")) {
            std::setvbuf(log, nullptr, _IONBF, 0);
            std::fprintf(log, "main: %s\n", error.what());
            std::fclose(log);
        }
        return 1;
    }
}
