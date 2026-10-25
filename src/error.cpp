#include "error.hpp"
#if REGML_PS2
#include <cstdio>
#endif
#include <utility>
namespace regml {
Error::Error(ErrorKind kind, std::string message, std::optional<std::size_t> offset)
    : std::runtime_error(std::move(message)), kind_(kind), offset_(offset) {
#if REGML_PS2
    if (auto *log = std::fopen("host:reGML/regml-error.log", "a")) {
        std::fprintf(log, "%.*s: %s", static_cast<int>(error_kind_name(kind).size()),
                     error_kind_name(kind).data(), what());
        if (offset)
            std::fprintf(log, " at 0x%zx", *offset);
        std::fputc('\n', log);
        std::fclose(log);
    }
#endif
}
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
