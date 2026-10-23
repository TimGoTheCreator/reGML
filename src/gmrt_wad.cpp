#include "gmrt_wad.hpp"
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_set>

namespace regml {
namespace {
std::vector<std::uint8_t>
read_range(std::ifstream &stream, std::size_t file_size, std::size_t offset, std::size_t length) {
    if (offset > file_size || length > file_size - offset)
        throw Error(ErrorKind::malformed, "GMRT WAD read exceeds file bounds", offset);
    if (offset > static_cast<std::size_t>(std::numeric_limits<std::streamoff>::max()) ||
        length > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
        throw Error(ErrorKind::unsupported, "GMRT WAD range is too large for this platform", offset);

    std::vector<std::uint8_t> bytes(length);
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(offset));
    if (!stream)
        throw Error(ErrorKind::io, "could not seek in GMRT WAD", offset);
    if (length != 0)
        stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(length));
    if (!stream)
        throw Error(ErrorKind::io, "could not read GMRT WAD range", offset);
    return bytes;
}

std::string module_name(std::size_t index) {
    std::ostringstream name;
    name << std::setw(4) << std::setfill('0') << index << ".gmir";
    return name.str();
}
} // namespace

GmrtWad GmrtWad::load(const std::filesystem::path &path, LoadLimits limits) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
        throw Error(ErrorKind::io, "cannot open GMRT WAD: " + path.string());
    const auto end = stream.tellg();
    if (end < 0)
        throw Error(ErrorKind::io, "cannot determine GMRT WAD size");
    const auto file_length = static_cast<std::uint64_t>(static_cast<std::streamoff>(end));
    if (file_length > limits.max_file_bytes || file_length > std::numeric_limits<std::size_t>::max())
        throw Error(ErrorKind::unsupported, "GMRT WAD exceeds configured file size limit");

    GmrtWad wad;
    wad.path_ = path;
    wad.size_ = static_cast<std::size_t>(file_length);
    if (wad.size_ < 16)
        throw Error(ErrorKind::malformed, "GMRT WAD header is truncated", wad.size_);

    const auto header_bytes = read_range(stream, wad.size_, 0, 16);
    const BinaryView header(header_bytes);
    if (header.text(0, 4) != "FORM")
        throw Error(ErrorKind::malformed, "expected a GMRT FORM container", 0);
    if (static_cast<std::size_t>(header.u32(4)) != wad.size_ - 8)
        throw Error(ErrorKind::malformed, "FORM length does not match GMRT WAD size", 4);

    for (std::size_t offset = 16; offset < wad.size_;) {
        if (wad.chunks_.size() >= limits.max_chunks)
            throw Error(ErrorKind::unsupported, "GMRT WAD chunk count exceeds configured limit", offset);
        if (wad.size_ - offset < 8)
            throw Error(ErrorKind::malformed, "truncated GMRT WAD chunk header", offset);

        const auto chunk_bytes = read_range(stream, wad.size_, offset, 8);
        const BinaryView chunk_header(chunk_bytes, offset);
        GmrtChunk chunk;
        for (std::size_t i = 0; i < chunk.tag.size(); ++i) {
            const auto byte = chunk_header.u8(i);
            if (byte < 0x20 || byte > 0x7e)
                throw Error(ErrorKind::malformed, "invalid GMRT WAD chunk tag", offset + i);
            chunk.tag[i] = static_cast<char>(byte);
        }
        chunk.header_offset = offset;
        chunk.data_offset = offset + 8;
        chunk.size = chunk_header.u32(4);
        if (chunk.size > wad.size_ - chunk.data_offset)
            throw Error(ErrorKind::malformed, "GMRT WAD chunk exceeds file bounds", offset + 4);
        wad.chunks_.push_back(chunk);
        offset = chunk.data_offset + chunk.size;
    }

    const GmrtChunk *scripts = nullptr;
    for (const auto &chunk : wad.chunks_) {
        if (chunk.name() != "SCPT")
            continue;
        if (scripts)
            throw Error(ErrorKind::unsupported, "multiple SCPT chunks are not supported",
                        chunk.header_offset);
        scripts = &chunk;
    }
    if (!scripts)
        throw Error(ErrorKind::unsupported, "GMRT WAD has no SCPT chunk");
    if (scripts->size < 4)
        throw Error(ErrorKind::malformed, "SCPT module table is truncated", scripts->data_offset);

    const auto count_bytes = read_range(stream, wad.size_, scripts->data_offset, 4);
    const auto count = static_cast<std::size_t>(BinaryView(count_bytes, scripts->data_offset).u32(0));
    if (count > limits.max_entries)
        throw Error(ErrorKind::unsupported, "GMRT module count exceeds configured limit",
                    scripts->data_offset);
    if (count > (scripts->size - 4) / 4)
        throw Error(ErrorKind::malformed, "SCPT module offset table is truncated", scripts->data_offset);

    const auto table_size = 4 + count * 4;
    const auto table = read_range(stream, wad.size_, scripts->data_offset, table_size);
    const BinaryView entries(table, scripts->data_offset);
    const auto scripts_end = scripts->data_offset + scripts->size;
    const auto records_begin = scripts->data_offset + table_size;
    std::unordered_set<std::size_t> seen_records;
    wad.modules_.reserve(count);

    for (std::size_t i = 0; i < count; ++i) {
        const auto record_offset = static_cast<std::size_t>(entries.u32(4 + i * 4));
        if (record_offset < records_begin || record_offset > scripts_end || scripts_end - record_offset < 8)
            throw Error(ErrorKind::malformed, "SCPT record offset is outside the record area",
                        scripts->data_offset + 4 + i * 4);
        if (!seen_records.insert(record_offset).second)
            throw Error(ErrorKind::malformed, "duplicate SCPT record offset", record_offset);

        const auto record_bytes = read_range(stream, wad.size_, record_offset, 8);
        const BinaryView record(record_bytes, record_offset);
        GmrtModuleRecord module;
        module.opaque_id = record.u32(0);
        module.record_offset = record_offset;
        module.length_offset = static_cast<std::size_t>(record.u32(4));
        if (module.length_offset < record_offset + 8 || module.length_offset < records_begin ||
            module.length_offset > scripts_end || scripts_end - module.length_offset < 4)
            throw Error(ErrorKind::malformed, "SCPT length offset is outside its chunk", record_offset + 4);

        const auto length_bytes = read_range(stream, wad.size_, module.length_offset, 4);
        module.size = static_cast<std::size_t>(BinaryView(length_bytes, module.length_offset).u32(0));
        module.data_offset = module.length_offset + 4;
        if (module.size > scripts_end - module.data_offset)
            throw Error(ErrorKind::malformed, "GMIR module exceeds SCPT bounds", module.length_offset);
        wad.modules_.push_back(module);
    }

    return wad;
}

std::vector<std::uint8_t> GmrtWad::read_module(std::size_t index) const {
    if (index >= modules_.size())
        throw Error(ErrorKind::usage, "GMIR module index is out of range");
    std::ifstream input(path_, std::ios::binary);
    if (!input)
        throw Error(ErrorKind::io, "cannot reopen GMRT WAD: " + path_.string());
    const auto &module = modules_[index];
    return read_range(input, size_, module.data_offset, module.size);
}

void GmrtWad::extract_modules(const std::filesystem::path &directory) const {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
        throw Error(ErrorKind::io, "cannot create GMIR output directory: " + error.message());

    for (std::size_t i = 0; i < modules_.size(); ++i) {
        const auto output = directory / module_name(i);
        if (std::filesystem::exists(output, error)) {
            if (error)
                throw Error(ErrorKind::io, "cannot inspect GMIR output path: " + error.message());
            throw Error(ErrorKind::io, "refusing to overwrite existing file: " + output.string());
        }
        if (error)
            throw Error(ErrorKind::io, "cannot inspect GMIR output path: " + error.message());
    }

    std::ifstream input(path_, std::ios::binary);
    if (!input)
        throw Error(ErrorKind::io, "cannot reopen GMRT WAD: " + path_.string());
    for (std::size_t i = 0; i < modules_.size(); ++i) {
        const auto &module = modules_[i];
        const auto bytes = read_range(input, size_, module.data_offset, module.size);
        const auto output_path = directory / module_name(i);
        std::ofstream output(output_path, std::ios::binary);
        if (!output)
            throw Error(ErrorKind::io, "cannot create GMIR file: " + output_path.string());
        if (!bytes.empty())
            output.write(reinterpret_cast<const char *>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
        if (!output)
            throw Error(ErrorKind::io, "could not write GMIR file: " + output_path.string());
    }
}
} // namespace regml
