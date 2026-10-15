#pragma once
#include "package.hpp"
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
namespace regml {
class GameAudio {
  public:
    GameAudio(const Package &package, std::filesystem::path directory);
    ~GameAudio();
    std::optional<int> find_sound_id(std::string_view filename) const;
    int create_stream(std::string_view filename);
    int play(int sound, bool loop, double priority);
    void stop(int id);
    void pause(int id, bool paused);
    bool playing(int id) const;
    double gain(int id);
    void gain(int id, double value);
    void pitch(int id, double value);
    double pitch(int id) const;
    void channel_limit(int count);
    double length(int id);
    double position(int id) const;
    void position(int id, double seconds);
    std::vector<float> mix(std::size_t frames);
    void mix(std::size_t frames, std::vector<float> &output);
    void mix(std::span<float> output, double playback_rate = 1.0) noexcept;
    void service();

  private:
#if REGML_WIIU
    struct AsyncState;
    std::unique_ptr<AsyncState> async_;
#endif
    struct Clip {
        std::vector<float> stereo;
        std::vector<std::uint8_t> compressed;
        std::string file_path;
        const Package *package = nullptr;
        std::size_t package_offset = 0, package_length = 0;
        std::size_t streamed_frames = 0;
        double gain = 1;
        std::size_t frames() const noexcept {
            return compressed.empty() && file_path.empty() && !package ? stereo.size() / 2 : streamed_frames;
        }
    };
    struct Stream;
    struct Sound {
        int embedded = -1;
        std::string filename;
        double gain = 1;
        std::unique_ptr<Clip> clip;
    };
    struct Voice {
        int sound = 0;
        Clip *clip = nullptr;
        double frame = 0, gain = 1, pitch = 1, priority = 0;
        bool loop = false, paused = false;
        std::shared_ptr<Stream> stream;
    };
    const Package &package_;
    std::filesystem::path directory_;
    std::vector<std::size_t> embedded_;
    std::unordered_map<int, Sound> sounds_;
    std::unordered_map<int, Voice> voices_;
    mutable std::mutex voices_mutex_;
    int next_stream_ = 100000, next_voice_ = 1000000, channels_ = 128;
    Clip *load(int id);
    std::vector<std::uint8_t> read_audio(int id) const;
    static std::unique_ptr<Clip> decode(std::vector<std::uint8_t> bytes);
};
} // namespace regml
