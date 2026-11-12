#include "json.hpp"

#include <cctype>
#include <string>

namespace regml {
namespace {

std::string quote(std::string_view value) {
    std::string out = "\"";
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else if (c == '\t')
            out += "\\t";
        else if (c >= 0x20)
            out += static_cast<char>(c);
    }
    return out + '"';
}

void append_value(std::string &out, const JsonValue &value) {
    if (std::holds_alternative<std::monostate>(value.value))
        out += "null";
    else if (const auto *boolean = std::get_if<bool>(&value.value))
        out += *boolean ? "true" : "false";
    else if (const auto *string = std::get_if<std::string>(&value.value))
        out += quote(*string);
    else if (const auto *number = std::get_if<JsonNumber>(&value.value))
        out += number->text;
    else if (const auto *array = std::get_if<JsonValue::Array>(&value.value)) {
        out += '[';
        for (std::size_t i = 0; i < array->size(); ++i) {
            if (i)
                out += ',';
            append_value(out, (*array)[i]);
        }
        out += ']';
    }
}

} // namespace

std::string json_encode(const JsonObject &object) {
    std::string output = "{";
    bool first = true;
    for (const auto &[key, value] : object) {
        if (!first)
            output += ',';
        first = false;
        output += quote(key);
        output += ':';
        append_value(output, value);
    }
    return output + '}';
}

JsonObject json_decode(std::string_view text) {
    JsonObject object;
    std::size_t i = text.find('{');
    if (i == std::string_view::npos)
        return object;
    ++i;
    auto whitespace = [&] {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
            ++i;
    };
    auto parse_string = [&]() {
        std::string out;
        if (i >= text.size() || text[i++] != '"')
            return out;
        while (i < text.size() && text[i] != '"') {
            char c = text[i++];
            if (c == '\\' && i < text.size()) {
                c = text[i++];
                if (c == 'n')
                    c = '\n';
                else if (c == 'r')
                    c = '\r';
                else if (c == 't')
                    c = '\t';
            }
            out += c;
        }
        if (i < text.size())
            ++i;
        return out;
    };
    whitespace();
    while (i < text.size() && text[i] == '"') {
        auto key = parse_string();
        whitespace();
        if (i >= text.size() || text[i++] != ':')
            break;
        whitespace();
        JsonValue value;
        if (i < text.size() && text[i] == '"')
            value = parse_string();
        else {
            const auto start = i;
            while (i < text.size() && text[i] != ',' && text[i] != '}' &&
                   !std::isspace(static_cast<unsigned char>(text[i])))
                ++i;
            const auto token = text.substr(start, i - start);
            if (token == "true")
                value = true;
            else if (token == "false")
                value = false;
            else if (token == "null")
                value = std::monostate{};
            else {
                try {
                    (void)std::stod(std::string(token));
                    value = JsonNumber{std::string(token)};
                } catch (...) {
                    value = std::string(token);
                }
            }
        }
        object.emplace_back(std::move(key), std::move(value));
        whitespace();
        if (i >= text.size() || text[i++] != ',')
            break;
        whitespace();
    }
    return object;
}

} // namespace regml
