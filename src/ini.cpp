#include "ini.hpp"

#include "error.hpp"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string_view>

namespace regml {

void IniFile::clear() noexcept {
    path_.clear();
    sections_.clear();
    dirty_ = false;
    opened_ = false;
}

void IniFile::parse_line(std::string_view line, std::string &section, Section *&active_section) {
    if (!line.empty() && line.back() == '\r')
        line.remove_suffix(1);
    if (line.size() > 1 && line.front() == '[' && line.back() == ']') {
        section.assign(line.data() + 1, line.size() - 2);
        const auto section_it = sections_.find(section);
        active_section = section_it == sections_.end() ? nullptr : &section_it->second;
    } else if (const auto equals = line.find('='); equals != std::string_view::npos) {
        const auto key = line.substr(0, equals);
        const auto value = line.substr(equals + 1);
        if (!active_section) {
            auto section_it = sections_.find(section);
            if (section_it == sections_.end())
                section_it = sections_.try_emplace(section).first;
            active_section = &section_it->second;
        }

        auto entry_it = active_section->find(key);
        if (entry_it == active_section->end())
            active_section->emplace(std::string(key), std::string(value));
        else
            entry_it->second.assign(value);
    }
}

void IniFile::parse_contents(std::string_view contents) {
    std::string section;
    Section *active_section = nullptr;
    std::size_t line_start = 0;
    while (line_start < contents.size()) {
        const auto newline = contents.find('\n', line_start);
        const auto line_end = newline == std::string_view::npos ? contents.size() : newline;
        parse_line(contents.substr(line_start, line_end - line_start), section, active_section);
        if (newline == std::string_view::npos)
            return;
        line_start = newline + 1;
    }
}

void IniFile::open_file(std::filesystem::path path) {
    path_ = std::move(path);
    sections_.clear();
    dirty_ = false;
    std::ifstream file(path_);
    const std::string contents{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    parse_contents(contents);
    opened_ = true;
}

void IniFile::open_from_string(std::string_view contents) {
    path_.clear();
    sections_.clear();
    dirty_ = false;
    parse_contents(contents);
    opened_ = true;
}

Value IniFile::read_real(std::string_view section, std::string_view key, const Value &default_value) const {
    if (!opened_)
        throw Error(ErrorKind::vm, "INI read without open");
    const auto section_it = sections_.find(section);
    if (section_it == sections_.end())
        return default_value;
    const auto entry_it = section_it->second.find(key);
    if (entry_it == section_it->second.end())
        return default_value;
    const auto &value = entry_it->second;
    const char *begin = value.c_str();
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        ++begin;

    errno = 0;
    char *end = nullptr;
    const double result = std::strtod(begin, &end);
    if (end == begin || errno == ERANGE)
        return default_value;
    return result;
}

Value IniFile::read_string(std::string_view section, std::string_view key, const Value &default_value) const {
    if (!opened_)
        throw Error(ErrorKind::vm, "INI read without open");
    const auto section_it = sections_.find(section);
    if (section_it == sections_.end())
        return default_value;
    const auto entry_it = section_it->second.find(key);
    if (entry_it == section_it->second.end())
        return default_value;
    const auto &value = entry_it->second;
    const auto view = value.size() >= 2 && value.front() == '"' && value.back() == '"'
                          ? std::string_view(value).substr(1, value.size() - 2)
                          : std::string_view(value);
    return std::string(view);
}

void IniFile::write(std::string_view section, std::string_view key, std::string_view value) {
    if (!opened_)
        throw Error(ErrorKind::vm, "INI write without open");

    auto section_it = sections_.find(section);
    if (section_it == sections_.end())
        section_it = sections_.try_emplace(std::string(section)).first;
    auto entry_it = section_it->second.find(key);
    if (entry_it == section_it->second.end())
        section_it->second.emplace(std::string(key), std::string(value));
    else
        entry_it->second.assign(value);
    dirty_ = true;
}

bool IniFile::section_exists(std::string_view section) const {
    return sections_.contains(section);
}

bool IniFile::key_exists(std::string_view section, std::string_view key) const {
    const auto section_it = sections_.find(section);
    return section_it != sections_.end() && section_it->second.contains(key);
}

void IniFile::delete_section(std::string_view section) {
    if (const auto it = sections_.find(section); it != sections_.end())
        sections_.erase(it);
    dirty_ = true;
}

void IniFile::delete_key(std::string_view section, std::string_view key) {
    if (auto section_it = sections_.find(section); section_it != sections_.end()) {
        if (const auto key_it = section_it->second.find(key); key_it != section_it->second.end())
            section_it->second.erase(key_it);
    }
    dirty_ = true;
}

void IniFile::close(const std::function<bool()> &prepare_save_directory) {
    if (dirty_ && !path_.empty()) {
        if (!prepare_save_directory()) {
            clear();
            return;
        }
        std::ofstream file(path_);
        if (file) {
            std::string contents;
            for (const auto &[section, entries] : sections_) {
                contents += '[';
                contents += section;
                contents += "]\n";
                for (const auto &[key, value] : entries) {
                    contents += key;
                    contents += '=';
                    contents += value;
                    contents += '\n';
                }
            }
            file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        }
    }
    clear();
}

} // namespace regml
