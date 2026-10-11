#include "regml/bytecode/linker.hpp"
#include <limits>
namespace regml {
LinkedPackage link_legacy_package(const Package &package, LoadLimits limits) {
    if (!package.general_info() || !package.general_info()->has_known_prefix)
        throw Error(ErrorKind::unsupported, "symbol linking requires known metadata");
    const auto version = package.general_info()->bytecode_version;
    if (version != 15 && version != 16)
        throw Error(ErrorKind::unsupported, "legacy linking requires bytecode 15/16");
    const auto &engine = package.general_info()->reported_version;
    if (engine[0] >= 2)
        throw Error(ErrorKind::unsupported, "legacy linker currently requires a GameMaker 1.x profile");
    LinkedPackage result;
    std::unordered_map<std::size_t, const Instruction *> instructions;
    result.programs.reserve(package.code().size());
    for (std::size_t i = 0; i < package.code().size(); ++i) {
        result.programs.push_back(decode(package.code_bytes(i), version));
        const auto &program = result.programs.back();
        result.instruction_count += program.instructions.size();
        // Decoder limits apply per program; bound the package total too.
        if (result.instruction_count > 10'000'000)
            throw Error(ErrorKind::unsupported, "package instruction limit exceeded");
        for (const auto &ins : program.instructions) {
            if (!instructions.emplace(program.origin + ins.offset, &ins).second)
                throw Error(ErrorKind::unsupported, "overlapping CODE blobs require a shared-code profile",
                            program.origin + ins.offset);
        }
    }
    const auto data = package.view();
    auto read_symbol = [&](std::size_t at, bool variable) {
        PackageSymbol symbol;
        symbol.name = package.string_pointer(data.u32(at));
        if (symbol.name.empty())
            throw Error(ErrorKind::malformed, "empty symbol name", at);
        if (variable) {
            symbol.instance_type = data.i32(at + 4);
            symbol.variable_id = data.i32(at + 8);
        }
        const auto occurrence_field = at + (variable ? 12U : 4U);
        symbol.occurrences = data.u32(occurrence_field);
        symbol.first_address = data.u32(occurrence_field + 4);
        if (symbol.occurrences > result.instruction_count)
            throw Error(ErrorKind::malformed, "symbol occurrence count exceeds instructions", occurrence_field);
        if (symbol.occurrences == 0 && symbol.first_address != 0xffffffffU)
            throw Error(ErrorKind::malformed, "unused symbol must have sentinel first address", occurrence_field + 4);
        return symbol;
    };
    if (const auto *chunk = package.find_chunk("VARI"); chunk && chunk->size) {
        if (chunk->size < 12 || (chunk->size - 12) % 20 != 0)
            throw Error(ErrorKind::malformed, "invalid legacy VARI record range", chunk->data_offset);
        const auto count = (chunk->size - 12) / 20;
        if (count > limits.max_entries)
            throw Error(ErrorKind::unsupported, "variable count exceeds limit", chunk->data_offset);
        for (std::size_t i = 0; i < count; ++i)
            result.variables.push_back(read_symbol(chunk->data_offset + 12 + i * 20, true));
    }
    if (const auto *chunk = package.find_chunk("FUNC"); chunk && chunk->size) {
        const auto local = data.slice(chunk->data_offset, chunk->size);
        const auto count = static_cast<std::size_t>(local.u32(0));
        if (count > limits.max_entries)
            throw Error(ErrorKind::unsupported, "function count exceeds limit", chunk->data_offset);
        if (count > (chunk->size - 4) / 12)
            throw Error(ErrorKind::malformed, "truncated function table", chunk->data_offset);
        for (std::size_t i = 0; i < count; ++i)
            result.functions.push_back(read_symbol(chunk->data_offset + 4 + i * 12, false));
        // FUNC local records do not affect chain resolution.
    }
    auto resolve = [&](const std::vector<PackageSymbol> &symbols, bool variable) {
        auto &bindings = variable ? result.variable_at : result.function_at;
        for (std::size_t id = 0; id < symbols.size(); ++id) {
            const auto &symbol = symbols[id];
            std::size_t address = symbol.first_address;
            for (std::size_t n = 0; n < symbol.occurrences; ++n) {
                const auto it = instructions.find(address);
                if (it == instructions.end())
                    throw Error(ErrorKind::malformed, "reference chain does not target an instruction", address);
                const auto &ins = *it->second;
                const bool variable_op = ins.opcode == Opcode::pop ||
                                         ((ins.opcode == Opcode::push || ins.opcode == Opcode::push_local ||
                                           ins.opcode == Opcode::push_global || ins.opcode == Opcode::push_builtin) &&
                                          ins.type1 == DataType::variable);
                if (!ins.reference || (variable ? !variable_op : ins.opcode != Opcode::call))
                    throw Error(ErrorKind::malformed, "reference chain targets wrong instruction kind", address);
                if (!bindings.emplace(address, id).second)
                    throw Error(ErrorKind::malformed, "reference chain overlaps another occurrence", address);
                const auto next = *ins.reference & 0x07ffffffU;
                if (n + 1 < symbol.occurrences) {
                    if (next == 0 || next % 4 != 0 || next > package.size() || address > package.size() - next)
                        throw Error(ErrorKind::malformed, "invalid reference chain displacement", address + 4);
                    address += next;
                } else {
                    const auto string_id = next & 0xffffffU;
                    if (string_id >= package.strings().size() || package.string_at(string_id) != symbol.name)
                        throw Error(ErrorKind::malformed, "reference chain terminal name does not match symbol",
                                    address + 4);
                }
            }
        }
    };
    resolve(result.variables, true);
    resolve(result.functions, false);
    for (const auto &[address, ins] : instructions) {
        if (ins->opcode == Opcode::call && !result.function_at.contains(address))
            throw Error(ErrorKind::malformed, "CALL missing from FUNC chains", address);
        const bool variable_op = (ins->opcode == Opcode::pop && ins->type1 != DataType::int16) ||
                                 ((ins->opcode == Opcode::push || ins->opcode == Opcode::push_local ||
                                   ins->opcode == Opcode::push_global || ins->opcode == Opcode::push_builtin) &&
                                  ins->type1 == DataType::variable);
        if (variable_op && !result.variable_at.contains(address))
            throw Error(ErrorKind::malformed, "variable instruction missing from VARI chains", address);
        if (ins->type1 == DataType::string && ins->reference && *ins->reference >= package.strings().size())
            throw Error(ErrorKind::malformed, "string immediate is out of range", address + 4);
    }
    return result;
}
} // namespace regml
