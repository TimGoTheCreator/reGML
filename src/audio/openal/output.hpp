#pragma once
#include <memory>

namespace regml {
class GameSession;

class OpenALOutput {
  public:
    OpenALOutput(GameSession &session, double speed);
    ~OpenALOutput();
    OpenALOutput(const OpenALOutput &) = delete;
    OpenALOutput &operator=(const OpenALOutput &) = delete;

    void set_speed(double speed) noexcept;
    void pump();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace regml
