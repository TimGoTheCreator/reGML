#pragma once
#include <memory>

namespace regml {
class GameSession;

class WiiAudioOutput {
  public:
    WiiAudioOutput(GameSession &session, double speed);
    ~WiiAudioOutput();
    WiiAudioOutput(const WiiAudioOutput &) = delete;
    WiiAudioOutput &operator=(const WiiAudioOutput &) = delete;

    void set_speed(double speed) noexcept;
    void pump();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace regml
