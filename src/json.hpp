#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace regml {

struct JsonNumber {
    std::string text;
};

struct JsonValue {
    using Array = std::vector<JsonValue>;
    std::variant<std::monostate, bool, std::string, JsonNumber, Array> value;

    JsonValue() = default;
    template <class T> JsonValue(T item) : value(std::move(item)) {}
};

using JsonObject = std::vector<std::pair<std::string, JsonValue>>;

[[nodiscard]] std::string json_encode(const JsonObject &object);
[[nodiscard]] JsonObject json_decode(std::string_view text);

} // namespace regml
