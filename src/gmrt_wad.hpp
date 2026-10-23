#pragma once
#include "package.hpp"
#include <array>
#include <filesystem>
#include <vector>

namespace regml {
struct GmrtChunk {
    std::array<char, 4> tag{};
    std::size_t header_offset = 0;
    std::size_t data_offset = 0;
    std::size_t size = 0;

    [[nodiscard]] std::string_view name() const noexcept { return {tag.data(), tag.size()}; }
};

struct GmrtModuleRecord {
    std::uint32_t opaque_id = 0;
    std::size_t record_offset = 0;
    std::size_t length_offset = 0;
    std::size_t data_offset = 0;
    std::size_t size = 0;
};

class GmrtWad {
  public:
    static GmrtWad load(const std::filesystem::path &path, LoadLimits limits = {});

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] const std::vector<GmrtChunk> &chunks() const noexcept { return chunks_; }
    [[nodiscard]] const std::vector<GmrtModuleRecord> &modules() const noexcept { return modules_; }
    [[nodiscard]] std::vector<std::uint8_t> read_module(std::size_t index) const;
    void extract_modules(const std::filesystem::path &directory) const;

  private:
    std::filesystem::path path_;
    std::size_t size_ = 0;
    std::vector<GmrtChunk> chunks_;
    std::vector<GmrtModuleRecord> modules_;
};
} // namespace regml
