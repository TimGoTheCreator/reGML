#include "package.hpp"
#include <algorithm>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_set>
namespace regml {
struct Package::FileReader {
    std::ifstream stream;
    std::mutex mutex;
    explicit FileReader(std::ifstream file) : stream(std::move(file)) {}
};
Package Package::load(const std::filesystem::path &path, LoadLimits limits) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
        throw Error(ErrorKind::io, "cannot open package: " + path.string());
    const auto end = stream.tellg();
    if (end < 0)
        throw Error(ErrorKind::io, "cannot determine package size");
    const auto length = static_cast<std::uint64_t>(static_cast<std::streamoff>(end));
    if (length > limits.max_file_bytes || length > std::numeric_limits<std::size_t>::max() ||
        length > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()))
        throw Error(ErrorKind::unsupported, "package exceeds configured file size limit");
    Package package;
    package.source_path_ = path;
    package.reader_ = std::make_shared<FileReader>(std::move(stream));
    package.size_ = static_cast<std::size_t>(length);
    package.parse(limits);
    return package;
}
Package Package::from_bytes(std::vector<std::uint8_t> bytes, LoadLimits limits) {
    if (bytes.size() > limits.max_file_bytes)
        throw Error(ErrorKind::unsupported, "package exceeds configured file size limit");
    Package package;
    package.bytes_ = std::move(bytes);
    package.size_ = package.bytes_.size();
    package.parse(limits);
    return package;
}
std::vector<std::uint8_t> Package::read_bytes(std::size_t offset, std::size_t length) const {
    if (offset > size_ || length > size_ - offset)
        throw Error(ErrorKind::malformed, "package read exceeds file bounds", offset);
    if (!bytes_.empty())
        return {bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
                bytes_.begin() + static_cast<std::ptrdiff_t>(offset + length)};
    if (source_path_.empty() && size_ == 0)
        return {};
    if (source_path_.empty())
        throw Error(ErrorKind::io, "package bytes are unavailable", offset);
    if (!reader_)
        throw Error(ErrorKind::io, "package reader is unavailable", offset);
    std::scoped_lock lock(reader_->mutex);
    auto &stream = reader_->stream;
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(offset));
    std::vector<std::uint8_t> bytes(length);
    if (length)
        stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(length));
    if (!stream)
        throw Error(ErrorKind::io, "could not read package range", offset);
    return bytes;
}
const Chunk *Package::find_chunk(std::string_view name) const noexcept {
    const auto it = std::find_if(chunks_.begin(), chunks_.end(), [name](const Chunk &c) { return c.name() == name; });
    return it == chunks_.end() ? nullptr : &*it;
}
void Package::parse(LoadLimits limits) {
    const auto header_bytes = read_bytes(0, std::min<std::size_t>(8, size_));
    const BinaryView header(header_bytes);
    const auto data_size = size_;
    header.require(0, 8);
    if (header.text(0, 4) != "FORM")
        throw Error(ErrorKind::malformed, "expected a GameMaker FORM container", 0);
    if (static_cast<std::size_t>(header.u32(4)) != size_ - 8)
        throw Error(ErrorKind::malformed, "FORM length does not match file size (strict inspection)", 4);
    std::unordered_set<std::string> seen;
    for (std::size_t offset = 8; offset < data_size;) {
        if (chunks_.size() >= limits.max_chunks)
            throw Error(ErrorKind::unsupported, "chunk count exceeds configured limit", offset);
        if (offset > data_size || 8 > data_size - offset)
            throw Error(ErrorKind::malformed, "truncated chunk header", offset);
        const auto record = read_bytes(offset, 8);
        const BinaryView chunk_header(record, offset);
        Chunk c;
        for (std::size_t i = 0; i < 4; ++i) {
            const auto byte = chunk_header.u8(i);
            if (byte < 0x20 || byte > 0x7e)
                throw Error(ErrorKind::malformed, "invalid chunk tag", offset + i);
            c.tag[i] = static_cast<char>(byte);
        }
        c.header_offset = offset;
        c.data_offset = offset + 8;
        c.size = chunk_header.u32(4);
        if (c.data_offset > data_size || c.size > data_size - c.data_offset)
            throw Error(ErrorKind::malformed, "chunk data exceeds package bounds", offset + 4);
        if (!seen.emplace(c.name()).second)
            throw Error(ErrorKind::unsupported, "duplicate chunk tags are not supported", offset);
        chunks_.push_back(c);
        // Chunk sizes already include padding.
        offset = c.data_offset + c.size;
    }
    if (const auto *c = find_chunk("GEN8"))
        parse_info(*c);
    if (info_ && info_->has_known_prefix) {
        if (const auto *c = find_chunk("STRG"))
            parse_strings(*c, limits);
        if (info_->name_pointer != 0)
            (void)string_pointer(info_->name_pointer);
        if (info_->display_name_pointer != 0)
            (void)string_pointer(info_->display_name_pointer);
        if (const auto *c = find_chunk("CODE"))
            parse_code(*c, limits);
    }
}
void Package::parse_info(const Chunk &c) {
    const auto bytes = read_bytes(c.data_offset, c.size);
    const BinaryView data(bytes, c.data_offset);
    data.require(0, 4);
    GeneralInfo info;
    info.debugger_disabled = data.u8(0) != 0;
    info.bytecode_version = data.u8(1);
    if (info.bytecode_version >= 14 && info.bytecode_version <= 17) {
        data.require(0, 104);
        info.game_id = data.u32(20);
        info.name_pointer = data.u32(40);
        for (std::size_t i = 0; i < 4; ++i)
            info.reported_version[i] = data.u32(44 + 4 * i);
        info.width = data.u32(60);
        info.height = data.u32(64);
        info.display_name_pointer = data.u32(100);
        info.has_known_prefix = true;
    }
    info_ = info;
}
std::size_t Package::list_count(const Chunk &c, LoadLimits limits) const {
    if (c.size < 4)
        throw Error(ErrorKind::malformed, "resource pointer table is truncated", c.data_offset);
    const auto prefix = read_bytes(c.data_offset, 4);
    const BinaryView data(prefix, c.data_offset);
    const auto count = static_cast<std::size_t>(data.u32(0));
    if (count > limits.max_entries)
        throw Error(ErrorKind::unsupported, "resource count exceeds configured limit", c.data_offset);
    if (count > (c.size - 4) / 4)
        throw Error(ErrorKind::malformed, "resource pointer table is truncated", c.data_offset);
    return count;
}
void Package::parse_strings(const Chunk &c, LoadLimits limits) {
    const auto bytes = read_bytes(c.data_offset, c.size);
    const BinaryView data(bytes, c.data_offset);
    const auto count = list_count(c, limits);
    const auto records_start = c.data_offset + 4 + 4 * count;
    strings_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto field = 4 + i * 4;
        const auto pointer = static_cast<std::size_t>(data.u32(field));
        const auto absolute_pointer = pointer;
        if (absolute_pointer < c.data_offset)
            throw Error(ErrorKind::malformed, "STRG pointer precedes its chunk", absolute_pointer);
        const auto local_pointer = absolute_pointer - c.data_offset;
        if (absolute_pointer < records_start)
            throw Error(ErrorKind::malformed, "STRG pointer targets the pointer table", c.data_offset + field);
        if (local_pointer > data.size() || data.size() - local_pointer < 4)
            throw Error(ErrorKind::malformed, "STRG reference leaves its chunk", absolute_pointer);
        const auto length = static_cast<std::size_t>(data.u32(local_pointer));
        if (length > data.size() - local_pointer - 4 || data.size() - local_pointer - 4 - length < 1)
            throw Error(ErrorKind::malformed, "STRG reference leaves its chunk", absolute_pointer);
        if (data.u8(local_pointer + 4 + length) != 0)
            throw Error(ErrorKind::malformed, "STRG string has no NUL terminator", absolute_pointer + 4 + length);
        strings_.push_back({absolute_pointer, length});
        string_values_.emplace_back(reinterpret_cast<const char *>(data.bytes().data() + local_pointer + 4), length);
        // STRG pointers include the length prefix; other string pointers skip it.
        string_lookup_.emplace(static_cast<std::size_t>(absolute_pointer + 4), i);
    }
}
void Package::parse_code(const Chunk &c, LoadLimits limits) {
    const auto bytes = read_bytes(c.data_offset, c.size);
    const BinaryView data(bytes, c.data_offset);
    const auto count = list_count(c, limits);
    const auto records_start = c.data_offset + 4 + 4 * count;
    code_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto field = 4 + 4 * i;
        const auto pointer = static_cast<std::size_t>(data.u32(field));
        const auto absolute_pointer = pointer;
        if (absolute_pointer < c.data_offset)
            throw Error(ErrorKind::malformed, "CODE pointer precedes its chunk", absolute_pointer);
        const auto local_pointer = absolute_pointer - c.data_offset;
        const bool old = info_->bytecode_version == 14;
        if (absolute_pointer < records_start)
            throw Error(ErrorKind::malformed, "CODE entry targets the pointer table", c.data_offset + field);
        if (local_pointer > data.size() || (old ? 8U : 20U) > data.size() - local_pointer)
            throw Error(ErrorKind::malformed, "CODE reference leaves its chunk", absolute_pointer);
        CodeEntry entry;
        entry.record_offset = absolute_pointer;
        entry.name_pointer = data.u32(local_pointer);
        if (entry.name_pointer != 0)
            (void)string_pointer(entry.name_pointer);
        entry.bytecode_length = data.u32(local_pointer + 4);
        if (old)
            entry.bytecode_offset = absolute_pointer + 8;
        else {
            entry.locals = data.u16(local_pointer + 8);
            const auto args = data.u16(local_pointer + 10);
            entry.arguments = static_cast<std::uint16_t>(args & 0x7fff);
            entry.locals_flag = (args & 0x8000) != 0;
            const auto target = static_cast<std::int64_t>(absolute_pointer + 12) + data.i32(local_pointer + 12);
            if (target < 0 || static_cast<std::uint64_t>(target) > size())
                throw Error(ErrorKind::malformed, "CODE relative bytecode address is out of bounds",
                            absolute_pointer + 12);
            entry.bytecode_offset = static_cast<std::size_t>(target);
            entry.entry_offset = data.u32(local_pointer + 16);
        }
        if (entry.bytecode_offset < c.data_offset || entry.bytecode_offset - c.data_offset > c.size ||
            entry.bytecode_length > c.size - (entry.bytecode_offset - c.data_offset))
            throw Error(ErrorKind::malformed, "CODE bytecode leaves its chunk", entry.bytecode_offset);
        if (entry.bytecode_length % 4 != 0 || entry.entry_offset % 4 != 0 || entry.entry_offset > entry.bytecode_length)
            throw Error(ErrorKind::malformed, "CODE length/entry offset is not a valid word range",
                        absolute_pointer + 4);
        if (entry.bytecode_offset < records_start)
            throw Error(ErrorKind::malformed, "CODE bytecode targets the pointer table", c.data_offset + field);
        code_.push_back(entry);
    }
}
std::string_view Package::string_at(std::size_t index) const {
    if (index >= strings_.size())
        throw Error(ErrorKind::usage, "string index is out of range");
    return string_values_.at(index);
}
std::string_view Package::string_pointer(std::uint32_t pointer) const {
    if (pointer == 0)
        return {};
    const auto it = string_lookup_.find(pointer);
    if (it == string_lookup_.end())
        throw Error(ErrorKind::malformed, "string reference does not target a STRG string", pointer);
    return string_at(it->second);
}
std::vector<std::uint8_t> Package::code_bytes(std::size_t index) const {
    if (index >= code_.size())
        throw Error(ErrorKind::usage, "code index is out of range");
    const auto &c = code_[index];
    return read_bytes(c.bytecode_offset, c.bytecode_length);
}
std::string_view Package::code_name(std::size_t index) const {
    if (index >= code_.size())
        throw Error(ErrorKind::usage, "code index is out of range");
    return string_pointer(code_[index].name_pointer);
}
} // namespace regml
