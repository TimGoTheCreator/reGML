#include "linker.hpp"
#include <algorithm>
#include <iterator>
#include <limits>
namespace regml {
LinkedPackage link_legacy_package(const Package &package, LoadLimits limits, bool retain_audit_maps,
                                  bool build_instruction_index, bool lazy_programs) {
    if (!package.general_info() || !package.general_info()->has_known_prefix)
        throw Error(ErrorKind::unsupported, "symbol linking requires known metadata");
    const auto version = package.general_info()->bytecode_version;
    if (version != 15 && version != 16)
        throw Error(ErrorKind::unsupported, "legacy linking requires bytecode 15/16");
    const auto &engine = package.general_info()->reported_version;
    if (engine[0] >= 2)
        throw Error(ErrorKind::unsupported, "legacy linker currently requires a GameMaker 1.x profile");
    LinkedPackage result;
    struct ProgramRange {
        std::size_t begin, end, index;
    };
    std::vector<ProgramRange> ranges;
    ranges.reserve(package.code().size());
    result.programs.reserve(package.code().size());
    for (std::size_t i = 0; i < package.code().size(); ++i) {
        const auto bytes = package.code_bytes(i);
        result.programs.push_back(decode(BinaryView(bytes, package.code()[i].bytecode_offset), version,
                                         {1'000'000, build_instruction_index}));
        auto &program = result.programs.back();
        result.instruction_count += program.instructions.size();
        // Decoder limits apply per program; bound the package total too.
        if (result.instruction_count > 10'000'000)
            throw Error(ErrorKind::unsupported, "package instruction limit exceeded");
        if (program.byte_length)
            ranges.push_back({program.origin, program.origin + program.byte_length, i});
    }
    std::sort(ranges.begin(), ranges.end(),
              [](const auto &left, const auto &right) { return left.begin < right.begin; });
    for (std::size_t i = 1; i < ranges.size(); ++i)
        if (ranges[i].begin < ranges[i - 1].end)
            throw Error(ErrorKind::unsupported, "overlapping CODE blobs require a shared-code profile",
                        ranges[i].begin);
    auto find_instruction = [&](std::size_t address) -> Instruction * {
        const auto it =
            std::upper_bound(ranges.begin(), ranges.end(), address,
                             [](std::size_t value, const ProgramRange &range) { return value < range.begin; });
        if (it == ranges.begin())
            return nullptr;
        const auto &range = *std::prev(it);
        if (address >= range.end)
            return nullptr;
        const auto index = result.programs[range.index].instruction_at(address - range.begin);
        return index ? &result.programs[range.index].instructions[*index] : nullptr;
    };
    auto read_symbol = [&](const BinaryView &data, std::size_t at, bool variable) {
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
        const auto bytes = package.read_bytes(chunk->data_offset, chunk->size);
        const BinaryView data(bytes, chunk->data_offset);
        const auto count = (chunk->size - 12) / 20;
        if (count > limits.max_entries)
            throw Error(ErrorKind::unsupported, "variable count exceeds limit", chunk->data_offset);
        for (std::size_t i = 0; i < count; ++i)
            result.variables.push_back(read_symbol(data, 12 + i * 20, true));
    }
    if (const auto *chunk = package.find_chunk("FUNC"); chunk && chunk->size) {
        const auto bytes = package.read_bytes(chunk->data_offset, chunk->size);
        const BinaryView local(bytes, chunk->data_offset);
        const auto count = static_cast<std::size_t>(local.u32(0));
        if (count > limits.max_entries)
            throw Error(ErrorKind::unsupported, "function count exceeds limit", chunk->data_offset);
        if (count > (chunk->size - 4) / 12)
            throw Error(ErrorKind::malformed, "truncated function table", chunk->data_offset);
        for (std::size_t i = 0; i < count; ++i)
            result.functions.push_back(read_symbol(local, 4 + i * 12, false));
        // FUNC local records do not affect chain resolution.
    }
    auto resolve = [&](const std::vector<PackageSymbol> &symbols, bool variable) {
        auto &bindings = variable ? result.variable_at : result.function_at;
        for (std::size_t id = 0; id < symbols.size(); ++id) {
            const auto &symbol = symbols[id];
            std::size_t address = symbol.first_address;
            for (std::size_t n = 0; n < symbol.occurrences; ++n) {
                auto *instruction = find_instruction(address);
                if (!instruction)
                    throw Error(ErrorKind::malformed, "reference chain does not target an instruction", address);
                auto &ins = *instruction;
                const bool variable_op =
                    ins.opcode() == Opcode::pop ||
                    ((ins.opcode() == Opcode::push || ins.opcode() == Opcode::push_local ||
                      ins.opcode() == Opcode::push_global || ins.opcode() == Opcode::push_builtin) &&
                     ins.type1 == DataType::variable);
                if (!ins.has_reference() || (variable ? !variable_op : ins.opcode() != Opcode::call))
                    throw Error(ErrorKind::malformed, "reference chain targets wrong instruction kind", address);
                if (ins.symbol_id != Instruction::no_symbol)
                    throw Error(ErrorKind::malformed, "reference chain overlaps another occurrence", address);
                if (retain_audit_maps)
                    bindings.emplace(address, id);
                ins.symbol_id = static_cast<std::uint32_t>(id);
                const auto next = ins.reference_value() & 0x07ffffffU;
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
    for (const auto &program : result.programs)
        for (const auto &ins : program.instructions) {
            const auto address = program.origin + ins.offset;
            if (ins.opcode() == Opcode::call && ins.symbol_id == Instruction::no_symbol)
                throw Error(ErrorKind::malformed, "CALL missing from FUNC chains", address);
            const bool variable_op = (ins.opcode() == Opcode::pop && ins.type1 != DataType::int16) ||
                                     ((ins.opcode() == Opcode::push || ins.opcode() == Opcode::push_local ||
                                       ins.opcode() == Opcode::push_global || ins.opcode() == Opcode::push_builtin) &&
                                      ins.type1 == DataType::variable);
            if (variable_op && ins.symbol_id == Instruction::no_symbol)
                throw Error(ErrorKind::malformed, "variable instruction missing from VARI chains", address);
            if (ins.type1 == DataType::string && ins.has_reference() &&
                ins.reference_value() >= package.strings().size())
                throw Error(ErrorKind::malformed, "string immediate is out of range", address + 4);
        }
    if (lazy_programs) {
        result.lazy_programs = true;
        std::size_t count = 0;
        for (const auto &symbol : result.variables)
            count += symbol.occurrences;
        for (const auto &symbol : result.functions)
            count += symbol.occurrences;
        result.references.reserve(count);
        for (const auto &program : result.programs)
            for (const auto &ins : program.instructions)
                if (ins.symbol_id != Instruction::no_symbol)
                    result.references.push_back(
                        {static_cast<std::uint32_t>(program.origin + ins.offset), ins.symbol_id});
        std::sort(result.references.begin(), result.references.end(),
                  [](const auto &left, const auto &right) { return left.address < right.address; });
        result.release_programs();
    }
    return result;
}
Program &LinkedPackage::program(const Package &package, std::size_t index) {
    auto &target = programs.at(index);
    if (!lazy_programs || !target.instructions.empty() || target.byte_length == 0)
        return target;
    const auto bytes = package.code_bytes(index);
    auto decoded = decode(BinaryView(bytes, target.origin), target.bytecode_version, {1'000'000, false});
    auto binding =
        std::lower_bound(references.begin(), references.end(), target.origin,
                         [](const ReferenceBinding &ref, std::size_t address) { return ref.address < address; });
    for (auto &ins : decoded.instructions) {
        if (binding != references.end() && binding->address == decoded.origin + ins.offset) {
            ins.symbol_id = binding->symbol;
            ++binding;
        }
    }
    target = std::move(decoded);
    decoded_program_bytes += target.instructions.capacity() * sizeof(Instruction) +
                             target.index_by_word.capacity() * sizeof(std::uint32_t);
    return target;
}
void LinkedPackage::release_programs() {
    if (!lazy_programs)
        return;
#if REGML_WII
    constexpr std::size_t retained_program_limit = 1024 * 1024;
    if (decoded_program_bytes <= retained_program_limit)
        return;
#endif
    for (auto &program : programs) {
        std::vector<Instruction>().swap(program.instructions);
        std::vector<std::uint32_t>().swap(program.index_by_word);
    }
    decoded_program_bytes = 0;
}
} // namespace regml
