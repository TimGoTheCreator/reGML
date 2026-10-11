#include "regml/bytecode/decoder.hpp"
#include <string>
namespace regml {
std::string_view opcode_name(Opcode op) noexcept {
    switch (op) {
    case Opcode::conv:
        return "conv";
    case Opcode::mul:
        return "mul";
    case Opcode::div:
        return "div";
    case Opcode::rem:
        return "rem";
    case Opcode::mod:
        return "mod";
    case Opcode::add:
        return "add";
    case Opcode::sub:
        return "sub";
    case Opcode::bit_and:
        return "and";
    case Opcode::bit_or:
        return "or";
    case Opcode::bit_xor:
        return "xor";
    case Opcode::neg:
        return "neg";
    case Opcode::bit_not:
        return "not";
    case Opcode::shl:
        return "shl";
    case Opcode::shr:
        return "shr";
    case Opcode::cmp:
        return "cmp";
    case Opcode::pop:
        return "pop";
    case Opcode::push_i:
        return "pushi";
    case Opcode::dup:
        return "dup";
    case Opcode::call_v:
        return "callv";
    case Opcode::ret:
        return "ret";
    case Opcode::exit:
        return "exit";
    case Opcode::popz:
        return "popz";
    case Opcode::branch:
        return "b";
    case Opcode::branch_true:
        return "bt";
    case Opcode::branch_false:
        return "bf";
    case Opcode::push_env:
        return "pushenv";
    case Opcode::pop_env:
        return "popenv";
    case Opcode::push:
        return "push";
    case Opcode::push_local:
        return "pushloc";
    case Opcode::push_global:
        return "pushglb";
    case Opcode::push_builtin:
        return "pushbltn";
    case Opcode::call:
        return "call";
    case Opcode::extended:
        return "break";
    }
    return "unknown";
}
std::string_view type_name(DataType type) noexcept {
    switch (type) {
    case DataType::real:
        return "d";
    case DataType::int32:
        return "i";
    case DataType::int64:
        return "l";
    case DataType::boolean:
        return "b";
    case DataType::variable:
        return "v";
    case DataType::string:
        return "s";
    case DataType::int16:
        return "e";
    }
    return "?";
}
Program decode(BinaryView code, std::uint8_t version, DecodeLimits limits) {
    if (version != 15 && version != 16)
        throw Error(ErrorKind::unsupported, "instruction decoding currently supports bytecode 15 and 16 only",
                    code.origin());
    if (code.size() % 4 != 0)
        throw Error(ErrorKind::malformed, "bytecode length is not word aligned", code.origin());
    Program program;
    program.origin = code.origin();
    program.byte_length = code.size();
    program.bytecode_version = version;
    for (std::size_t offset = 0; offset < code.size();) {
        if (program.instructions.size() >= limits.max_instructions)
            throw Error(ErrorKind::unsupported, "instruction count exceeds configured limit", code.origin() + offset);
        Instruction ins;
        ins.offset = offset;
        ins.word = code.u32(offset);
        ins.opcode = static_cast<Opcode>(ins.word >> 24);
        if (opcode_name(ins.opcode) == "unknown")
            throw Error(ErrorKind::unsupported, "unknown bytecode opcode " + std::to_string(ins.word >> 24),
                        code.origin() + offset);
        ins.type1 = static_cast<DataType>((ins.word >> 16) & 0xf);
        ins.type2 = static_cast<DataType>((ins.word >> 20) & 0xf);
        ins.comparison = static_cast<std::uint8_t>((ins.word >> 8) & 0xff);
        ins.extra = static_cast<std::uint8_t>(ins.word & 0xff);
        switch (ins.opcode) {
        case Opcode::push_i:
            if (static_cast<std::uint8_t>((ins.word >> 16) & 0xff) != 15)
                throw Error(ErrorKind::unsupported, "pushi with unexpected operand type", code.origin() + offset);
            ins.immediate =
                static_cast<std::int32_t>(std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(ins.word)));
            break;
        case Opcode::push:
        case Opcode::push_local:
        case Opcode::push_global:
        case Opcode::push_builtin: {
            const auto full_type = static_cast<DataType>((ins.word >> 16) & 0xff);
            ins.type1 = full_type;
            switch (full_type) {
            case DataType::real:
                ins.size = 12;
                ins.immediate = code.f64(offset + 4);
                break;
            case DataType::int32:
                ins.size = 8;
                ins.immediate = code.i32(offset + 4);
                break;
            case DataType::int64:
                ins.size = 12;
                ins.immediate = code.i64(offset + 4);
                break;
            case DataType::variable:
            case DataType::string:
                ins.size = 8;
                ins.reference = code.u32(offset + 4);
                break;
            case DataType::int16:
                ins.immediate = static_cast<std::int32_t>(code.i16(offset));
                break;
            default:
                throw Error(ErrorKind::unsupported, "unsupported push operand type", code.origin() + offset);
            }
            break;
        }
        case Opcode::pop:
            if (ins.type1 != DataType::int16) {
                ins.size = 8;
                ins.reference = code.u32(offset + 4);
            }
            break;
        case Opcode::call:
            ins.size = 8;
            ins.reference = code.u32(offset + 4);
            break;
        case Opcode::extended:
            if (static_cast<DataType>((ins.word >> 16) & 0xff) == DataType::int32) {
                ins.size = 8;
                ins.immediate = code.i32(offset + 4);
            }
            break;
        case Opcode::branch:
        case Opcode::branch_true:
        case Opcode::branch_false:
        case Opcode::push_env:
        case Opcode::pop_env: {
            const auto encoded = ins.word & 0xffffff;
            if (ins.opcode == Opcode::pop_env && encoded == 0xf00000) {
                ins.environment_exit = true;
                break;
            }
            // v15+ encodes a signed 23-bit word displacement relative to this instruction.
            if ((encoded & 0x800000) != 0)
                throw Error(ErrorKind::malformed, "invalid branch sign encoding", code.origin() + offset);
            auto words = static_cast<std::int64_t>(encoded & 0x7fffff);
            if ((encoded & 0x400000) != 0)
                words -= 0x800000;
            const auto target = static_cast<std::int64_t>(offset) + words * 4;
            if (target < 0 || static_cast<std::uint64_t>(target) > code.size())
                throw Error(ErrorKind::malformed, "branch leaves code range", code.origin() + offset);
            ins.branch_target = static_cast<std::size_t>(target);
            break;
        }
        default:
            break;
        }
        code.require(offset, ins.size);
        program.index_by_offset.emplace(offset, program.instructions.size());
        program.instructions.push_back(ins);
        offset += ins.size;
    }
    for (const auto &ins : program.instructions) {
        if (ins.branch_target && *ins.branch_target != program.byte_length &&
            !program.index_by_offset.contains(*ins.branch_target))
            throw Error(ErrorKind::malformed, "branch targets an immediate payload instead of an instruction",
                        code.origin() + ins.offset);
    }
    return program;
}
} // namespace regml
