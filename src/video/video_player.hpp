#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

namespace regml {

class VideoPlayer {
  public:
    VideoPlayer();
    ~VideoPlayer();
    VideoPlayer(const VideoPlayer &) = delete;
    VideoPlayer &operator=(const VideoPlayer &) = delete;

    bool open(const std::filesystem::path &path);
    void close() noexcept;
    void pause() noexcept;
    void resume() noexcept;
    void enable_loop(bool enabled) noexcept;
    void set_volume(double volume) noexcept;
    [[nodiscard]] double volume() const noexcept;
    void mix_audio(std::span<float> stereo_output, double playback_rate = 1.0) noexcept;
    bool update_frame() noexcept;

    [[nodiscard]] bool loaded() const noexcept;
    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] bool has_frame() const noexcept;
    [[nodiscard]] int width() const noexcept;
    [[nodiscard]] int height() const noexcept;
    [[nodiscard]] int status() const noexcept;
    [[nodiscard]] double duration_ms() const noexcept;
    [[nodiscard]] double position_ms() const noexcept;
    [[nodiscard]] std::span<const std::uint32_t> pixels() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace regml
