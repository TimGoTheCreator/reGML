#pragma once
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
namespace regml {
enum class ErrorKind { malformed, unsupported, io, vm, usage };
std::string_view error_kind_name(ErrorKind kind) noexcept;
class Error final : public std::runtime_error {
  public:
    Error(ErrorKind kind, std::string message, std::optional<std::size_t> offset = std::nullopt);
    [[nodiscard]] ErrorKind kind() const noexcept { return kind_; }
    [[nodiscard]] std::optional<std::size_t> offset() const noexcept { return offset_; }

  private:
    ErrorKind kind_;
    std::optional<std::size_t> offset_;
};
} // namespace regml
