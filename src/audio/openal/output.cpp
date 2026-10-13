#include "output.hpp"
#include "error.hpp"
#include "game.hpp"
#include <AL/al.h>
#include <AL/alc.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <string>

namespace regml {
namespace {
constexpr std::size_t block_frames = 1024;
constexpr std::size_t block_samples = block_frames * 2;
constexpr ALsizei block_bytes = static_cast<ALsizei>(block_samples * sizeof(std::int16_t));

void check_al(const char *operation) {
    const auto error = alGetError();
    if (error != AL_NO_ERROR)
        throw Error(ErrorKind::io, std::string(operation) + ": OpenAL error " + std::to_string(error));
}
} // namespace

struct OpenALOutput::Impl {
    GameSession &session;
    std::atomic<double> playback_rate;
    ALCdevice *device = nullptr;
    ALCcontext *context = nullptr;
    ALuint source = 0;
    std::array<ALuint, 4> buffers{};
    std::array<float, block_samples> samples{};
    std::array<std::int16_t, block_samples> pcm{};

    Impl(GameSession &game, double speed) : session(game), playback_rate(speed) {
        device = alcOpenDevice(nullptr);
        if (!device)
            throw Error(ErrorKind::io, "OpenAL device open failed");
        context = alcCreateContext(device, nullptr);
        if (!context) {
            alcCloseDevice(device);
            device = nullptr;
            throw Error(ErrorKind::io, "OpenAL context creation failed");
        }
        if (!alcMakeContextCurrent(context)) {
            alcDestroyContext(context);
            alcCloseDevice(device);
            context = nullptr;
            device = nullptr;
            throw Error(ErrorKind::io, "OpenAL context activation failed");
        }

        try {
            alGenSources(1, &source);
            check_al("OpenAL source creation");
            alGenBuffers(static_cast<ALsizei>(buffers.size()), buffers.data());
            check_al("OpenAL buffer creation");
            for (const auto buffer : buffers)
                queue_buffer(buffer);
            alSourceQueueBuffers(source, static_cast<ALsizei>(buffers.size()), buffers.data());
            check_al("OpenAL buffer queue");
            alSourcePlay(source);
            check_al("OpenAL playback start");
        } catch (...) {
            release();
            throw;
        }
    }

    ~Impl() { release(); }

    void release() noexcept {
        if (context && alcGetCurrentContext() == context) {
            if (source) {
                alSourceStop(source);
                alDeleteSources(1, &source);
                source = 0;
            }
            if (std::any_of(buffers.begin(), buffers.end(), [](ALuint buffer) { return buffer != 0; })) {
                alDeleteBuffers(static_cast<ALsizei>(buffers.size()), buffers.data());
                buffers.fill(0);
            }
            alcMakeContextCurrent(nullptr);
        }
        if (context) {
            alcDestroyContext(context);
            context = nullptr;
        }
        if (device) {
            alcCloseDevice(device);
            device = nullptr;
        }
    }

    void queue_buffer(ALuint buffer) {
        session.mix_audio(samples, playback_rate.load(std::memory_order_relaxed));
        std::transform(samples.begin(), samples.end(), pcm.begin(), [](float sample) {
            const auto bounded = std::clamp(sample, -1.0f, 1.0f);
            return static_cast<std::int16_t>(bounded * 32767.0f);
        });
        alBufferData(buffer, AL_FORMAT_STEREO16, pcm.data(), block_bytes, 48000);
        check_al("OpenAL buffer upload");
    }

    void pump() {
        ALint processed = 0;
        alGetSourcei(source, AL_BUFFERS_PROCESSED, &processed);
        check_al("OpenAL buffer query");
        while (processed-- > 0) {
            ALuint buffer = 0;
            alSourceUnqueueBuffers(source, 1, &buffer);
            check_al("OpenAL buffer unqueue");
            queue_buffer(buffer);
            alSourceQueueBuffers(source, 1, &buffer);
            check_al("OpenAL buffer requeue");
        }

        ALint state = 0;
        alGetSourcei(source, AL_SOURCE_STATE, &state);
        check_al("OpenAL playback query");
        if (state != AL_PLAYING) {
            alSourcePlay(source);
            check_al("OpenAL playback restart");
        }
    }
};

OpenALOutput::OpenALOutput(GameSession &session, double speed) : impl_(std::make_unique<Impl>(session, speed)) {}
OpenALOutput::~OpenALOutput() = default;
void OpenALOutput::set_speed(double speed) noexcept {
    impl_->playback_rate.store(speed, std::memory_order_relaxed);
}
void OpenALOutput::pump() {
    impl_->pump();
}
} // namespace regml
