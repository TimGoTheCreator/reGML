#pragma once
#include "interpreter.hpp"
#include "package.hpp"
#include <filesystem>
#include <optional>
#include <span>
#include <variant>

namespace regml {
enum class GmrtConstantKind : std::uint8_t {
    none = 0,
    real64 = 1,
    int32 = 2,
    int64 = 3,
    boolean = 4,
    string = 5,
    symbol = 6,
    special = 7,
    immutable_global = 8,
    function = 9
};

enum class GmrtValueType : std::uint8_t { gmval = 0, real64 = 1, object = 2, boolean = 3, void_value = 4 };

struct GmrtSourceLocation {
    std::int32_t line = 0;
    std::int32_t column = 0;
    std::string file;
};

struct GmrtConstant {
    GmrtConstantKind kind = GmrtConstantKind::none;
    std::variant<std::monostate, std::int32_t, std::int64_t, double, bool, std::string> value;
};

struct GmrtRegister {
    std::string name;
    GmrtValueType type = GmrtValueType::gmval;
};

struct GmrtInstruction {
    std::size_t binary_offset = 0;
    std::uint8_t opcode = 0;
    std::vector<std::uint16_t> arguments;
    std::optional<GmrtSourceLocation> location;
};

struct GmrtBlock {
    std::size_t binary_offset = 0;
    std::string name;
    std::vector<GmrtInstruction> instructions;
    std::optional<GmrtSourceLocation> location;
};

struct GmrtFunction {
    std::size_t binary_offset = 0;
    std::string symbol;
    std::string global_name;
    std::string alias;
    std::int32_t flags = 0;
    std::int32_t argument_count = 0;
    std::int32_t try_catch_block_count = 0;
    std::uint8_t signature_type = 0;
    bool dynamic_arguments = false;
    std::vector<GmrtRegister> registers;
    std::vector<GmrtConstant> constants;
    std::vector<GmrtBlock> blocks;
    std::optional<GmrtSourceLocation> location;
};

struct GmrtModule {
    std::int32_t language_version = 0;
    std::string name;
    std::vector<GmrtFunction> functions;
    std::vector<std::uint64_t> global_startup_functions;

    static GmrtModule load(const std::filesystem::path &path, LoadLimits limits = {});
    static GmrtModule parse(std::span<const std::uint8_t> bytes);
};

[[nodiscard]] std::string_view gmrt_opcode_name(std::uint8_t opcode) noexcept;
[[nodiscard]] Value gmrt_constant_value(const GmrtConstant &constant);
} // namespace regml
