#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace regml {
inline constexpr std::size_t ps2_audio_frames = 512;

class GameSession;

class Ps2AudioOutput {
  public:
    explicit Ps2AudioOutput(GameSession &session);
    ~Ps2AudioOutput();
    Ps2AudioOutput(const Ps2AudioOutput &) = delete;
    Ps2AudioOutput &operator=(const Ps2AudioOutput &) = delete;

  private:
    static void worker_entry(void *context);
    void run();

    GameSession &session_;
    std::atomic<bool> running_{false};
    int thread_id_ = -1;
    int finished_sema_ = -1;
    alignas(64) std::array<float, ps2_audio_frames * 2> mixed_{};
    alignas(64) std::array<std::int16_t, ps2_audio_frames * 2> pcm_{};
    alignas(64) std::array<std::uint8_t, 32 * 1024> thread_stack_{};
};
} // namespace regml
