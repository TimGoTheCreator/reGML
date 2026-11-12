#pragma once

#include "interpreter.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace regml {

class IniFile {
  public:
    void open_file(std::filesystem::path path);
    void open_from_string(std::string_view contents);
    [[nodiscard]] bool opened() const noexcept { return opened_; }

    [[nodiscard]] Value
    read_real(std::string_view section, std::string_view key, const Value &default_value) const;
    [[nodiscard]] Value
    read_string(std::string_view section, std::string_view key, const Value &default_value) const;
    void write(std::string_view section, std::string_view key, std::string_view value);
    [[nodiscard]] bool section_exists(std::string_view section) const;
    [[nodiscard]] bool key_exists(std::string_view section, std::string_view key) const;
    void delete_section(std::string_view section);
    void delete_key(std::string_view section, std::string_view key);
    void close(const std::function<bool()> &prepare_save_directory);

  private:
    using Section = std::map<std::string, std::string, std::less<>>;
    std::filesystem::path path_;
    std::map<std::string, Section, std::less<>> sections_;
    bool dirty_ = false;
    bool opened_ = false;

    void clear() noexcept;
    void parse_contents(std::string_view contents);
    void parse_line(std::string_view line, std::string &section, Section *&active_section);
};

} // namespace regml
