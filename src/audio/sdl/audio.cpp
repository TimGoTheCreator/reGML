#include "audio.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vorbis/vorbisfile.h>
#if REGML_WIIU
#include "platform/wiiu/audio_async.hpp"
#endif
namespace regml {
namespace {
std::optional<std::filesystem::path> find_audio_file(const std::filesystem::path &directory,
                                                     std::string_view filename) {
    const std::filesystem::path relative_path(filename);
    std::vector<std::filesystem::path> names{relative_path};
    if (relative_path.extension().empty())
        for (const auto extension : {".ogg", ".wav", ".mp3"})
            names.emplace_back(relative_path.string() + extension);
    const auto search = [&](const std::filesystem::path &base) -> std::optional<std::filesystem::path> {
        for (const auto &name : names) {
            const auto direct = base / name;
            if (std::filesystem::is_regular_file(direct))
                return direct;
            const auto exported = base / "Exported_Sounds" / name;
            if (std::filesystem::is_regular_file(exported))
                return exported;
        }
        return {};
    };
    const auto chapter_root = directory.parent_path();
    for (auto base = directory; !base.empty();) {
        if (auto found = search(base))
            return found;
        if (base == chapter_root) {
            std::error_code error;
            for (std::filesystem::directory_iterator entry(base, error), end; !error && entry != end;
                 entry.increment(error)) {
                if (!entry->is_directory(error) ||
                    entry->path().filename().string().find("_windows") == std::string::npos)
                    continue;
                if (auto found = search(entry->path()))
                    return found;
            }
        }
        const auto parent = base.parent_path();
        if (parent == base)
            break;
        base = parent;
    }
    return {};
}
} // namespace
struct GameAudio::Stream {
    std::span<const std::uint8_t> bytes;
    std::size_t position = 0, begin = 0, count = 0;
    long rate = 0;
    int channels = 0;
    std::ifstream file_stream;
    const Package *package = nullptr;
    std::size_t package_offset = 0, package_length = 0;
    OggVorbis_File ogg_file{};
    std::vector<float> samples;
    explicit Stream(const Clip &clip) : bytes(clip.compressed), samples(8192) {
        package = clip.package;
        package_offset = clip.package_offset;
        package_length = clip.package_length;
        if (!clip.file_path.empty()) {
            file_stream.open(clip.file_path, std::ios::binary);
            if (!file_stream)
                throw Error(ErrorKind::io, "streaming Ogg file could not be opened");
        }
        ov_callbacks callbacks{};
        callbacks.read_func = [](void *ptr, std::size_t size, std::size_t count, void *context) {
            auto &stream = *static_cast<Stream *>(context);
            if (stream.file_stream.is_open()) {
                if (!size)
                    return std::size_t{0};
                stream.file_stream.read(static_cast<char *>(ptr), static_cast<std::streamsize>(size * count));
                return static_cast<std::size_t>(stream.file_stream.gcount()) / size;
            }
            const auto available = stream.package ? stream.package_length - stream.position
                                                  : stream.bytes.size() - stream.position;
            const auto items = size ? std::min(count, available / size) : 0;
            if (stream.package) {
                try {
                    stream.package->read_into(stream.package_offset + stream.position,
                                              {static_cast<std::uint8_t *>(ptr), items * size});
                } catch (...) {
                    return std::size_t{0};
                }
            } else {
                std::memcpy(ptr, stream.bytes.data() + stream.position, items * size);
            }
            stream.position += items * size;
            return items;
        };
        callbacks.seek_func = [](void *context, ogg_int64_t offset, int origin) {
            auto &stream = *static_cast<Stream *>(context);
            if (stream.file_stream.is_open()) {
                const auto direction = origin == SEEK_SET   ? std::ios::beg
                                       : origin == SEEK_CUR ? std::ios::cur
                                                            : std::ios::end;
                stream.file_stream.clear();
                stream.file_stream.seekg(static_cast<std::streamoff>(offset), direction);
                return stream.file_stream ? 0 : -1;
            }
            const auto length = stream.package ? stream.package_length : stream.bytes.size();
            const auto base = origin == SEEK_SET   ? 0
                              : origin == SEEK_CUR ? static_cast<ogg_int64_t>(stream.position)
                                                   : static_cast<ogg_int64_t>(length);
            const auto target = base + offset;
            if (target < 0 || static_cast<std::uint64_t>(target) > length)
                return -1;
            stream.position = static_cast<std::size_t>(target);
            return 0;
        };
        callbacks.tell_func = [](void *context) {
            auto &stream = *static_cast<Stream *>(context);
            if (stream.file_stream.is_open()) {
                const auto position = stream.file_stream.tellg();
                return position < 0 ? -1L : static_cast<long>(position);
            }
            return static_cast<long>(stream.position);
        };
        if (ov_open_callbacks(this, &ogg_file, nullptr, 0, callbacks) < 0)
            throw Error(ErrorKind::malformed, "streaming Ogg initialization failed");
        const auto *info = ov_info(&ogg_file, -1);
        rate = info->rate;
        channels = info->channels;
    }
    ~Stream() { ov_clear(&ogg_file); }
    bool read_frame(double output_frame, float (&out)[2]) noexcept {
        const double input = output_frame * static_cast<double>(rate) / 48000;
        const auto frame = static_cast<std::size_t>(input);
        if (frame < begin || frame >= begin + count) {
            const auto cursor = ov_pcm_tell(&ogg_file);
            if (cursor < 0)
                return false;
            if (static_cast<std::uint64_t>(cursor) != frame &&
                ov_pcm_seek(&ogg_file, static_cast<ogg_int64_t>(frame)) < 0)
                return false;
            int section = 0;
            float **pcm = nullptr;
            const auto read = ov_read_float(&ogg_file, &pcm, 4096, &section);
            if (read <= 0)
                return false;
            const auto *info = ov_info(&ogg_file, section);
            if (!info || info->channels != channels || info->rate != rate)
                return false;
            begin = frame;
            count = static_cast<std::size_t>(read);
            for (std::size_t i = 0; i < count; ++i)
                for (int c = 0; c < channels; ++c)
                    samples[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)] = pcm[c][i];
        }
        const auto a = frame - begin, b = std::min(a + 1, count - 1);
        const auto t = static_cast<float>(input - std::floor(input));
        for (std::size_t c = 0; c < 2; ++c) {
            const auto channel = std::min(c, static_cast<std::size_t>(channels - 1));
            out[c] = samples[a * static_cast<std::size_t>(channels) + channel] * (1 - t) +
                     samples[b * static_cast<std::size_t>(channels) + channel] * t;
        }
        return true;
    }
};
GameAudio::GameAudio(const Package &package, std::filesystem::path directory)
    : package_(package), directory_(std::move(directory)) {
#if REGML_WIIU
    async_ = std::make_unique<AsyncState>();
#endif
    auto pointers = [&](const Chunk &c) {
        if (c.size < 4)
            throw Error(ErrorKind::malformed, "audio table range", c.data_offset);
        const auto prefix = package.read_bytes(c.data_offset, 4);
        const auto count = BinaryView(prefix, c.data_offset).u32(0);
        if (count > 1'000'000 || count > (c.size - 4) / 4)
            throw Error(ErrorKind::malformed, "audio table range", c.data_offset);
        const auto table = package.read_bytes(c.data_offset, 4 + static_cast<std::size_t>(count) * 4);
        const BinaryView view(table, c.data_offset);
        std::vector<std::size_t> out;
        for (std::size_t i = 0; i < count; ++i) {
            const auto p = view.u32(4 + i * 4);
            if (p < c.data_offset || p >= c.data_offset + c.size)
                throw Error(ErrorKind::malformed, "audio pointer outside chunk", p);
            out.push_back(p);
        }
        return out;
    };
    if (const auto *c = package.find_chunk("AUDO"))
        embedded_ = pointers(*c);
    if (const auto *c = package.find_chunk("SOND")) {
        int id = 0;
        for (auto p : pointers(*c)) {
            if (c->data_offset + c->size - p < 36)
                throw Error(ErrorKind::malformed, "sound record range", p);
            const auto record = package.read_bytes(p, 36);
            const BinaryView d(record, p);
            Sound sound;
            sound.filename = package.string_pointer(d.u32(12));
            sound.gain = std::bit_cast<float>(d.u32(20));
            const auto group = d.i32(28), audio_id = d.i32(32);
            if (group == 0)
                sound.embedded = audio_id;
            sounds_.emplace(id++, std::move(sound));
        }
    }
}
GameAudio::~GameAudio() = default;
std::optional<int> GameAudio::find_sound_id(std::string_view filename) const {
    for (const auto &[id, sound] : sounds_)
        if (sound.filename == filename)
            return id;
    return std::nullopt;
}
std::unique_ptr<GameAudio::Clip> GameAudio::decode(std::vector<std::uint8_t> owned_bytes) {
    const std::span<const std::uint8_t> bytes(owned_bytes);
    std::vector<float> source;
    long rate = 0;
    int channels = 0;
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "OggS", 4) == 0) {
        struct Reader {
            std::span<const std::uint8_t> bytes;
            std::size_t position = 0;
        } reader{bytes};
        ov_callbacks callbacks{};
        callbacks.read_func = [](void *ptr, std::size_t size, std::size_t count,
                                 void *context) -> std::size_t {
            auto &r = *static_cast<Reader *>(context);
            if (size == 0)
                return 0;
            const auto items = std::min(count, (r.bytes.size() - r.position) / size);
            std::memcpy(ptr, r.bytes.data() + r.position, items * size);
            r.position += items * size;
            return items;
        };
        callbacks.seek_func = [](void *context, ogg_int64_t offset, int origin) -> int {
            auto &r = *static_cast<Reader *>(context);
            const auto base = origin == SEEK_SET   ? 0
                              : origin == SEEK_CUR ? static_cast<ogg_int64_t>(r.position)
                                                   : static_cast<ogg_int64_t>(r.bytes.size());
            const auto target = base + offset;
            if (target < 0 || static_cast<std::uint64_t>(target) > r.bytes.size())
                return -1;
            r.position = static_cast<std::size_t>(target);
            return 0;
        };
        callbacks.tell_func = [](void *context) -> long {
            return static_cast<long>(static_cast<Reader *>(context)->position);
        };
        OggVorbis_File file{};
        if (ov_open_callbacks(&reader, &file, nullptr, 0, callbacks) < 0)
            throw Error(ErrorKind::malformed, "Ogg audio decode initialization failed");
        struct Guard {
            OggVorbis_File &f;
            ~Guard() { ov_clear(&f); }
        } guard{file};
        const auto *info = ov_info(&file, -1);
        rate = info->rate;
        channels = info->channels;
        if (channels < 1 || channels > 2 || rate < 1 || rate > 192000)
            throw Error(ErrorKind::unsupported, "unsupported Ogg channel/rate profile");
        const auto total = ov_pcm_total(&file, -1);
        if (total <= 0 || total > 100'000'000 / channels)
            throw Error(ErrorKind::unsupported, "Ogg duration exceeds clip limit");
#if REGML_WII
        constexpr std::uint64_t decoded_audio_limit = 128 * 1024;
#else
        constexpr std::uint64_t decoded_audio_limit = 1024 * 1024;
#endif
        if (static_cast<std::uint64_t>(total) * static_cast<unsigned>(channels) * sizeof(float) >
            decoded_audio_limit) {
            auto clip = std::make_unique<Clip>();
            clip->compressed = std::move(owned_bytes);
            clip->streamed_frames =
                static_cast<std::size_t>(static_cast<double>(total) * 48000 / static_cast<double>(rate));
            if (clip->streamed_frames > 50'000'000)
                throw Error(ErrorKind::unsupported, "resampled audio exceeds limit");
            return clip;
        }
        int section = 0;
        float **pcm = nullptr;
        while (true) {
            const long count = ov_read_float(&file, &pcm, 4096, &section);
            if (count == 0)
                break;
            if (count < 0)
                throw Error(ErrorKind::malformed, "Ogg decode failed");
            if (source.size() + static_cast<std::size_t>(count) * static_cast<std::size_t>(channels) >
                100'000'000)
                throw Error(ErrorKind::unsupported, "decoded audio exceeds clip limit");
            for (long i = 0; i < count; ++i)
                for (int c = 0; c < channels; ++c)
                    source.push_back(pcm[c][i]);
        }
    } else {
        const BinaryView d(bytes);
        if (d.text(0, 4) != "RIFF" || d.text(8, 4) != "WAVE")
            throw Error(ErrorKind::unsupported, "audio format requires WAV or Ogg Vorbis");
        int format = 0, bits = 0;
        std::span<const std::uint8_t> pcm;
        for (std::size_t at = 12; at + 8 <= d.size();) {
            const auto size = d.u32(at + 4);
            const auto block = d.slice(at + 8, size);
            if (d.text(at, 4) == "fmt ") {
                format = block.u16(0);
                channels = block.u16(2);
                const auto sample_rate = block.u32(4);
                if (sample_rate > 192000)
                    throw Error(ErrorKind::unsupported, "WAV sample rate exceeds limit");
                rate = static_cast<long>(sample_rate);
                bits = block.u16(14);
            } else if (d.text(at, 4) == "data")
                pcm = block.bytes();
            at += 8 + size + (size & 1);
        }
        if (channels < 1 || channels > 2 || rate < 1 || rate > 192000 || format != 1 ||
            (bits != 8 && bits != 16))
            throw Error(ErrorKind::unsupported, "unsupported WAV sample profile");
        const BinaryView samples(pcm);
        for (std::size_t at = 0; at + static_cast<std::size_t>(bits / 8) <= samples.size();
             at += static_cast<std::size_t>(bits / 8))
            source.push_back(bits == 16 ? static_cast<float>(samples.i16(at)) / 32768.0f
                                        : (static_cast<float>(samples.u8(at)) - 128) / 128);
    }
    auto clip = std::make_unique<Clip>();
    const auto count = source.size() / static_cast<std::size_t>(channels);
    if (count == 0)
        throw Error(ErrorKind::malformed, "empty audio clip");
    const auto frames =
        static_cast<std::size_t>(static_cast<double>(count) * 48000 / static_cast<double>(rate));
    if (frames > 50'000'000)
        throw Error(ErrorKind::unsupported, "resampled audio exceeds limit");
    clip->stereo.resize(frames * 2);
    for (std::size_t f = 0; f < frames; ++f) {
        const double input = static_cast<double>(f) * static_cast<double>(rate) / 48000;
        const auto a = std::min(static_cast<std::size_t>(input), count - 1), b = std::min(a + 1, count - 1);
        const float t = static_cast<float>(input - std::floor(input));
        for (std::size_t c = 0; c < 2; ++c) {
            const auto ch = std::min(c, static_cast<std::size_t>(channels - 1));
            clip->stereo[f * 2 + c] = source[a * static_cast<std::size_t>(channels) + ch] * (1 - t) +
                                      source[b * static_cast<std::size_t>(channels) + ch] * t;
        }
    }
    return clip;
}
std::vector<std::uint8_t> GameAudio::read_audio(int id) const {
    auto found = sounds_.find(id);
    if (found == sounds_.end())
        throw Error(ErrorKind::vm, "audio resource does not exist");
    const auto &sound = found->second;
    const auto path = directory_ / sound.filename;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (file) {
        const auto size = file.tellg();
        if (size < 0 || size > 100'000'000)
            throw Error(ErrorKind::unsupported, "audio file size exceeds limit");
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        file.seekg(0);
        file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file)
            throw Error(ErrorKind::io, "audio file read failed");
        return bytes;
    } else if (sound.embedded >= 0) {
        if (static_cast<std::size_t>(sound.embedded) >= embedded_.size())
            throw Error(ErrorKind::malformed, "embedded audio ID out of range");
        const auto at = embedded_[static_cast<std::size_t>(sound.embedded)];
        const auto *c = package_.find_chunk("AUDO");
        if (!c)
            throw Error(ErrorKind::malformed, "embedded audio chunk is missing", at);
        if (at < c->data_offset || at > c->data_offset + c->size || c->data_offset + c->size - at < 4)
            throw Error(ErrorKind::malformed, "embedded audio length out of range", at);
        const auto prefix = package_.read_bytes(at, 4);
        const auto size = BinaryView(prefix, at).u32(0);
        if (size > c->data_offset + c->size - at - 4)
            throw Error(ErrorKind::malformed, "embedded audio length out of range", at);
        return package_.read_bytes(at + 4, size);
    } else {
        throw Error(ErrorKind::io, "audio file missing: " + path.string());
    }
}
GameAudio::Clip *GameAudio::load(int id) {
    auto &sound = sounds_.at(id);
    if (sound.clip)
        return sound.clip.get();
    if (!sound.filename.empty())
        if (const auto path = find_audio_file(directory_, sound.filename))
            sound.filename = path->string();
#if REGML_WII
    if (!sound.filename.empty()) {
        const auto path = directory_ / sound.filename;
        std::ifstream input(path, std::ios::binary);
        char signature[4]{};
        input.read(signature, sizeof(signature));
        if (input && std::memcmp(signature, "OggS", sizeof(signature)) == 0) {
            OggVorbis_File file{};
            const auto path_text = path.string();
            if (ov_fopen(path_text.c_str(), &file) == 0) {
                const auto *info = ov_info(&file, -1);
                const auto total = ov_pcm_total(&file, -1);
                const bool valid = info && info->channels >= 1 && info->channels <= 2 && info->rate > 0 &&
                                   total > 0 && total <= 100'000'000 / info->channels;
                if (valid) {
                    auto clip = std::make_unique<Clip>();
                    clip->file_path = path_text;
                    clip->streamed_frames = static_cast<std::size_t>(static_cast<double>(total) * 48000 /
                                                                     static_cast<double>(info->rate));
                    clip->gain = sound.gain;
                    ov_clear(&file);
                    sound.clip = std::move(clip);
                    return sound.clip.get();
                }
                ov_clear(&file);
            }
        }
    }
#endif
    if (!sound.filename.empty()) {
        auto clip = std::make_unique<Clip>();
        clip->file_path = (directory_ / sound.filename).string();
        std::ifstream external(clip->file_path, std::ios::binary);
        char signature[4]{};
        external.read(signature, sizeof(signature));
        if (external && std::memcmp(signature, "OggS", sizeof(signature)) == 0) {
            Stream probe(*clip);
            const auto total = ov_pcm_total(&probe.ogg_file, -1);
            if (!probe.rate || probe.channels < 1 || probe.channels > 2 || total <= 0 ||
                total > 100'000'000 / probe.channels)
                throw Error(ErrorKind::unsupported, "Ogg duration exceeds clip limit");
            clip->streamed_frames = static_cast<std::size_t>(static_cast<double>(total) * 48000 /
                                                             static_cast<double>(probe.rate));
            if (clip->streamed_frames > 50'000'000)
                throw Error(ErrorKind::unsupported, "resampled audio exceeds limit");
            clip->gain = sound.gain;
            sound.clip = std::move(clip);
            return sound.clip.get();
        }
    }
    std::ifstream external_audio(directory_ / sound.filename, std::ios::binary);
    if (sound.embedded >= 0 && !external_audio) {
        if (static_cast<std::size_t>(sound.embedded) >= embedded_.size())
            throw Error(ErrorKind::malformed, "embedded audio ID out of range");
        const auto at = embedded_[static_cast<std::size_t>(sound.embedded)];
        const auto *chunk = package_.find_chunk("AUDO");
        if (!chunk || at < chunk->data_offset || at > chunk->data_offset + chunk->size ||
            chunk->data_offset + chunk->size - at < 4)
            throw Error(ErrorKind::malformed, "embedded audio length out of range", at);
        const auto prefix = package_.read_bytes(at, 4);
        const auto length = static_cast<std::size_t>(BinaryView(prefix, at).u32(0));
        if (length > chunk->data_offset + chunk->size - at - 4)
            throw Error(ErrorKind::malformed, "embedded audio length out of range", at);
        const auto signature = package_.read_bytes(at + 4, std::min<std::size_t>(length, 4));
        if (signature.size() == 4 && std::memcmp(signature.data(), "OggS", 4) == 0) {
            auto clip = std::make_unique<Clip>();
            clip->package = &package_;
            clip->package_offset = at + 4;
            clip->package_length = length;
            Stream probe(*clip);
            const auto total = ov_pcm_total(&probe.ogg_file, -1);
            if (!probe.rate || probe.channels < 1 || probe.channels > 2 || total <= 0 ||
                total > 100'000'000 / probe.channels)
                throw Error(ErrorKind::unsupported, "embedded Ogg duration exceeds clip limit", at);
            clip->streamed_frames = static_cast<std::size_t>(static_cast<double>(total) * 48000 /
                                                             static_cast<double>(probe.rate));
            if (clip->streamed_frames > 50'000'000)
                throw Error(ErrorKind::unsupported, "resampled audio exceeds limit", at);
            clip->gain = sound.gain;
            sound.clip = std::move(clip);
            return sound.clip.get();
        }
    }
    sound.clip = decode(read_audio(id));
    sound.clip->gain = sound.gain;
    return sound.clip.get();
}
int GameAudio::create_stream(std::string_view filename) {
    const int id = next_stream_++;
    sounds_[id] = {-1, std::string(filename), 1, {}};
    const auto path = find_audio_file(directory_, filename);
    if (!path) {
        sounds_[id].clip = std::make_unique<Clip>();
        sounds_[id].clip->silent = true;
        sounds_[id].clip->streamed_frames = 48'000 * 60 * 60;
        return id;
    }
    sounds_[id].filename = path->string();
    load(id);
    return id;
}
int GameAudio::play(int sound, bool loop, double priority) {
#if REGML_WIIU
    if (!sounds_.contains(sound))
        throw Error(ErrorKind::vm, "audio resource does not exist");
    auto *clip = sounds_.at(sound).clip.get();
#else
    auto *clip = load(sound);
#endif
    if (!clip || clip->frames() == 0)
        return -1;
    std::scoped_lock lock(voices_mutex_);
    if (voices_.size() >= static_cast<std::size_t>(channels_)) {
        auto lowest = std::min_element(voices_.begin(), voices_.end(), [](const auto &a, const auto &b) {
            return a.second.priority < b.second.priority;
        });
        if (lowest->second.priority > priority)
            return -1;
        voices_.erase(lowest);
    }
    const int id = next_voice_++;
    voices_[id] = {sound, clip, 0, 1, 1, priority, loop, false};
    if (clip && (!clip->compressed.empty() || !clip->file_path.empty() || clip->package))
        voices_[id].stream = std::make_shared<Stream>(*clip);
#if REGML_WIIU
    if (!clip && !async_->pending(sound)) {
        const auto &resource = sounds_.at(sound);
        const auto audio_path =
            find_audio_file(directory_, resource.filename).value_or(directory_ / resource.filename);
        std::size_t embedded_offset = 0, embedded_size = 0;
        bool has_embedded = false;
        if (resource.embedded >= 0) {
            if (static_cast<std::size_t>(resource.embedded) >= embedded_.size())
                throw Error(ErrorKind::malformed, "embedded audio ID out of range");
            const auto at = embedded_[static_cast<std::size_t>(resource.embedded)];
            const auto *chunk = package_.find_chunk("AUDO");
            if (!chunk || at < chunk->data_offset || at > chunk->data_offset + chunk->size ||
                chunk->data_offset + chunk->size - at < 4)
                throw Error(ErrorKind::malformed, "embedded audio length out of range", at);
            const auto prefix = package_.read_bytes(at, 4);
            const auto size = BinaryView(prefix, at).u32(0);
            if (size > chunk->data_offset + chunk->size - at - 4)
                throw Error(ErrorKind::malformed, "embedded audio length out of range", at);
            embedded_offset = at + 4;
            embedded_size = size;
            has_embedded = true;
        }
        try {
            async_->request(sound, audio_path, package_, embedded_offset, embedded_size, has_embedded);
        } catch (...) {
            voices_.erase(id);
            throw;
        }
    }
#endif
    return id;
}
void GameAudio::stop(int id) {
    std::scoped_lock lock(voices_mutex_);
    std::erase_if(voices_, [&](const auto &p) { return id == -1 || p.first == id || p.second.sound == id; });
}
void GameAudio::pause(int id, bool paused) {
    std::scoped_lock lock(voices_mutex_);
    for (auto &[voice, v] : voices_)
        if (id == -1 || voice == id || v.sound == id)
            v.paused = paused;
}
bool GameAudio::playing(int id) const {
    std::scoped_lock lock(voices_mutex_);
    for (const auto &[voice, v] : voices_)
        if ((voice == id || v.sound == id) && !v.paused)
            return true;
    return false;
}
double GameAudio::gain(int id) {
    std::scoped_lock lock(voices_mutex_);
    if (id < 0)
        return 1.0;
    if (const auto it = voices_.find(id); it != voices_.end())
        return it->second.gain;
    if (const auto it = sounds_.find(id); it != sounds_.end())
        return it->second.gain;
    return load(id)->gain;
}
void GameAudio::gain(int id, double value) {
    if (!std::isfinite(value))
        throw Error(ErrorKind::vm, "nonfinite audio gain");
    if (id < 0)
        return;
    std::scoped_lock lock(voices_mutex_);
    if (auto it = voices_.find(id); it != voices_.end())
        it->second.gain = value;
    else if (auto it = sounds_.find(id); it != sounds_.end()) {
        it->second.gain = value;
        if (it->second.clip)
            it->second.clip->gain = value;
    } else
        load(id)->gain = value;
}
void GameAudio::pitch(int id, double value) {
    if (!std::isfinite(value) || value <= 0 || value > 16)
        throw Error(ErrorKind::vm, "invalid audio pitch");
    std::scoped_lock lock(voices_mutex_);
    for (auto &[voice, v] : voices_)
        if (voice == id || v.sound == id)
            v.pitch = value;
}
double GameAudio::pitch(int id) const {
    std::scoped_lock lock(voices_mutex_);
    if (const auto it = voices_.find(id); it != voices_.end())
        return it->second.pitch;
    for (const auto &[voice, v] : voices_)
        if (v.sound == id)
            return v.pitch;
    return 1.0;
}
void GameAudio::channel_limit(int count) {
    std::scoped_lock lock(voices_mutex_);
    channels_ = std::clamp(count, 1, 1024);
}
double GameAudio::length(int id) {
    std::scoped_lock lock(voices_mutex_);
    if (auto it = voices_.find(id); it != voices_.end() && it->second.clip)
        return static_cast<double>(it->second.clip->frames()) / 48000;
    return static_cast<double>(load(id)->frames()) / 48000;
}
double GameAudio::position(int id) const {
    std::scoped_lock lock(voices_mutex_);
    const auto it = voices_.find(id);
    return it == voices_.end() ? 0 : it->second.frame / 48000;
}
void GameAudio::position(int id, double seconds) {
    if (!std::isfinite(seconds) || seconds < 0)
        throw Error(ErrorKind::vm, "invalid audio seek");
    std::scoped_lock lock(voices_mutex_);
    if (auto it = voices_.find(id); it != voices_.end())
        it->second.frame = it->second.clip
                               ? std::min(seconds * 48000, static_cast<double>(it->second.clip->frames()))
                               : seconds * 48000;
}
std::vector<float> GameAudio::mix(std::size_t frames) {
    std::vector<float> output;
    mix(frames, output);
    return output;
}
void GameAudio::mix(std::size_t frames, std::vector<float> &out) {
    if (frames > 48000)
        throw Error(ErrorKind::vm, "audio block exceeds limit");
    service();
    out.resize(frames * 2);
    mix(out);
}
void GameAudio::service() {
#if REGML_WIIU
    auto completed = async_->take_completed();
    std::scoped_lock lock(voices_mutex_);
    for (auto &item : completed) {
        async_->finish(item.sound);
        if (!item.error.empty()) {
            std::erase_if(voices_, [&](const auto &voice) { return voice.second.sound == item.sound; });
            throw Error(ErrorKind::io, item.error);
        }
        auto &sound = sounds_.at(item.sound);
        sound.clip.reset(item.clip);
        sound.clip->gain = sound.gain;
        for (auto &[id, voice] : voices_)
            if (voice.sound == item.sound) {
                voice.clip = sound.clip.get();
                if (!voice.clip->compressed.empty() || !voice.clip->file_path.empty())
                    voice.stream = std::make_shared<Stream>(*voice.clip);
            }
    }
#endif
}
void GameAudio::mix(std::span<float> out, double playback_rate) noexcept {
    std::fill(out.begin(), out.end(), 0.0f);
    if (out.size() % 2 != 0 || !std::isfinite(playback_rate) || playback_rate <= 0)
        return;
    std::scoped_lock lock(voices_mutex_);
    const auto frames = out.size() / 2;
    for (auto &[id, v] : voices_) {
        if (v.paused || !v.clip)
            continue;
        const auto length = v.clip->frames();
        for (std::size_t f = 0; f < frames; ++f) {
            if (v.frame >= static_cast<double>(length)) {
                if (v.loop)
                    v.frame = std::fmod(v.frame, static_cast<double>(length));
                else
                    break;
            }
            if (v.clip->silent) {
                v.frame += v.pitch * playback_rate;
                continue;
            }
            const auto source = static_cast<std::size_t>(v.frame);
            float streamed[2]{};
            if (v.stream && !v.stream->read_frame(v.frame, streamed)) {
                v.frame = static_cast<double>(length);
                break;
            }
            for (std::size_t c = 0; c < 2; ++c) {
                const auto sample = (v.stream ? streamed[c] : v.clip->stereo[source * 2 + c]) *
                                    static_cast<float>(v.gain * v.clip->gain);
                out[f * 2 + c] += sample;
            }
            v.frame += v.pitch * playback_rate;
        }
    }
    std::erase_if(voices_, [](const auto &p) {
        return p.second.clip && !p.second.loop &&
               p.second.frame >= static_cast<double>(p.second.clip->frames());
    });
    for (auto &sample : out)
        sample = std::clamp(sample, -1.0f, 1.0f);
}
} // namespace regml
