#include "audio.hpp"
#include "bit_cast.hpp"
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
bool has_device_prefix(std::string_view path) {
    const auto colon = path.find(':');
    const auto separator = path.find_first_of("/\\");
    return colon != std::string_view::npos && (separator == std::string_view::npos || colon < separator);
}

std::vector<std::size_t> audio_entries(const Package &package, const Chunk &chunk) {
    if (chunk.size < 4)
        throw Error(ErrorKind::malformed, "audio table range", chunk.data_offset);
    const auto prefix = package.read_bytes(chunk.data_offset, 4);
    const auto count = BinaryView(prefix, chunk.data_offset).u32(0);
    if (count > 1'000'000 || count > (chunk.size - 4) / 4)
        throw Error(ErrorKind::malformed, "audio table range", chunk.data_offset);
    const auto table = package.read_bytes(chunk.data_offset, 4 + static_cast<std::size_t>(count) * 4);
    const BinaryView view(table, chunk.data_offset);
    std::vector<std::size_t> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto pointer = view.u32(4 + i * 4);
        if (pointer < chunk.data_offset || pointer >= chunk.data_offset + chunk.size)
            throw Error(ErrorKind::malformed, "audio pointer outside chunk", pointer);
        out.push_back(pointer);
    }
    return out;
}
std::optional<std::filesystem::path> find_audio_file(const std::filesystem::path &directory,
                                                     std::string_view filename) {
    const std::filesystem::path relative_path(filename);
    if (relative_path.is_absolute() || has_device_prefix(filename)) {
        std::error_code error;
        if (std::filesystem::is_regular_file(relative_path, error) && !error)
            return relative_path;
        return {};
    }
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

std::filesystem::path resolve_audio_file(const std::filesystem::path &directory, std::string_view filename) {
    if (const auto found = find_audio_file(directory, filename))
        return *found;
    const std::filesystem::path requested(filename);
    if (requested.is_absolute() || has_device_prefix(filename))
        return requested;
    return directory / requested;
}

std::optional<std::uint64_t> ogg_total_samples(std::span<const std::uint8_t> bytes) {
    std::size_t offset = 0;
    std::optional<std::uint64_t> total;
    while (offset + 27 <= bytes.size()) {
        if (std::memcmp(bytes.data() + offset, "OggS", 4) != 0 || bytes[offset + 4] != 0)
            return std::nullopt;
        const auto segment_count = bytes[offset + 26];
        if (offset + 27 + segment_count > bytes.size())
            return std::nullopt;
        std::size_t page_size = 27 + segment_count;
        for (std::size_t i = 0; i < segment_count; ++i)
            page_size += bytes[offset + 27 + i];
        if (page_size > bytes.size() - offset)
            return std::nullopt;
        if (bytes[offset + 5] & 0x04) {
            std::uint64_t granule = 0;
            for (int i = 0; i < 8; ++i)
                granule |= static_cast<std::uint64_t>(bytes[offset + 6 + i]) << (i * 8);
            if (granule != UINT64_MAX)
                total = granule;
        }
        offset += page_size;
    }
    return offset == bytes.size() ? total : std::nullopt;
}

void normalize_ogg_serial(std::vector<std::uint8_t> &bytes) {
    std::size_t offset = 0;
    while (offset + 27 <= bytes.size()) {
        if (std::memcmp(bytes.data() + offset, "OggS", 4) != 0 || bytes[offset + 4] != 0)
            return;
        const auto segment_count = bytes[offset + 26];
        if (offset + 27 + segment_count > bytes.size())
            return;
        std::size_t page_size = 27 + segment_count;
        for (std::size_t i = 0; i < segment_count; ++i)
            page_size += bytes[offset + 27 + i];
        if (page_size > bytes.size() - offset)
            return;

        const auto serial = static_cast<std::uint32_t>(bytes[offset + 14]) |
                            (static_cast<std::uint32_t>(bytes[offset + 15]) << 8) |
                            (static_cast<std::uint32_t>(bytes[offset + 16]) << 16) |
                            (static_cast<std::uint32_t>(bytes[offset + 17]) << 24);
        if (serial & 0x80000000u) {
            auto normalized = serial & 0x7fffffffu;
            if (normalized == 0)
                normalized = 1;
            for (int i = 0; i < 4; ++i)
                bytes[offset + 14 + i] = static_cast<std::uint8_t>(normalized >> (i * 8));

            std::uint32_t crc = 0;
            for (std::size_t i = 0; i < page_size; ++i) {
                const auto byte = i >= 22 && i < 26 ? 0 : bytes[offset + i];
                crc ^= static_cast<std::uint32_t>(byte) << 24;
                for (int bit = 0; bit < 8; ++bit)
                    crc = (crc << 1) ^ ((crc & 0x80000000) ? 0x04c11db7 : 0);
            }
            for (int i = 0; i < 4; ++i)
                bytes[offset + 22 + i] = static_cast<std::uint8_t>(crc >> (i * 8));
        }
        offset += page_size;
    }
}

bool ogg_first_pages_valid(std::span<const std::uint8_t> bytes) {
    std::size_t offset = 0;
    for (int page_index = 0; page_index < 2; ++page_index) {
        if (offset + 27 > bytes.size() || std::memcmp(bytes.data() + offset, "OggS", 4) != 0)
            return false;
        const auto segment_count = bytes[offset + 26];
        if (offset + 27 + segment_count > bytes.size())
            return false;
        std::size_t size = 27 + segment_count;
        for (std::size_t i = 0; i < segment_count; ++i)
            size += bytes[offset + 27 + i];
        if (size > bytes.size() - offset)
            return false;
        std::uint32_t crc = 0;
        for (std::size_t i = 0; i < size; ++i) {
            const auto byte = i >= 22 && i < 26 ? 0 : bytes[offset + i];
            crc ^= static_cast<std::uint32_t>(byte) << 24;
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc << 1) ^ ((crc & 0x80000000) ? 0x04c11db7 : 0);
        }
        std::uint32_t stored = 0;
        for (int i = 0; i < 4; ++i)
            stored |= static_cast<std::uint32_t>(bytes[offset + 22 + i]) << (i * 8);
        if (crc != stored)
            return false;
        offset += size;
    }
    return true;
}

std::string inspect_ogg_headers(std::span<const std::uint8_t> bytes) {
    ogg_sync_state sync{};
    if (ogg_sync_init(&sync) != 0)
        return "sync-init";
    const auto count = std::min<std::size_t>(bytes.size(), 8192);
    auto *buffer = ogg_sync_buffer(&sync, static_cast<long>(count));
    if (!buffer) {
        ogg_sync_clear(&sync);
        return "sync-buffer";
    }
    std::memcpy(buffer, bytes.data(), count);
    ogg_sync_wrote(&sync, static_cast<long>(count));

    vorbis_info info{};
    vorbis_comment comment{};
    vorbis_info_init(&info);
    vorbis_comment_init(&comment);
    ogg_stream_state stream{};
    ogg_page page{};
    ogg_packet packet{};
    bool stream_ready = false;
    long serial = 0;
    int page_serial = 0, page_version = -1, serial_matches = 0;
    std::uint32_t raw_serial = 0;
    int pages = 0, packets = 0, pagein_result = -999, packet_result = -999, init_result = -999;
    int header_results[3]{-999, -999, -999};
    while (pages < 4) {
        const int page_result = ogg_sync_pageout(&sync, &page);
        if (page_result <= 0)
            break;
        ++pages;
        if (!stream_ready) {
            page_serial = ogg_page_serialno(&page);
            serial = page_serial;
            page_version = ogg_page_version(&page);
            raw_serial = static_cast<std::uint32_t>(page.header[14]) |
                         (static_cast<std::uint32_t>(page.header[15]) << 8) |
                         (static_cast<std::uint32_t>(page.header[16]) << 16) |
                         (static_cast<std::uint32_t>(page.header[17]) << 24);
            init_result = ogg_stream_init(&stream, static_cast<int>(raw_serial));
            if (init_result != 0)
                break;
            stream_ready = true;
            serial_matches = stream.serialno == ogg_page_serialno(&page);
        }
        pagein_result = ogg_stream_pagein(&stream, &page);
        if (pagein_result != 0)
            break;
        while (packets < 3 && (packet_result = ogg_stream_packetout(&stream, &packet)) > 0) {
            if (packets == 0 && !vorbis_synthesis_idheader(&packet))
                header_results[packets] = -998;
            else
                header_results[packets] = vorbis_synthesis_headerin(&info, &comment, &packet);
            ++packets;
        }
        if (packets == 3)
            break;
    }
    const auto result = "pages=" + std::to_string(pages) + " packets=" + std::to_string(packets) +
                        " serial=" + std::to_string(serial) + " pagein=" +
                        std::to_string(pagein_result) + " packetout=" + std::to_string(packet_result) +
                        " raw=" + std::to_string(raw_serial) + " stream=" +
                        std::to_string(stream.serialno) + " sizes=" + std::to_string(sizeof(long)) + "/" +
                        std::to_string(sizeof(int)) + "/" + std::to_string(sizeof(void *)) +
                        " init=" + std::to_string(init_result) + " page_serial=" +
                        std::to_string(page_serial) + " match=" + std::to_string(serial_matches) +
                        " version=" + std::to_string(page_version) +
                        " headers=" + std::to_string(header_results[0]) + "," +
                        std::to_string(header_results[1]) + "," + std::to_string(header_results[2]) +
                        " format=" + std::to_string(info.channels) + "ch/" + std::to_string(info.rate);
    if (stream_ready)
        ogg_stream_clear(&stream);
    vorbis_comment_clear(&comment);
    vorbis_info_clear(&info);
    ogg_sync_clear(&sync);
    return result;
}
} // namespace
struct GameAudio::Stream {
    std::span<const std::uint8_t> bytes;
    std::size_t position = 0, begin = 0, count = 0;
#if REGML_PS2
    std::size_t decoded_end = 0;
#endif
    long rate = 0;
    int channels = 0;
    std::ifstream file_stream;
    const Package *package = nullptr;
    std::size_t package_offset = 0, package_length = 0;
    ov_callbacks callbacks{};
    OggVorbis_File ogg_file{};
    std::vector<float> samples;
    explicit Stream(const Clip &clip) : bytes(clip.compressed), samples(8192) {
        package = clip.package;
        package_offset = clip.package_offset;
        package_length = clip.package_length;
        if (!clip.file_path.empty()) {
#if REGML_PS2
            if (ov_fopen(clip.file_path.c_str(), &ogg_file) < 0)
                throw Error(ErrorKind::malformed, "streaming Ogg file could not be opened");
#else
            file_stream.open(clip.file_path, std::ios::binary);
            if (!file_stream)
                throw Error(ErrorKind::io, "streaming Ogg file could not be opened");
#endif
        }
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
#if REGML_PS2
        callbacks.seek_func = nullptr;
        callbacks.tell_func = nullptr;
#endif
        if (ov_open_callbacks(this, &ogg_file, nullptr, 0, callbacks) < 0)
            throw Error(ErrorKind::malformed, "streaming Ogg initialization failed");
        const auto *info = ov_info(&ogg_file, -1);
        if (!info)
            throw Error(ErrorKind::malformed, "streaming Ogg headers are unavailable");
        rate = info->rate;
        channels = info->channels;
    }
    bool restart() {
        ov_clear(&ogg_file);
        position = 0;
        begin = 0;
        count = 0;
#if REGML_PS2
        decoded_end = 0;
#endif
        return ov_open_callbacks(this, &ogg_file, nullptr, 0, callbacks) == 0;
    }
    ~Stream() { ov_clear(&ogg_file); }
    bool read_frame(double output_frame, float (&out)[2]) noexcept {
        const double input = output_frame * static_cast<double>(rate) / 48000;
        const auto frame = static_cast<std::size_t>(input);
#if REGML_PS2
        if (frame < begin) {
            if (ov_seekable(&ogg_file)) {
                if (ov_pcm_seek(&ogg_file, static_cast<ogg_int64_t>(frame)) < 0)
                    return false;
                begin = frame;
                count = 0;
                decoded_end = frame;
            } else if (!restart()) {
                return false;
            }
        }
        while (frame >= begin + count) {
            int section = 0;
            float **pcm = nullptr;
            const auto read = ov_read_float(&ogg_file, &pcm, 4096, &section);
            if (read <= 0)
                return false;
            const auto *info = ov_info(&ogg_file, section);
            if (!info || info->channels != channels || info->rate != rate)
                return false;
            begin = decoded_end;
            count = static_cast<std::size_t>(read);
            decoded_end += count;
            for (std::size_t i = 0; i < count; ++i)
                for (int c = 0; c < channels; ++c)
                    samples[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)] = pcm[c][i];
        }
#else
        if (frame < begin || frame >= begin + count) {
            auto cursor = ov_pcm_tell(&ogg_file);
            if (cursor < 0)
                return false;
            if (static_cast<std::uint64_t>(cursor) != frame &&
                ov_pcm_seek(&ogg_file, static_cast<ogg_int64_t>(frame)) < 0) {
                return false;
            }
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
#endif
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
    if (package_.is_gmrt())
        return;
    if (const auto *c = package.find_chunk("AUDO"))
        embedded_ = audio_entries(package, *c);
    if (const auto *c = package.find_chunk("SOND")) {
        const auto records = audio_entries(package, *c);
        sounds_.reserve(records.size());
        for (auto p : records) {
            if (c->data_offset + c->size - p < 36)
                throw Error(ErrorKind::malformed, "sound record range", p);
            const auto record = package.read_bytes(p, 36);
            const BinaryView d(record, p);
            Sound sound;
            sound.name = package.string_pointer(d.u32(0));
            sound.flags = d.u32(4);
            sound.filename = package.string_pointer(d.u32(12));
            sound.gain = regml::bit_cast<float>(d.u32(20));
            const auto group = d.i32(28), audio_id = d.i32(32);
            sound.group = group;
            sound.audio_id = audio_id;
            sounds_.push_back(std::move(sound));
        }
    }
}
GameAudio::~GameAudio() {
#if REGML_WIIU
    // Async decode tasks may still be reading a lazily opened audio-group file.
    async_.reset();
#endif
}
GameAudio::Sound *GameAudio::find_sound(int id) noexcept {
    if (const auto stream = stream_sounds_.find(id); stream != stream_sounds_.end())
        return &stream->second;
    if (id >= 0 && static_cast<std::size_t>(id) < sounds_.size())
        return &sounds_[static_cast<std::size_t>(id)];
    return nullptr;
}
const GameAudio::Sound *GameAudio::find_sound(int id) const noexcept {
    if (const auto stream = stream_sounds_.find(id); stream != stream_sounds_.end())
        return &stream->second;
    if (id >= 0 && static_cast<std::size_t>(id) < sounds_.size())
        return &sounds_[static_cast<std::size_t>(id)];
    return nullptr;
}
GameAudio::Sound &GameAudio::sound_at(int id) {
    if (auto *sound = find_sound(id))
        return *sound;
    throw Error(ErrorKind::vm, "audio resource does not exist");
}
const GameAudio::AudioGroup *GameAudio::load_audio_group(int group) {
    if (group <= 0 || group > 4096 || missing_audio_groups_.contains(group))
        return nullptr;
    if (const auto found = audio_groups_.find(group); found != audio_groups_.end())
        return &found->second;

    const auto path = directory_ / ("audiogroup" + std::to_string(group) + ".dat");
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        missing_audio_groups_.insert(group);
        return nullptr;
    }

    AudioGroup loaded;
    loaded.package = std::make_unique<Package>(Package::load(path));
    const auto *chunk = loaded.package->find_chunk("AUDO");
    if (!chunk)
        throw Error(ErrorKind::malformed, "audio group has no AUDO chunk");
    loaded.entries = audio_entries(*loaded.package, *chunk);
    return &audio_groups_.emplace(group, std::move(loaded)).first->second;
}
bool GameAudio::load_group(int group) {
    return group == 0 || load_audio_group(group) != nullptr;
}
bool GameAudio::group_is_loaded(int group) const {
    return group == 0 || audio_groups_.contains(group);
}
std::optional<GameAudio::AudioData> GameAudio::audio_data(const Sound &sound) {
    constexpr std::uint32_t embedded_flag = 0x1;
    if (!(sound.flags & embedded_flag) || sound.audio_id < 0)
        return {};

    const Package *package = nullptr;
    const std::vector<std::size_t> *entries = nullptr;
    if (sound.group == 0) {
        package = &package_;
        entries = &embedded_;
    } else if (const auto *group = load_audio_group(sound.group)) {
        package = group->package.get();
        entries = &group->entries;
    } else {
        return {};
    }

    if (static_cast<std::size_t>(sound.audio_id) >= entries->size())
        throw Error(ErrorKind::malformed, "audio ID out of range");
    const auto *chunk = package->find_chunk("AUDO");
    const auto at = (*entries)[static_cast<std::size_t>(sound.audio_id)];
    if (!chunk || at < chunk->data_offset || at > chunk->data_offset + chunk->size ||
        chunk->data_offset + chunk->size - at < 4)
        throw Error(ErrorKind::malformed, "audio length out of range", at);
    const auto prefix = package->read_bytes(at, 4);
    const auto length = static_cast<std::size_t>(BinaryView(prefix, at).u32(0));
    if (length > chunk->data_offset + chunk->size - at - 4)
        throw Error(ErrorKind::malformed, "audio length out of range", at);
    return AudioData{package, at + 4, length};
}
std::optional<int> GameAudio::find_sound_id(std::string_view filename) const {
    for (std::size_t id = 0; id < sounds_.size(); ++id)
        if (sounds_[id].name == filename || sounds_[id].filename == filename)
            return static_cast<int>(id);
    for (const auto &[id, sound] : stream_sounds_)
        if (sound.name == filename || sound.filename == filename)
            return id;
    return std::nullopt;
}
std::unique_ptr<GameAudio::Clip> GameAudio::decode(std::vector<std::uint8_t> owned_bytes) {
#if REGML_PS2
    if (owned_bytes.size() >= 4 && std::memcmp(owned_bytes.data(), "OggS", 4) == 0)
        normalize_ogg_serial(owned_bytes);
#endif
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
#if REGML_PS2
        callbacks.seek_func = nullptr;
        callbacks.tell_func = nullptr;
#endif
        OggVorbis_File file{};
        const int open_result = ov_open_callbacks(&reader, &file, nullptr, 0, callbacks);
        if (open_result < 0)
            throw Error(ErrorKind::malformed, "Ogg audio decode initialization failed (code " +
                                                    std::to_string(open_result) + ", read " +
                                                    std::to_string(reader.position) + "/" +
                                                    std::to_string(bytes.size()) + ", pages " +
                                                    (ogg_first_pages_valid(bytes) ? "ok" : "bad") + ", " +
                                                    inspect_ogg_headers(bytes) + ")");
        struct Guard {
            OggVorbis_File &f;
            ~Guard() { ov_clear(&f); }
        } guard{file};
        const auto *info = ov_info(&file, -1);
        rate = info->rate;
        channels = info->channels;
        if (channels < 1 || channels > 2 || rate < 1 || rate > 192000)
            throw Error(ErrorKind::unsupported, "unsupported Ogg channel/rate profile");
        auto total = ov_pcm_total(&file, -1);
#if REGML_PS2
        if (total <= 0) {
            if (const auto samples = ogg_total_samples(bytes);
                samples && *samples <= static_cast<std::uint64_t>(INT64_MAX))
                total = static_cast<ogg_int64_t>(*samples);
        }
#endif
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
std::vector<std::uint8_t> GameAudio::read_audio(int id) {
    const auto *sound = find_sound(id);
    if (!sound)
        throw Error(ErrorKind::vm, "audio resource does not exist");
    if (const auto data = audio_data(*sound))
        return data->package->read_bytes(data->offset, data->length);
    const auto path = resolve_audio_file(directory_, sound->filename);
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
    } else {
        throw Error(ErrorKind::io, "audio file missing: " + path.string());
    }
}
GameAudio::Clip *GameAudio::load(int id) {
    auto &sound = sound_at(id);
    if (sound.clip)
        return sound.clip.get();
    const auto data = audio_data(sound);
    const auto path = !data && !sound.filename.empty() ? resolve_audio_file(directory_, sound.filename)
                                                        : std::filesystem::path{};
#if REGML_WII || REGML_PS2
    if (!data && !path.empty()) {
        std::ifstream input(path, std::ios::binary);
        char signature[4]{};
        input.read(signature, sizeof(signature));
        if (input && std::memcmp(signature, "OggS", sizeof(signature)) == 0) {
#if REGML_PS2
            auto clip = decode(read_audio(id));
            clip->gain = sound.gain;
            sound.clip = std::move(clip);
            return sound.clip.get();
#else
            OggVorbis_File file{};
            const auto path_text = path.string();
            const int open_result = ov_fopen(path_text.c_str(), &file);
            if (open_result == 0) {
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
            } else {
#if REGML_PS2
                auto clip = std::make_unique<Clip>();
                clip->silent = true;
                clip->streamed_frames = 48'000 * 60 * 60;
                clip->gain = sound.gain;
                sound.clip = std::move(clip);
                return sound.clip.get();
#endif
            }
#endif
        }
    }
#endif
#if !REGML_PS2
    if (!data && !path.empty()) {
        auto clip = std::make_unique<Clip>();
        clip->file_path = path.string();
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
#endif
    if (data) {
        const auto signature = data->package->read_bytes(data->offset, std::min<std::size_t>(data->length, 4));
        if (signature.size() == 4 && std::memcmp(signature.data(), "OggS", 4) == 0) {
#if REGML_PS2
            auto clip = decode(data->package->read_bytes(data->offset, data->length));
            clip->gain = sound.gain;
            sound.clip = std::move(clip);
            return sound.clip.get();
#else
            auto clip = std::make_unique<Clip>();
            clip->package = data->package;
            clip->package_offset = data->offset;
            clip->package_length = data->length;
            Stream probe(*clip);
            auto total = ov_pcm_total(&probe.ogg_file, -1);
#if REGML_PS2
            if (total <= 0 && data->length <= 100'000'000) {
                const auto bytes = data->package->read_bytes(data->offset, data->length);
                if (const auto samples = ogg_total_samples(bytes);
                    samples && *samples <= static_cast<std::uint64_t>(INT64_MAX))
                    total = static_cast<ogg_int64_t>(*samples);
            }
#endif
            if (!probe.rate || probe.channels < 1 || probe.channels > 2 || total <= 0 ||
                total > 100'000'000 / probe.channels)
                throw Error(ErrorKind::unsupported, "audio-group Ogg duration exceeds clip limit", data->offset);
            clip->streamed_frames = static_cast<std::size_t>(static_cast<double>(total) * 48000 /
                                                             static_cast<double>(probe.rate));
            if (clip->streamed_frames > 50'000'000)
                throw Error(ErrorKind::unsupported, "resampled audio exceeds limit", data->offset);
            clip->gain = sound.gain;
            sound.clip = std::move(clip);
            return sound.clip.get();
#endif
        }
    }
    sound.clip = decode(read_audio(id));
    sound.clip->gain = sound.gain;
    return sound.clip.get();
}
int GameAudio::create_stream(std::string_view filename) {
    const int id = next_stream_++;
    Sound stream;
    stream.filename = filename;
    stream_sounds_.emplace(id, std::move(stream));
    streams_.insert(id);
    const auto path = resolve_audio_file(directory_, filename);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        auto &sound = sound_at(id);
        sound.clip = std::make_unique<Clip>();
        sound.clip->silent = true;
        sound.clip->streamed_frames = 48'000 * 60 * 60;
        return id;
    }
    sound_at(id).filename = path.string();
    load(id);
    return id;
}
bool GameAudio::destroy_stream(int id) {
    std::scoped_lock lock(voices_mutex_);
    const auto stream = streams_.find(id);
    if (stream == streams_.end())
        return false;
    std::erase_if(voices_, [id](const auto &voice) { return voice.second.sound == id; });
    stream_sounds_.erase(id);
    streams_.erase(stream);
    return true;
}
void GameAudio::group_gain(int group, double value, double duration_ms) {
    if (!std::isfinite(value) || !std::isfinite(duration_ms) || duration_ms < 0)
        throw Error(ErrorKind::vm, "invalid audio group gain or fade duration");
    const auto frames = static_cast<std::size_t>(std::clamp(duration_ms, 0.0, 3'600'000.0) * 48.0);
    std::scoped_lock lock(voices_mutex_);
    auto &gain = group_gains_[group];
    gain.target = value;
    gain.frames_left = frames;
    gain.step = frames ? (gain.target - gain.current) / static_cast<double>(frames) : 0;
    if (!frames)
        gain.current = value;
}
double GameAudio::group_gain(int group) const {
    std::scoped_lock lock(voices_mutex_);
    const auto found = group_gains_.find(group);
    return found == group_gains_.end() ? 1.0 : found->second.current;
}
int GameAudio::play(int sound, bool loop, double priority) {
#if REGML_WIIU
    auto *resource = find_sound(sound);
    if (!resource)
        throw Error(ErrorKind::vm, "audio resource does not exist");
    auto *clip = resource->clip.get();
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
    voices_[id].group = sound_at(sound).group;
    if (clip && (!clip->compressed.empty() || !clip->file_path.empty() || clip->package))
        voices_[id].stream = std::make_shared<Stream>(*clip);
#if REGML_WIIU
    if (!clip && !async_->pending(sound)) {
        const auto &resource = sound_at(sound);
        const auto data = audio_data(resource);
        const auto audio_path = data ? std::filesystem::path{}
                                     : find_audio_file(directory_, resource.filename)
                                           .value_or(directory_ / resource.filename);
        const auto &source_package = data ? *data->package : package_;
        try {
            async_->request(sound,
                            audio_path,
                            source_package,
                            data ? data->offset : 0,
                            data ? data->length : 0,
                            data.has_value());
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
    if (const auto *sound = find_sound(id))
        return sound->gain;
    if ((id >= 100'000 && id < next_stream_) || (id >= 1'000'000 && id < next_voice_))
        return 1.0;
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
    else if (auto *sound = find_sound(id)) {
        sound->gain = value;
        if (sound->clip)
            sound->clip->gain = value;
    } else if (!((id >= 100'000 && id < next_stream_) || (id >= 1'000'000 && id < next_voice_)))
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
    if (((id >= 100'000 && id < next_stream_) || (id >= 1'000'000 && id < next_voice_)) &&
        !find_sound(id))
        return 0.0;
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
        auto &sound = sound_at(item.sound);
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
        const auto group = group_gains_.find(v.group);
        const auto group_start = group == group_gains_.end() ? 1.0 : group->second.current;
        const auto group_target = group == group_gains_.end() ? 1.0 : group->second.target;
        const auto group_step = group == group_gains_.end() ? 0.0 : group->second.step;
        const auto group_frames_left = group == group_gains_.end() ? 0 : group->second.frames_left;
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
            const auto group_volume = f < group_frames_left
                                          ? group_start + group_step * static_cast<double>(f + 1)
                                          : group_target;
            for (std::size_t c = 0; c < 2; ++c) {
                const auto sample = (v.stream ? streamed[c] : v.clip->stereo[source * 2 + c]) *
                                    static_cast<float>(v.gain * v.clip->gain * group_volume);
                out[f * 2 + c] += sample;
            }
            v.frame += v.pitch * playback_rate;
        }
    }
    std::erase_if(voices_, [](const auto &p) {
        return p.second.clip && !p.second.loop &&
               p.second.frame >= static_cast<double>(p.second.clip->frames());
    });
    for (auto &[group, gain] : group_gains_) {
        if (!gain.frames_left)
            continue;
        const auto advanced = std::min(frames, gain.frames_left);
        gain.current += gain.step * static_cast<double>(advanced);
        gain.frames_left -= advanced;
        if (!gain.frames_left)
            gain.current = gain.target;
    }
    for (auto &sample : out)
        sample = std::clamp(sample, -1.0f, 1.0f);
}
} // namespace regml
