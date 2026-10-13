#pragma once
#include "demo/demo.hpp"
#include <memory>
#include <optional>
#include <span>
#include <string_view>
namespace regml {
class GameSession {
  public:
    struct InstanceState {
        int id;
        std::string object_name;
        VariableStore variables;
    };
    explicit GameSession(const std::filesystem::path &package, std::optional<std::uint32_t> seed = {},
                         const std::filesystem::path &save_directory = {});
    ~GameSession();
    void tick(const KeyboardState &held, const KeyboardState &pressed);
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
};
} // namespace regml
