#pragma once
#include "regml/runtime/demo.hpp"
#include <memory>
#include <optional>
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
    const Canvas &canvas() const;
    std::size_t frames() const;
    std::size_t instructions() const;
    std::string room_name() const;
    std::span<const float> audio() const;
    double speed() const;
    std::vector<InstanceState> instance_states() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace regml
