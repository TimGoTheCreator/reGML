#pragma once
#include "regml/core/binary_view.hpp"
#include <optional>
#include <unordered_map>
#include <variant>
#include <vector>
namespace regml {
enum class DataType : std::uint8_t {
    real = 0,
    int32 = 2,
    int64 = 3,
    boolean = 4,
    variable = 5,
    string = 6,
    int16 = 15
};
enum class Opcode : std::uint8_t {
    conv = 0x07,
    mul = 0x08,
    div = 0x09,
    rem = 0x0a,
    mod = 0x0b,
    add = 0x0c,
    sub = 0x0d,
    bit_and = 0x0e,
    bit_or = 0x0f,
    bit_xor = 0x10,
    neg = 0x11,
    bit_not = 0x12,
    shl = 0x13,
    shr = 0x14,
    cmp = 0x15,
    pop = 0x45,
    push_i = 0x84,
    dup = 0x86,
    call_v = 0x99,
    ret = 0x9c,
    exit = 0x9d,
    popz = 0x9e,
    branch = 0xb6,
    branch_true = 0xb7,
    branch_false = 0xb8,
    push_env = 0xba,
    pop_env = 0xbb,
    push = 0xc0,
    push_local = 0xc1,
    push_global = 0xc2,
    push_builtin = 0xc3,
    call = 0xd9,
    extended = 0xff
};
std::string_view opcode_name(Opcode opcode) noexcept;
std::string_view type_name(DataType type) noexcept;
struct Instruction {
    static constexpr std::size_t no_symbol = static_cast<std::size_t>(-1);
    std::size_t offset = 0, size = 4;
    std::uint32_t word = 0;
    Opcode opcode{};
    DataType type1{}, type2{};
    std::uint8_t comparison = 0, extra = 0;
    // Symbol words are left for the linker.
    std::variant<std::monostate, std::int32_t, std::int64_t, double> immediate;
    std::optional<std::uint32_t> reference;
    std::size_t variable_id = no_symbol, function_id = no_symbol;
    std::optional<std::size_t> branch_target;
    bool environment_exit = false;
};
struct Program {
    std::size_t origin = 0, byte_length = 0;
    std::uint8_t bytecode_version = 0;
    std::vector<Instruction> instructions;
    std::unordered_map<std::size_t, std::size_t> index_by_offset;
};
// v15/v16 only.
struct DecodeLimits {
    std::size_t max_instructions = 1'000'000;
};
Program decode(BinaryView code, std::uint8_t bytecode_version, DecodeLimits limits = {});
} // namespace regml
