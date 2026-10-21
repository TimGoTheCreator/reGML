#include "output.hpp"
#include "error.hpp"
#include "game.hpp"
#include <atomic>
#include <span>
#include <string>

#include "miniaudio.h"

namespace regml {
struct MiniAudioOutput::Impl {
    GameSession &session;
    std::atomic<double> playback_rate;
    ma_device device{};
    bool initialized = false;

    Impl(GameSession &game, double speed) : session(game), playback_rate(speed) {
        auto config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = 2;
        config.sampleRate = 48000;
        config.periodSizeInFrames = 1024;
        config.dataCallback = &Impl::data_callback;
        config.pUserData = this;

        auto result = ma_device_init(nullptr, &config, &device);
        if (result != MA_SUCCESS)
            throw Error(ErrorKind::io,
                        std::string("miniaudio device init: ") + ma_result_description(result));
        initialized = true;

        result = ma_device_start(&device);
        if (result != MA_SUCCESS) {
            ma_device_uninit(&device);
            initialized = false;
            throw Error(ErrorKind::io,
                        std::string("miniaudio device start: ") + ma_result_description(result));
        }
    }

    ~Impl() {
        if (initialized)
            ma_device_uninit(&device);
    }

    static void data_callback(ma_device *device, void *output, const void *, ma_uint32 frame_count) noexcept {
        auto &self = *static_cast<Impl *>(device->pUserData);
        auto samples =
            std::span<float>(static_cast<float *>(output), static_cast<std::size_t>(frame_count) * 2);
        self.session.mix_audio(samples, self.playback_rate.load(std::memory_order_relaxed));
    }
};

MiniAudioOutput::MiniAudioOutput(GameSession &session, double speed)
    : impl_(std::make_unique<Impl>(session, speed)) {}
MiniAudioOutput::~MiniAudioOutput() = default;

void MiniAudioOutput::set_speed(double speed) noexcept {
    impl_->playback_rate.store(speed, std::memory_order_relaxed);
}
} // namespace regml
