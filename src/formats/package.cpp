#include "regml/formats/package.hpp"
#include <algorithm>
#include <fstream>
#include <limits>
#include <string>
#include <unordered_set>
namespace regml {
namespace {
void require_in_chunk(const Chunk &c, std::size_t offset, std::size_t length) {
    if (offset < c.data_offset || offset - c.data_offset > c.size || length > c.size - (offset - c.data_offset))
        throw Error(ErrorKind::malformed, std::string(c.name()) + " reference leaves its chunk", offset);
}
} // namespace
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
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    stream.seekg(0);
    if (!bytes.empty())
        stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(length));
    if (!stream)
        throw Error(ErrorKind::io, "could not read the complete package");
    return from_bytes(std::move(bytes), limits);
}
Package Package::from_bytes(std::vector<std::uint8_t> bytes, LoadLimits limits) {
    if (bytes.size() > limits.max_file_bytes)
        throw Error(ErrorKind::unsupported, "package exceeds configured file size limit");
    Package package;
    package.bytes_ = std::move(bytes);
    package.parse(limits);
    return package;
}
const Chunk *Package::find_chunk(std::string_view name) const noexcept {
    const auto it = std::find_if(chunks_.begin(), chunks_.end(), [name](const Chunk &c) { return c.name() == name; });
    return it == chunks_.end() ? nullptr : &*it;
}
void Package::parse(LoadLimits limits) {
    const auto data = view();
    data.require(0, 8);
    if (data.text(0, 4) != "FORM")
        throw Error(ErrorKind::malformed, "expected a GameMaker FORM container", 0);
    if (static_cast<std::size_t>(data.u32(4)) != size() - 8)
        throw Error(ErrorKind::malformed, "FORM length does not match file size (strict inspection)", 4);
    std::unordered_set<std::string> seen;
    for (std::size_t offset = 8; offset < size();) {
        if (chunks_.size() >= limits.max_chunks)
            throw Error(ErrorKind::unsupported, "chunk count exceeds configured limit", offset);
        data.require(offset, 8);
        Chunk c;
        for (std::size_t i = 0; i < 4; ++i) {
            const auto byte = data.u8(offset + i);
            if (byte < 0x20 || byte > 0x7e)
                throw Error(ErrorKind::malformed, "invalid chunk tag", offset + i);
            c.tag[i] = static_cast<char>(byte);
        }
        c.header_offset = offset;
        c.data_offset = offset + 8;
        c.size = data.u32(offset + 4);
        data.require(c.data_offset, c.size);
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
    const auto data = view().slice(c.data_offset, c.size);
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
    const auto data = view().slice(c.data_offset, c.size);
    const auto count = static_cast<std::size_t>(data.u32(0));
    if (count > limits.max_entries)
        throw Error(ErrorKind::unsupported, "resource count exceeds configured limit", c.data_offset);
    if (count > (data.size() - 4) / 4)
        throw Error(ErrorKind::malformed, "resource pointer table is truncated", c.data_offset);
    return count;
}
void Package::parse_strings(const Chunk &c, LoadLimits limits) {
    const auto data = view();
    const auto count = list_count(c, limits);
    const auto records_start = c.data_offset + 4 + 4 * count;
    strings_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto field = c.data_offset + 4 + i * 4;
        const auto pointer = static_cast<std::size_t>(data.u32(field));
        if (pointer < records_start)
            throw Error(ErrorKind::malformed, "STRG pointer targets the pointer table", field);
        require_in_chunk(c, pointer, 4);
        const auto length = static_cast<std::size_t>(data.u32(pointer));
        require_in_chunk(c, pointer + 4, length);
        require_in_chunk(c, pointer + 4 + length, 1);
        if (data.u8(pointer + 4 + length) != 0)
            throw Error(ErrorKind::malformed, "STRG string has no NUL terminator", pointer + 4 + length);
        strings_.push_back({pointer, length});
        // STRG pointers include the length prefix; other string pointers skip it.
        string_lookup_.emplace(pointer + 4, i);
    }
}
void Package::parse_code(const Chunk &c, LoadLimits limits) {
    const auto data = view();
    const auto count = list_count(c, limits);
    const auto records_start = c.data_offset + 4 + 4 * count;
    code_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto field = c.data_offset + 4 + 4 * i;
        const auto pointer = static_cast<std::size_t>(data.u32(field));
        const bool old = info_->bytecode_version == 14;
        if (pointer < records_start)
            throw Error(ErrorKind::malformed, "CODE entry targets the pointer table", field);
        require_in_chunk(c, pointer, old ? 8 : 20);
        CodeEntry entry;
        entry.record_offset = pointer;
        entry.name_pointer = data.u32(pointer);
        if (entry.name_pointer != 0)
            (void)string_pointer(entry.name_pointer);
        entry.bytecode_length = data.u32(pointer + 4);
        if (old)
            entry.bytecode_offset = pointer + 8;
        else {
            entry.locals = data.u16(pointer + 8);
            const auto args = data.u16(pointer + 10);
            entry.arguments = static_cast<std::uint16_t>(args & 0x7fff);
            entry.locals_flag = (args & 0x8000) != 0;
            const auto target = static_cast<std::int64_t>(pointer + 12) + data.i32(pointer + 12);
            if (target < 0 || static_cast<std::uint64_t>(target) > size())
                throw Error(ErrorKind::malformed, "CODE relative bytecode address is out of bounds", pointer + 12);
            entry.bytecode_offset = static_cast<std::size_t>(target);
            entry.entry_offset = data.u32(pointer + 16);
        }
        require_in_chunk(c, entry.bytecode_offset, entry.bytecode_length);
        if (entry.bytecode_length % 4 != 0 || entry.entry_offset % 4 != 0 || entry.entry_offset > entry.bytecode_length)
            throw Error(ErrorKind::malformed, "CODE length/entry offset is not a valid word range", pointer + 4);
        if (entry.bytecode_offset < records_start)
            throw Error(ErrorKind::malformed, "CODE bytecode targets the pointer table", field);
        code_.push_back(entry);
    }
}
std::string_view Package::string_at(std::size_t index) const {
    if (index >= strings_.size())
        throw Error(ErrorKind::usage, "string index is out of range");
    const auto &s = strings_[index];
    return view().text(s.record_offset + 4, s.length);
}
std::string_view Package::string_pointer(std::uint32_t pointer) const {
    if (pointer == 0)
        return {};
    const auto it = string_lookup_.find(pointer);
    if (it == string_lookup_.end())
        throw Error(ErrorKind::malformed, "string reference does not target a STRG string", pointer);
    return string_at(it->second);
}
BinaryView Package::code_bytes(std::size_t index) const {
    if (index >= code_.size())
        throw Error(ErrorKind::usage, "code index is out of range");
    const auto &c = code_[index];
    return view().slice(c.bytecode_offset, c.bytecode_length);
}
std::string_view Package::code_name(std::size_t index) const {
    if (index >= code_.size())
        throw Error(ErrorKind::usage, "code index is out of range");
    return string_pointer(code_[index].name_pointer);
}
} // namespace regml
