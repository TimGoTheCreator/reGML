#pragma once
#include <filesystem>
#include <optional>
namespace regml {
struct PlayOptions {
    std::size_t frame_limit = 0;
    bool headless = false;
    bool hidden = false;
    bool uncapped = false;
    bool speed_controls = false;
    bool debug_overlay = false;
    double speed_multiplier = 1.0;
    std::filesystem::path screenshot;
    std::filesystem::path input_script;
    std::filesystem::path trace_log;
    std::filesystem::path profile_log;
    std::optional<std::uint32_t> seed;
    std::filesystem::path save_directory;
};
struct PlayReport {
    std::size_t frames = 0, instructions = 0;
    std::uint64_t frame_hash = 0;
    std::string room;
};
PlayReport play_game(const std::filesystem::path &package, const PlayOptions &options);
} // namespace regml
