#pragma once
#include "binary_view.hpp"
#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>
namespace regml {
struct LoadLimits {
    std::size_t max_file_bytes = 1024ULL * 1024 * 1024;
    std::size_t max_chunks = 4096;
    std::size_t max_entries = 1'000'000;
};
struct Chunk {
    std::array<char, 4> tag{};
    std::size_t header_offset = 0, data_offset = 0, size = 0;
    [[nodiscard]] std::string_view name() const noexcept { return {tag.data(), tag.size()}; }
};
struct GeneralInfo {
    bool debugger_disabled = false;
    std::uint8_t bytecode_version = 0;
    std::array<std::uint32_t, 4> reported_version{};
    std::uint32_t game_id = 0, name_pointer = 0, display_name_pointer = 0, width = 0, height = 0;
    bool has_known_prefix = false;
};
struct StringRecord {
    std::uint32_t record_offset = 0, length = 0;
};
struct CodeEntry {
    std::size_t record_offset = 0;
    std::uint32_t name_pointer = 0;
    std::size_t bytecode_offset = 0, bytecode_length = 0;
    std::uint16_t locals = 0, arguments = 0;
    bool locals_flag = false;
    std::uint32_t entry_offset = 0;
};
class Package {
  public:
    static Package load(const std::filesystem::path &path, LoadLimits limits = {});
    static Package from_bytes(std::vector<std::uint8_t> bytes, LoadLimits limits = {});
    [[nodiscard]] BinaryView view() const {
        if (bytes_.size() != size_)
            throw Error(ErrorKind::unsupported, "file-backed package has no contiguous view; use read_bytes");
        return BinaryView(bytes_);
    }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::vector<std::uint8_t> read_bytes(std::size_t offset, std::size_t length) const;
    void read_into(std::size_t offset, std::span<std::uint8_t> output) const;
    [[nodiscard]] const std::vector<Chunk> &chunks() const noexcept { return chunks_; }
    [[nodiscard]] const Chunk *find_chunk(std::string_view name) const noexcept;
    [[nodiscard]] const std::optional<GeneralInfo> &general_info() const noexcept { return info_; }
    [[nodiscard]] const std::vector<StringRecord> &strings() const noexcept { return strings_; }
    [[nodiscard]] const std::vector<CodeEntry> &code() const noexcept { return code_; }
    [[nodiscard]] std::string_view string_at(std::size_t index) const;
    [[nodiscard]] std::string_view string_pointer(std::uint32_t pointer) const;
    [[nodiscard]] std::vector<std::uint8_t> code_bytes(std::size_t index) const;
    [[nodiscard]] std::string_view code_name(std::size_t index) const;

  private:
    struct FileReader;
    std::shared_ptr<FileReader> reader_;
    void parse(LoadLimits limits);
    void parse_strings(const Chunk &chunk, LoadLimits limits);
    void parse_info(const Chunk &chunk);
    void parse_code(const Chunk &chunk, LoadLimits limits);
    [[nodiscard]] std::size_t list_count(const Chunk &chunk, LoadLimits limits) const;
    std::vector<std::uint8_t> bytes_;
    std::size_t string_chunk_offset_ = 0;
    std::filesystem::path source_path_;
    std::size_t size_ = 0;
    std::vector<Chunk> chunks_;
    std::optional<GeneralInfo> info_;
    std::vector<StringRecord> strings_;
    std::vector<std::uint8_t> string_chunk_;
    std::vector<char> string_data_;
    std::vector<std::size_t> string_data_offsets_;
    // STRG record indices are enough to build the sorted pointer lookup. Keeping
    // another pointer beside every index wastes 4 bytes per string.
    std::vector<std::uint32_t> string_lookup_;
    std::vector<CodeEntry> code_;
};
} // namespace regml
