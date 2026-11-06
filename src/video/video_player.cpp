#include "video_player.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <vector>

#ifndef REGML_WITH_FFMPEG
#define REGML_WITH_FFMPEG 0
#endif

#if REGML_WITH_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
}
#endif

namespace regml {
namespace {
using Clock = std::chrono::steady_clock;
constexpr int status_closed = 0;
constexpr int status_preparing = 1;
constexpr int status_playing = 2;
constexpr int status_paused = 3;
} // namespace

struct VideoPlayer::Impl {
    bool open = false;
    bool error = false;
    bool paused = false;
    bool looping = false;
    bool finished = false;
    bool frame_ready = false;
    int width = 0;
    int height = 0;
    double duration = 0;
    double frame_position = 0;
    double frame_interval = 1000.0 / 30.0;
    std::vector<std::uint32_t> rgba;
    Clock::time_point playback_start{};
    Clock::time_point paused_at{};
    Clock::duration paused_total{};

#if REGML_WITH_FFMPEG
    AVFormatContext *format = nullptr;
    AVCodecContext *decoder = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    SwsContext *scaler = nullptr;
    AVStream *stream = nullptr;
    int stream_index = -1;
    bool flush_sent = false;
    std::int64_t timestamp_origin = AV_NOPTS_VALUE;
    double fallback_frame_position = 0;
#endif

    void free_decoder() noexcept {
#if REGML_WITH_FFMPEG
        sws_freeContext(scaler);
        scaler = nullptr;
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
        stream = nullptr;
        stream_index = -1;
        flush_sent = false;
        timestamp_origin = AV_NOPTS_VALUE;
        fallback_frame_position = 0;
#endif
    }

#if REGML_WITH_FFMPEG
    bool decode_next() noexcept {
        if (!decoder || !format || !frame || !packet || !stream)
            return false;
        for (;;) {
            int result = avcodec_receive_frame(decoder, frame);
            if (result == 0) {
                const auto timestamp = frame->best_effort_timestamp;
                if (timestamp != AV_NOPTS_VALUE) {
                    if (timestamp_origin == AV_NOPTS_VALUE)
                        timestamp_origin = timestamp;
                    frame_position = std::max(
                        0.0, static_cast<double>(timestamp - timestamp_origin) *
                                 av_q2d(stream->time_base) * 1000.0);
                } else {
                    frame_position = fallback_frame_position;
                }
                fallback_frame_position = frame_position + frame_interval;
                scaler = sws_getCachedContext(scaler,
                                              frame->width,
                                              frame->height,
                                              static_cast<AVPixelFormat>(frame->format),
                                              width,
                                              height,
                                              AV_PIX_FMT_BGRA,
                                              SWS_BILINEAR,
                                              nullptr,
                                              nullptr,
                                              nullptr);
                if (!scaler) {
                    av_frame_unref(frame);
                    error = true;
                    return false;
                }
                auto *destination = reinterpret_cast<std::uint8_t *>(rgba.data());
                int destination_stride = width * static_cast<int>(sizeof(std::uint32_t));
                const int scaled = sws_scale(scaler,
                                             frame->data,
                                             frame->linesize,
                                             0,
                                             frame->height,
                                             &destination,
                                             &destination_stride);
                av_frame_unref(frame);
                if (scaled != height) {
                    error = true;
                    return false;
                }
                frame_ready = true;
                return true;
            }
            if (result == AVERROR_EOF)
                return false;
            if (result != AVERROR(EAGAIN)) {
                error = true;
                return false;
            }
            if (flush_sent)
                return false;

            result = av_read_frame(format, packet);
            if (result < 0) {
                const int sent = avcodec_send_packet(decoder, nullptr);
                flush_sent = true;
                if (sent < 0 && sent != AVERROR_EOF) {
                    error = true;
                    return false;
                }
                continue;
            }
            if (packet->stream_index != stream_index) {
                av_packet_unref(packet);
                continue;
            }
            result = avcodec_send_packet(decoder, packet);
            av_packet_unref(packet);
            if (result < 0 && result != AVERROR(EAGAIN)) {
                error = true;
                return false;
            }
        }
    }

    bool restart() noexcept {
        if (!format || !decoder || !stream)
            return false;
        const std::int64_t start = stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
        if (av_seek_frame(format, stream_index, start, AVSEEK_FLAG_BACKWARD) < 0) {
            error = true;
            return false;
        }
        avcodec_flush_buffers(decoder);
        flush_sent = false;
        timestamp_origin = AV_NOPTS_VALUE;
        fallback_frame_position = 0;
        frame_position = 0;
        frame_ready = false;
        finished = false;
        paused_total = Clock::duration{};
        playback_start = Clock::now();
        return true;
    }
#endif

    double position_ms() const noexcept {
        if (!open)
            return 0;
        if (finished)
            return duration;
        const auto now = paused ? paused_at : Clock::now();
        const auto elapsed = now - playback_start - paused_total;
        double position = std::max(0.0, std::chrono::duration<double, std::milli>(elapsed).count());
        if (duration > 0) {
            if (looping)
                position = std::fmod(position, duration);
            else
                position = std::min(position, duration);
        }
        return position;
    }
};

VideoPlayer::VideoPlayer() : impl_(std::make_unique<Impl>()) {}
VideoPlayer::~VideoPlayer() { close(); }

bool VideoPlayer::open(const std::filesystem::path &path) {
    close();
#if REGML_WITH_FFMPEG
    auto &state = *impl_;
    const auto filename = path.string();
    if (avformat_open_input(&state.format, filename.c_str(), nullptr, nullptr) < 0)
        return false;
    if (avformat_find_stream_info(state.format, nullptr) < 0) {
        close();
        return false;
    }
    const AVCodec *codec = nullptr;
    state.stream_index = av_find_best_stream(state.format, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (state.stream_index < 0 || !codec) {
        close();
        return false;
    }
    state.stream = state.format->streams[state.stream_index];
    state.decoder = avcodec_alloc_context3(codec);
    if (!state.decoder || avcodec_parameters_to_context(state.decoder, state.stream->codecpar) < 0 ||
        avcodec_open2(state.decoder, codec, nullptr) < 0) {
        close();
        return false;
    }
    state.width = state.decoder->width;
    state.height = state.decoder->height;
    if (state.width < 1 || state.height < 1 || state.width > 4096 || state.height > 4096 ||
        static_cast<std::uint64_t>(state.width) * static_cast<std::uint64_t>(state.height) > 16ULL * 1024 * 1024) {
        close();
        return false;
    }
    state.packet = av_packet_alloc();
    state.frame = av_frame_alloc();
    if (!state.packet || !state.frame) {
        close();
        return false;
    }
    state.rgba.resize(static_cast<std::size_t>(state.width) * static_cast<std::size_t>(state.height));
    if (state.stream->duration != AV_NOPTS_VALUE && state.stream->duration > 0)
        state.duration = static_cast<double>(state.stream->duration) * av_q2d(state.stream->time_base) * 1000.0;
    else if (state.format->duration != AV_NOPTS_VALUE && state.format->duration > 0)
        state.duration = static_cast<double>(state.format->duration) * 1000.0 / AV_TIME_BASE;
    const AVRational rate = av_guess_frame_rate(state.format, state.stream, nullptr);
    if (rate.num > 0 && rate.den > 0)
        state.frame_interval = 1000.0 * rate.den / rate.num;
    state.open = true;
    state.playback_start = Clock::now();
    return true;
#else
    (void)path;
    return false;
#endif
}

void VideoPlayer::close() noexcept {
    if (!impl_)
        return;
    impl_->free_decoder();
    impl_->open = false;
    impl_->error = false;
    impl_->paused = false;
    impl_->looping = false;
    impl_->finished = false;
    impl_->frame_ready = false;
    impl_->width = 0;
    impl_->height = 0;
    impl_->duration = 0;
    impl_->frame_position = 0;
    impl_->rgba.clear();
    impl_->paused_total = Clock::duration{};
}

void VideoPlayer::pause() noexcept {
    if (impl_->open && !impl_->paused && !impl_->finished) {
        impl_->paused = true;
        impl_->paused_at = Clock::now();
    }
}

void VideoPlayer::resume() noexcept {
    if (impl_->open && impl_->paused) {
        impl_->paused_total += Clock::now() - impl_->paused_at;
        impl_->paused = false;
    }
}

void VideoPlayer::enable_loop(bool enabled) noexcept { impl_->looping = enabled; }

bool VideoPlayer::update_frame() noexcept {
    auto &state = *impl_;
#if REGML_WITH_FFMPEG
    if (!state.open || state.error || state.paused || state.finished)
        return false;
    const double position = state.position_ms();
    if (state.duration > 0 && position >= state.duration) {
        if (state.looping) {
            if (!state.restart())
                return false;
        } else {
            state.finished = true;
            return false;
        }
    }
    bool updated = false;
    for (int decoded = 0; decoded < 8; ++decoded) {
        if (state.frame_ready && state.frame_position + state.frame_interval > position)
            break;
        if (!state.decode_next()) {
            if (state.error)
                return updated;
            if (state.looping) {
                if (!state.restart())
                    return updated;
                continue;
            }
            state.finished = true;
            break;
        }
        updated = true;
    }
    return updated;
#else
    (void)state;
    return false;
#endif
}

bool VideoPlayer::loaded() const noexcept { return impl_->open && !impl_->error; }
bool VideoPlayer::failed() const noexcept { return impl_->error; }
bool VideoPlayer::has_frame() const noexcept { return impl_->frame_ready; }
int VideoPlayer::width() const noexcept { return impl_->width; }
int VideoPlayer::height() const noexcept { return impl_->height; }
int VideoPlayer::status() const noexcept {
    if (!loaded() || impl_->finished)
        return status_closed;
    if (impl_->paused)
        return status_paused;
    if (!impl_->frame_ready)
        return status_preparing;
    return status_playing;
}
double VideoPlayer::duration_ms() const noexcept { return impl_->duration; }
double VideoPlayer::position_ms() const noexcept { return impl_->position_ms(); }
std::span<const std::uint32_t> VideoPlayer::pixels() const noexcept { return impl_->rgba; }

} // namespace regml
