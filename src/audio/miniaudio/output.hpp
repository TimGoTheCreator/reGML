#pragma once
#include <memory>

namespace regml {
class GameSession;

class MiniAudioOutput {
  public:
    MiniAudioOutput(GameSession &session, double speed);
    ~MiniAudioOutput();
    MiniAudioOutput(const MiniAudioOutput &) = delete;
    MiniAudioOutput &operator=(const MiniAudioOutput &) = delete;

    void set_speed(double speed) noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace regml
