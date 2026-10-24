#pragma once
#include "input.hpp"
#include "interpreter.hpp"
#include "rendersoftware/canvas.hpp"
#include <cstdint>
#include <filesystem>
#include <memory>
#if !REGML_PS2
#include <mutex>
#endif
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace regml {
class GameSession {
  public:
    struct TickProfile {
        double logic_ms = 0;
        double draw_ms = 0;
        double image_ms = 0;
        double text_ms = 0;
        std::uint64_t image_calls = 0;
        std::uint64_t image_quads = 0;
        std::uint64_t glyph_quads = 0;
        std::uint64_t texture_uploads = 0;
    };
    struct InstanceState {
        int id;
        std::string object_name;
        VariableStore variables;
    };
    explicit GameSession(const std::filesystem::path &package,
                         std::optional<std::uint32_t> seed = {},
                         const std::filesystem::path &save_directory = {});
    ~GameSession();
    void tick(const KeyboardState &held, const KeyboardState &pressed);
    void profile_tick(bool enabled) noexcept;
    TickProfile take_tick_profile() noexcept;
    void text_input(std::string_view text);
    Canvas &canvas();
    const Canvas &canvas() const;
    std::size_t frames() const;
    std::size_t instructions() const;
    std::string room_name() const;
    std::string_view vm_code_name() const;
    std::size_t vm_address() const;
    void mix_audio(std::span<float> output, double playback_rate = 1.0) noexcept;
    double speed() const;
    std::vector<InstanceState> instance_states() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
#if !REGML_PS2
    std::mutex audio_mutex_;
#endif
};
} // namespace regml
