#pragma once
#include "binary_view.hpp"
#include <algorithm>
#include <limits>
#include <optional>
#include <variant>
#include <vector>
namespace regml {
enum class DataType : std::uint8_t {
    real = 0,
    float32 = 1,
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
    static constexpr std::uint32_t no_symbol = std::numeric_limits<std::uint32_t>::max();
    enum class ImmediateKind : std::uint8_t { none, int32, int64, real32, real64 };
    std::uint64_t immediate_bits = no_symbol;
    std::uint32_t offset = 0, word = 0, symbol_id = no_symbol;
    DataType type1{};
    ImmediateKind immediate_kind = ImmediateKind::none;
    std::uint8_t size = 4;
    std::uint8_t flags = 0;
    [[nodiscard]] Opcode opcode() const noexcept { return static_cast<Opcode>(word >> 24); }
    [[nodiscard]] DataType type2() const noexcept { return static_cast<DataType>((word >> 20) & 0xf); }
    [[nodiscard]] std::uint8_t comparison() const noexcept { return static_cast<std::uint8_t>(word >> 8); }
    [[nodiscard]] std::uint8_t extra() const noexcept { return static_cast<std::uint8_t>(word); }
    [[nodiscard]] bool has_reference() const noexcept { return (flags & 1U) != 0; }
    [[nodiscard]] std::uint32_t reference_value() const noexcept {
        return static_cast<std::uint32_t>(immediate_bits);
    }
    [[nodiscard]] bool has_branch_target() const noexcept {
        return (opcode() == Opcode::branch || opcode() == Opcode::branch_true ||
                opcode() == Opcode::branch_false || opcode() == Opcode::push_env ||
                opcode() == Opcode::pop_env) &&
               has_reference();
    }
    [[nodiscard]] std::uint32_t branch_target_value() const noexcept { return reference_value(); }
    [[nodiscard]] bool environment_exit() const noexcept { return (flags & 2U) != 0; }
    void set_environment_exit() noexcept { flags |= 2U; }
    void set_operand(std::uint32_t value) noexcept {
        immediate_bits = value;
        flags |= 1U;
    }
    [[nodiscard]] bool has_immediate() const noexcept { return immediate_kind != ImmediateKind::none; }
    void set_immediate(std::int32_t value) noexcept {
        immediate_bits = static_cast<std::uint32_t>(value);
        immediate_kind = ImmediateKind::int32;
    }
    void set_immediate(std::int64_t value) noexcept {
        immediate_bits = static_cast<std::uint64_t>(value);
        immediate_kind = ImmediateKind::int64;
    }
    void set_immediate(double value) noexcept {
        immediate_bits = std::bit_cast<std::uint64_t>(value);
        immediate_kind = ImmediateKind::real64;
    }
    void set_immediate(float value) noexcept {
        immediate_bits = std::bit_cast<std::uint32_t>(value);
        immediate_kind = ImmediateKind::real32;
    }
    [[nodiscard]] std::int32_t immediate_i32() const noexcept {
        return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(immediate_bits));
    }
    [[nodiscard]] std::int64_t immediate_i64() const noexcept {
        return std::bit_cast<std::int64_t>(immediate_bits);
    }
    [[nodiscard]] double immediate_real() const noexcept {
        return immediate_kind == ImmediateKind::real32
                   ? static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(immediate_bits)))
                   : std::bit_cast<double>(immediate_bits);
    }
};
static_assert(sizeof(Instruction) == 24);
struct Program {
    std::size_t origin = 0, byte_length = 0;
    std::uint8_t bytecode_version = 0;
    std::vector<Instruction> instructions;
    // code lines up on word boundaries, so one flat table is simpler than a hash node per opcode
    std::vector<std::uint32_t> index_by_word;
    [[nodiscard]] std::optional<std::size_t> instruction_at(std::size_t offset) const noexcept {
        if (index_by_word.empty()) {
            const auto it =
                std::lower_bound(instructions.begin(), instructions.end(), offset,
                                 [](const Instruction &ins, std::size_t at) { return ins.offset < at; });
            return it != instructions.end() && it->offset == offset
                       ? std::optional<std::size_t>(static_cast<std::size_t>(it - instructions.begin()))
                       : std::nullopt;
        }
        if (offset % 4 != 0 || offset / 4 >= index_by_word.size())
            return std::nullopt;
        const auto index = index_by_word[offset / 4];
        return index == std::numeric_limits<std::uint32_t>::max() ? std::nullopt
                                                                  : std::optional<std::size_t>(index);
    }
};
// bcode 17 is decoded for inspections: the game (VM) remains 1.x (for now?)
struct DecodeLimits {
    std::size_t max_instructions = 1'000'000;
    bool build_index = true;
};
Program decode(BinaryView code, std::uint8_t bytecode_version, DecodeLimits limits = {});
} // namespace regml
