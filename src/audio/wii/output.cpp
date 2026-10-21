#include "output.hpp"
#include "error.hpp"
#include "game.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <ogc/audio.h>
#include <ogc/cache.h>
#include <ogc/lwp.h>
#include <ogc/semaphore.h>
#include <span>

namespace regml {
namespace {
constexpr std::size_t block_frames = 1024;
constexpr std::size_t block_samples = block_frames * 2;
constexpr std::size_t dma_bytes = block_samples * sizeof(std::int16_t);
constexpr std::size_t buffer_count = 4;
constexpr std::size_t worker_stack_size = 32 * 1024;

enum class BufferState : unsigned { free, filling, ready, playing };
static_assert(std::atomic<BufferState>::is_always_lock_free);
static_assert(std::atomic<int>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<float>::is_always_lock_free);
} // namespace

struct WiiAudioOutput::Impl {
    struct alignas(32) DmaBuffer {
        std::array<std::int16_t, block_samples> samples{};
    };

    GameSession &session;
    std::atomic<float> playback_rate;
    std::array<DmaBuffer, buffer_count> buffers{};
    std::array<std::atomic<BufferState>, buffer_count> states{};
    alignas(32) std::array<std::int16_t, block_samples> silence{};
    std::array<float, block_samples> mix_buffer{};
    alignas(32) std::array<u8, worker_stack_size> worker_stack{};
    std::atomic<int> playing{-1};
    std::atomic<bool> running{false};
    lwp_t worker = LWP_THREAD_NULL;
    sem_t wake_sem = LWP_SEM_NULL;
    std::size_t next_play_buffer = 0;
    std::size_t next_fill_buffer = 0;
    bool semaphore_initialized = false;
    bool worker_started = false;

    static std::atomic<Impl *> active;

    Impl(GameSession &game, double speed) : session(game), playback_rate(static_cast<float>(speed)) {
        Impl *expected = nullptr;
        if (!active.compare_exchange_strong(expected, this, std::memory_order_acq_rel))
            throw Error(ErrorKind::io, "Wii audio output is already active");

        try {
            AUDIO_Init(nullptr);
            AUDIO_StopDMA();
            AUDIO_SetDSPSampleRate(AI_SAMPLERATE_48KHZ);
            DCFlushRange(silence.data(), static_cast<u32>(dma_bytes));

            for (std::size_t i = 0; i < buffer_count; ++i)
                states[i].store(BufferState::free, std::memory_order_relaxed);

            if (LWP_SemInit(&wake_sem, 0, buffer_count) < 0)
                throw Error(ErrorKind::io, "Wii audio worker semaphore could not be created");
            semaphore_initialized = true;

            AUDIO_RegisterDMACallback(&Impl::dma_callback);
            AUDIO_InitDMA(address(silence.data()), static_cast<u32>(dma_bytes));

            running.store(true, std::memory_order_release);
            if (LWP_CreateThread(&worker, &Impl::worker_entry, this, worker_stack.data(),
                                 static_cast<u32>(worker_stack.size()), 80) < 0) {
                throw Error(ErrorKind::io, "Wii audio worker thread could not be created");
            }
            worker_started = true;

            AUDIO_StartDMA();
        } catch (...) {
            shutdown();
            throw;
        }
    }

    ~Impl() { shutdown(); }

    void shutdown() noexcept {
        running.store(false, std::memory_order_release);
        AUDIO_StopDMA();
        AUDIO_RegisterDMACallback(nullptr);
        active.store(nullptr, std::memory_order_release);
        if (worker_started) {
            LWP_SemPost(wake_sem);
            LWP_JoinThread(worker, nullptr);
            worker_started = false;
        }
        if (semaphore_initialized) {
            LWP_SemDestroy(wake_sem);
            semaphore_initialized = false;
        }
    }

    static u32 address(const void *pointer) noexcept {
        return static_cast<u32>(reinterpret_cast<std::uintptr_t>(pointer));
    }

    static void *worker_entry(void *context) noexcept {
        auto &self = *static_cast<Impl *>(context);
        while (self.running.load(std::memory_order_acquire)) {
            if (LWP_SemWait(self.wake_sem) < 0)
                continue;
            if (!self.running.load(std::memory_order_acquire))
                break;
            self.fill_one();
        }
        return nullptr;
    }

    static void dma_callback() noexcept {
        if (auto *self = active.load(std::memory_order_acquire))
            self->on_dma_complete();
    }

    int take_ready_buffer() noexcept {
        for (std::size_t offset = 0; offset < buffer_count; ++offset) {
            const auto index = (next_play_buffer + offset) % buffer_count;
            auto expected = BufferState::ready;
            if (states[index].compare_exchange_strong(expected, BufferState::playing,
                                                      std::memory_order_acq_rel)) {
                next_play_buffer = (index + 1) % buffer_count;
                return static_cast<int>(index);
            }
        }
        return -1;
    }

    void on_dma_complete() noexcept {
        const int finished = playing.exchange(-1, std::memory_order_relaxed);
        if (finished >= 0)
            states[static_cast<std::size_t>(finished)].store(BufferState::free, std::memory_order_release);

        const int next = take_ready_buffer();
        if (next >= 0) {
            playing.store(next, std::memory_order_relaxed);
            AUDIO_InitDMA(address(buffers[static_cast<std::size_t>(next)].samples.data()),
                          static_cast<u32>(dma_bytes));
        } else {
            AUDIO_InitDMA(address(silence.data()), static_cast<u32>(dma_bytes));
        }
        LWP_SemPost(wake_sem);
    }

    void fill_one() noexcept {
        for (std::size_t offset = 0; offset < buffer_count; ++offset) {
            const auto index = (next_fill_buffer + offset) % buffer_count;
            auto expected = BufferState::free;
            if (!states[index].compare_exchange_strong(expected, BufferState::filling,
                                                       std::memory_order_acq_rel))
                continue;

            session.mix_audio(mix_buffer, playback_rate.load(std::memory_order_relaxed));
            auto &output = buffers[index].samples;
            std::transform(mix_buffer.begin(), mix_buffer.end(), output.begin(), [](float sample) {
                return static_cast<std::int16_t>(std::clamp(sample, -1.0f, 1.0f) * 32767.0f);
            });
            DCFlushRange(output.data(), static_cast<u32>(dma_bytes));
            states[index].store(BufferState::ready, std::memory_order_release);
            next_fill_buffer = (index + 1) % buffer_count;
            return;
        }
    }
};

std::atomic<WiiAudioOutput::Impl *> WiiAudioOutput::Impl::active{nullptr};

WiiAudioOutput::WiiAudioOutput(GameSession &session, double speed)
    : impl_(std::make_unique<Impl>(session, speed)) {}
WiiAudioOutput::~WiiAudioOutput() = default;
void WiiAudioOutput::set_speed(double speed) noexcept {
    impl_->playback_rate.store(static_cast<float>(speed), std::memory_order_relaxed);
}
void WiiAudioOutput::pump() {}
} // namespace regml
