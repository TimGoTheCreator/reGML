#include "output.hpp"
#include "error.hpp"
#include "game.hpp"
#include <audsrv.h>
#include <kernel.h>
#include <algorithm>
#include <cstdint>
#include <string>

namespace regml {
namespace {
constexpr int kAudioBytes = static_cast<int>(ps2_audio_frames * 2 * sizeof(std::int16_t));
} // namespace

Ps2AudioOutput::Ps2AudioOutput(GameSession &session) : session_(session) {
    if (audsrv_init() != AUDSRV_ERR_NOERROR)
        throw Error(ErrorKind::io, std::string("PS2 audio initialization failed: ") + audsrv_get_error_string());

    audsrv_fmt_t format{};
    format.freq = 48000;
    format.bits = 16;
    format.channels = 2;
    if (audsrv_set_format(&format) != AUDSRV_ERR_NOERROR) {
        const std::string message = std::string("PS2 audio format setup failed: ") + audsrv_get_error_string();
        audsrv_quit();
        throw Error(ErrorKind::io, message);
    }
    audsrv_set_volume(MAX_VOLUME);

    ee_sema_t semaphore{};
    semaphore.init_count = 0;
    semaphore.max_count = 1;
    finished_sema_ = CreateSema(&semaphore);
    if (finished_sema_ < 0) {
        audsrv_quit();
        throw Error(ErrorKind::io, "PS2 audio thread semaphore creation failed");
    }

    ee_thread_t thread{};
    thread.func = reinterpret_cast<void *>(worker_entry);
    thread.stack = thread_stack_.data();
    thread.stack_size = static_cast<int>(thread_stack_.size());
    thread.gp_reg = _gp;
    thread.initial_priority = 1;
    thread_id_ = CreateThread(&thread);
    if (thread_id_ < 0) {
        DeleteSema(finished_sema_);
        finished_sema_ = -1;
        audsrv_quit();
        throw Error(ErrorKind::io, "PS2 audio thread creation failed");
    }

    running_.store(true, std::memory_order_release);
    if (StartThread(thread_id_, this) < 0) {
        running_.store(false, std::memory_order_release);
        DeleteThread(thread_id_);
        thread_id_ = -1;
        DeleteSema(finished_sema_);
        finished_sema_ = -1;
        audsrv_quit();
        throw Error(ErrorKind::io, "PS2 audio thread start failed");
    }
}

Ps2AudioOutput::~Ps2AudioOutput() {
    running_.store(false, std::memory_order_release);
    if (thread_id_ >= 0) {
        WaitSema(finished_sema_);
        ee_thread_status_t status{};
        while (ReferThreadStatus(thread_id_, &status) >= 0 && status.status != THS_DORMANT) {
        }
        DeleteThread(thread_id_);
    }
    if (finished_sema_ >= 0)
        DeleteSema(finished_sema_);
    audsrv_stop_audio();
    audsrv_quit();
}

void Ps2AudioOutput::worker_entry(void *context) {
    auto &output = *static_cast<Ps2AudioOutput *>(context);
    output.run();
    SignalSema(output.finished_sema_);
    ExitThread();
}

void Ps2AudioOutput::run() {
    while (running_.load(std::memory_order_acquire)) {
        if (audsrv_wait_audio(kAudioBytes) != AUDSRV_ERR_NOERROR)
            break;
        if (!running_.load(std::memory_order_acquire))
            break;

        session_.mix_audio(mixed_);
        for (std::size_t i = 0; i < mixed_.size(); ++i) {
            const auto sample = std::clamp(mixed_[i], -1.0f, 1.0f);
            pcm_[i] = static_cast<std::int16_t>(sample < 0 ? sample * 32768.0f : sample * 32767.0f);
        }
        if (audsrv_play_audio(reinterpret_cast<const char *>(pcm_.data()), kAudioBytes) < 0)
            break;
    }
    running_.store(false, std::memory_order_release);
}
} // namespace regml
