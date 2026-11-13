#include "video_player.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#ifndef REGML_PS2
#define REGML_PS2 0
#endif
#if !REGML_PS2
#include <mutex>
#endif
#include <vector>

#ifndef REGML_WITH_FFMPEG
#define REGML_WITH_FFMPEG 0
#endif

#if REGML_WITH_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/version.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/version.h>
#include <libswresample/swresample.h>
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

#if REGML_WITH_FFMPEG
constexpr int video_audio_rate = 48000;
constexpr std::size_t max_video_audio_frames = static_cast<std::size_t>(video_audio_rate) * 60 * 10;

struct AudioDecodeResources {
    AVFormatContext *format = nullptr;
    AVCodecContext *decoder = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    SwrContext *resampler = nullptr;

    ~AudioDecodeResources() {
        swr_free(&resampler);
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
    }
};

std::vector<float> decode_audio_track(const std::filesystem::path &path) {
    AudioDecodeResources resources;
    const auto filename = path.string();
    if (avformat_open_input(&resources.format, filename.c_str(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(resources.format, nullptr) < 0)
        return {};

#if LIBAVCODEC_VERSION_MAJOR >= 59
    const AVCodec *codec = nullptr;
#else
    AVCodec *codec = nullptr;
#endif
    const int stream_index =
        av_find_best_stream(resources.format, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (stream_index < 0 || !codec)
        return {};
    auto *stream = resources.format->streams[stream_index];
    resources.decoder = avcodec_alloc_context3(codec);
    if (!resources.decoder || avcodec_parameters_to_context(resources.decoder, stream->codecpar) < 0 ||
        avcodec_open2(resources.decoder, codec, nullptr) < 0 || resources.decoder->sample_rate <= 0)
        return {};

#if LIBAVUTIL_VERSION_MAJOR >= 57
    AVChannelLayout output_layout = AV_CHANNEL_LAYOUT_STEREO;
    AVChannelLayout input_layout{};
    if (resources.decoder->ch_layout.nb_channels > 0) {
        if (av_channel_layout_copy(&input_layout, &resources.decoder->ch_layout) < 0)
            return {};
    } else {
        av_channel_layout_default(&input_layout, 2);
    }
    const int resampler_result = swr_alloc_set_opts2(&resources.resampler, &output_layout, AV_SAMPLE_FMT_FLT,
                                                    video_audio_rate, &input_layout,
                                                    resources.decoder->sample_fmt,
                                                    resources.decoder->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&input_layout);
    if (resampler_result < 0 || !resources.resampler || swr_init(resources.resampler) < 0)
        return {};
#else
    std::int64_t input_layout = resources.decoder->channel_layout;
    if (!input_layout)
        input_layout = av_get_default_channel_layout(std::max(resources.decoder->channels, 1));
    resources.resampler = swr_alloc_set_opts(resources.resampler, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_FLT,
                                             video_audio_rate, input_layout, resources.decoder->sample_fmt,
                                             resources.decoder->sample_rate, 0, nullptr);
    if (!resources.resampler || swr_init(resources.resampler) < 0)
        return {};
#endif

    resources.packet = av_packet_alloc();
    resources.frame = av_frame_alloc();
    if (!resources.packet || !resources.frame)
        return {};

    std::vector<float> samples;
    const auto append_frame = [&]() -> bool {
        if (resources.frame->nb_samples <= 0)
            return true;
        const auto delayed = swr_get_delay(resources.resampler, resources.decoder->sample_rate);
        const auto capacity = av_rescale_rnd(delayed + resources.frame->nb_samples, video_audio_rate,
                                             resources.decoder->sample_rate, AV_ROUND_UP);
        const auto current_frames = samples.size() / 2;
        if (capacity <= 0 || static_cast<std::uint64_t>(capacity) > max_video_audio_frames - current_frames)
            return false;
        const auto old_size = samples.size();
        samples.resize(old_size + static_cast<std::size_t>(capacity) * 2);
        std::uint8_t *output[] = {reinterpret_cast<std::uint8_t *>(samples.data() + old_size)};
        const int converted = swr_convert(resources.resampler, output, static_cast<int>(capacity),
                                          const_cast<const std::uint8_t **>(resources.frame->extended_data),
                                          resources.frame->nb_samples);
        if (converted < 0) {
            samples.resize(old_size);
            return false;
        }
        samples.resize(old_size + static_cast<std::size_t>(converted) * 2);
        return true;
    };
    const auto drain_decoder = [&]() -> int {
        for (;;) {
            const int result = avcodec_receive_frame(resources.decoder, resources.frame);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
                return 0;
            if (result < 0)
                return -1;
            const bool converted = append_frame();
            av_frame_unref(resources.frame);
            if (!converted)
                return -1;
        }
    };

    for (;;) {
        const int read_result = av_read_frame(resources.format, resources.packet);
        if (read_result < 0)
            break;
        if (resources.packet->stream_index == stream_index) {
            int sent = avcodec_send_packet(resources.decoder, resources.packet);
            if (sent == AVERROR(EAGAIN)) {
                if (drain_decoder() < 0) {
                    av_packet_unref(resources.packet);
                    return {};
                }
                sent = avcodec_send_packet(resources.decoder, resources.packet);
            }
            av_packet_unref(resources.packet);
            if ((sent < 0 && sent != AVERROR_EOF) || drain_decoder() < 0)
                return {};
        } else {
            av_packet_unref(resources.packet);
        }
    }
    const int flush_result = avcodec_send_packet(resources.decoder, nullptr);
    if ((flush_result < 0 && flush_result != AVERROR_EOF && flush_result != AVERROR(EAGAIN)) ||
        drain_decoder() < 0)
        return {};

    for (;;) {
        const auto delayed = swr_get_delay(resources.resampler, video_audio_rate);
        if (delayed <= 0)
            break;
        const auto current_frames = samples.size() / 2;
        if (static_cast<std::uint64_t>(delayed) > max_video_audio_frames - current_frames)
            return {};
        const auto old_size = samples.size();
        samples.resize(old_size + static_cast<std::size_t>(delayed) * 2);
        std::uint8_t *output[] = {reinterpret_cast<std::uint8_t *>(samples.data() + old_size)};
        const int converted = swr_convert(resources.resampler, output, static_cast<int>(delayed), nullptr, 0);
        if (converted < 0) {
            samples.resize(old_size);
            return {};
        }
        samples.resize(old_size + static_cast<std::size_t>(converted) * 2);
        if (converted == 0)
            break;
    }
    return samples;
}
#endif
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
#if !REGML_PS2
    mutable std::mutex audio_mutex;
#endif
    std::vector<float> audio_samples;
    double audio_frame = 0;
    double audio_volume = 1.0;
    bool audio_paused = false;
    bool audio_looping = false;

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
                    frame_position = std::max(0.0, static_cast<double>(timestamp - timestamp_origin) *
                                                       av_q2d(stream->time_base) * 1000.0);
                } else {
                    frame_position = fallback_frame_position;
                }
                fallback_frame_position = frame_position + frame_interval;
                scaler = sws_getCachedContext(scaler, frame->width, frame->height,
                                              static_cast<AVPixelFormat>(frame->format), width, height,
                                              AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (!scaler) {
                    av_frame_unref(frame);
                    error = true;
                    return false;
                }
                auto *destination = reinterpret_cast<std::uint8_t *>(rgba.data());
                int destination_stride = width * static_cast<int>(sizeof(std::uint32_t));
                const int scaled = sws_scale(scaler, frame->data, frame->linesize, 0, frame->height,
                                             &destination, &destination_stride);
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
VideoPlayer::~VideoPlayer() {
    close();
}

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
#if LIBAVCODEC_VERSION_MAJOR >= 59
    const AVCodec *codec = nullptr;
#else
    AVCodec *codec = nullptr;
#endif
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
        static_cast<std::uint64_t>(state.width) * static_cast<std::uint64_t>(state.height) >
            16ULL * 1024 * 1024) {
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
        state.duration =
            static_cast<double>(state.stream->duration) * av_q2d(state.stream->time_base) * 1000.0;
    else if (state.format->duration != AV_NOPTS_VALUE && state.format->duration > 0)
        state.duration = static_cast<double>(state.format->duration) * 1000.0 / AV_TIME_BASE;
    const AVRational rate = av_guess_frame_rate(state.format, state.stream, nullptr);
    if (rate.num > 0 && rate.den > 0)
        state.frame_interval = 1000.0 * rate.den / rate.num;
    state.open = true;
    state.playback_start = Clock::now();
    try {
        auto decoded_audio = decode_audio_track(path);
#if !REGML_PS2
        std::lock_guard lock(state.audio_mutex);
#endif
        state.audio_samples = std::move(decoded_audio);
        state.audio_frame = 0;
        state.audio_paused = false;
        state.audio_looping = false;
    } catch (...) {
        // Video playback remains useful when a container has an unsupported
        // or malformed audio stream, or when allocating its PCM buffer fails.
    }
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
#if !REGML_PS2
    {
        std::lock_guard lock(impl_->audio_mutex);
#endif
        impl_->audio_samples.clear();
        impl_->audio_frame = 0;
        impl_->audio_paused = false;
        impl_->audio_looping = false;
#if !REGML_PS2
    }
#endif
}

void VideoPlayer::pause() noexcept {
    if (impl_->open && !impl_->paused && !impl_->finished) {
        impl_->paused = true;
        impl_->paused_at = Clock::now();
#if !REGML_PS2
        std::lock_guard lock(impl_->audio_mutex);
#endif
        impl_->audio_paused = true;
    }
}

void VideoPlayer::resume() noexcept {
    if (impl_->open && impl_->paused) {
        impl_->paused_total += Clock::now() - impl_->paused_at;
        impl_->paused = false;
#if !REGML_PS2
        std::lock_guard lock(impl_->audio_mutex);
#endif
        impl_->audio_paused = false;
    }
}

void VideoPlayer::enable_loop(bool enabled) noexcept {
    impl_->looping = enabled;
#if !REGML_PS2
    std::lock_guard lock(impl_->audio_mutex);
#endif
    impl_->audio_looping = enabled;
}

void VideoPlayer::set_volume(double volume) noexcept {
    const auto clamped = std::isfinite(volume) ? std::clamp(volume, 0.0, 1.0) : 0.0;
#if !REGML_PS2
    std::lock_guard lock(impl_->audio_mutex);
#endif
    impl_->audio_volume = clamped;
}

double VideoPlayer::volume() const noexcept {
#if !REGML_PS2
    std::lock_guard lock(impl_->audio_mutex);
#endif
    return impl_->audio_volume;
}

void VideoPlayer::mix_audio(std::span<float> output, double playback_rate) noexcept {
    if (output.size() % 2 != 0 || !std::isfinite(playback_rate) || playback_rate <= 0)
        return;
#if !REGML_PS2
    std::lock_guard lock(impl_->audio_mutex);
#endif
    const auto frame_count = impl_->audio_samples.size() / 2;
    if (frame_count == 0 || impl_->audio_paused || impl_->audio_volume <= 0)
        return;

    for (std::size_t out_frame = 0; out_frame < output.size() / 2; ++out_frame) {
        if (impl_->audio_frame >= static_cast<double>(frame_count)) {
            if (!impl_->audio_looping)
                break;
            impl_->audio_frame = std::fmod(impl_->audio_frame, static_cast<double>(frame_count));
        }
        const auto source_frame = static_cast<std::size_t>(impl_->audio_frame);
        const auto next_frame = source_frame + 1 < frame_count ? source_frame + 1
                                                               : (impl_->audio_looping ? 0 : source_frame);
        const float fraction = static_cast<float>(impl_->audio_frame - source_frame);
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const float first = impl_->audio_samples[source_frame * 2 + channel];
            const float second = impl_->audio_samples[next_frame * 2 + channel];
            const float sample = first + (second - first) * fraction;
            output[out_frame * 2 + channel] += sample * static_cast<float>(impl_->audio_volume);
        }
        impl_->audio_frame += playback_rate;
    }
}

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

bool VideoPlayer::loaded() const noexcept {
    return impl_->open && !impl_->error;
}
bool VideoPlayer::failed() const noexcept {
    return impl_->error;
}
bool VideoPlayer::has_frame() const noexcept {
    return impl_->frame_ready;
}
int VideoPlayer::width() const noexcept {
    return impl_->width;
}
int VideoPlayer::height() const noexcept {
    return impl_->height;
}
int VideoPlayer::status() const noexcept {
    if (!loaded() || impl_->finished)
        return status_closed;
    if (impl_->paused)
        return status_paused;
    if (!impl_->frame_ready)
        return status_preparing;
    return status_playing;
}
double VideoPlayer::duration_ms() const noexcept {
    return impl_->duration;
}
double VideoPlayer::position_ms() const noexcept {
    return impl_->position_ms();
}
std::span<const std::uint32_t> VideoPlayer::pixels() const noexcept {
    return impl_->rgba;
}

} // namespace regml
