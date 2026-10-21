#include "error.hpp"
#include <utility>
namespace regml {
Error::Error(ErrorKind kind, std::string message, std::optional<std::size_t> offset)
    : std::runtime_error(std::move(message)), kind_(kind), offset_(offset) {}
std::string_view error_kind_name(ErrorKind kind) noexcept {
    switch (kind) {
    case ErrorKind::malformed:
        return "malformed package";
    case ErrorKind::unsupported:
        return "unsupported feature";
    case ErrorKind::io:
        return "I/O";
    case ErrorKind::vm:
        return "VM";
    case ErrorKind::usage:
        return "usage";
    }
    return "unknown";
}
} // namespace regml
