#include "linker.hpp"
#include <algorithm>
#include <iterator>
#include <limits>
#include <map>
namespace regml {
namespace {
bool variable_reference(const Instruction &ins) {
    return (ins.opcode() == Opcode::pop && ins.type1 != DataType::int16) ||
           ((ins.opcode() == Opcode::push || ins.opcode() == Opcode::push_local ||
             ins.opcode() == Opcode::push_global || ins.opcode() == Opcode::push_builtin) &&
            ins.type1 == DataType::variable);
}
bool optional_function_reference(const Instruction &ins, std::uint8_t version) {
    return version == 17 &&
           ((ins.opcode() == Opcode::push && ins.type1 == DataType::int32) ||
            (ins.opcode() == Opcode::extended && ins.type1 == DataType::int32));
}
bool function_reference(const Instruction &ins, std::uint8_t version) {
    return ins.opcode() == Opcode::call || optional_function_reference(ins, version);
}
} // namespace
LinkedPackage link_legacy_package(const Package &package,
                                  LoadLimits limits,
                                  bool retain_audit_maps,
                                  bool build_instruction_index,
                                  bool lazy_programs) {
    if (!package.general_info() || !package.general_info()->has_known_prefix)
        throw Error(ErrorKind::unsupported, "symbol linking requires known metadata");
    const auto version = package.general_info()->bytecode_version;
    if (version != 15 && version != 16 && version != 17)
        throw Error(ErrorKind::unsupported, "linking requires bytecode 15, 16, or 17");
    const auto &engine = package.general_info()->reported_version;
    if (version != 17 && engine[0] >= 2)
        throw Error(ErrorKind::unsupported, "legacy linker currently requires a GameMaker 1.x profile");
    LinkedPackage result;
    result.shared_code = version == 17;
    struct ProgramRange {
        std::size_t begin, end, index;
    };
    constexpr std::uint32_t reference_variable_flag = 0x40000000U;
    constexpr std::uint32_t reference_site_flag = 0x80000000U;
    constexpr std::uint32_t reference_operand_mask = 0x3fffffffU;
    static_assert(sizeof(LinkedPackage::ReferenceBinding) == 12);
    std::vector<LinkedPackage::ReferenceBinding> reference_bindings;
    const auto variable_count = [&](const Chunk &chunk, const auto &bytes) {
        if (chunk.size < 12)
            throw Error(ErrorKind::malformed, "invalid legacy VARI record range", chunk.data_offset);
        const auto records_size = chunk.size - 12;
        const auto padding_size = records_size % 20;
        if (padding_size != 0) {
            const bool aligned_v17_padding =
                version == 17 && padding_size <= 15 &&
                (chunk.data_offset % 16 + chunk.size % 16) % 16 == 0 &&
                std::all_of(bytes.end() - static_cast<std::ptrdiff_t>(padding_size), bytes.end(),
                            [](std::uint8_t byte) { return byte == 0; });
            if (!aligned_v17_padding)
                throw Error(ErrorKind::malformed, "invalid legacy VARI record range", chunk.data_offset);
        }
        return records_size / 20;
    };
    if (lazy_programs) {
        std::size_t reference_count = 0;
        auto add_occurrences = [&](std::uint32_t occurrences, std::size_t at) {
            constexpr std::size_t max_instructions = 10'000'000;
            if (occurrences > max_instructions - reference_count)
                throw Error(ErrorKind::unsupported, "package reference limit exceeded", at);
            reference_count += occurrences;
        };
        if (const auto *chunk = package.find_chunk("VARI"); chunk && chunk->size) {
            const auto bytes = package.read_bytes(chunk->data_offset, chunk->size);
            const BinaryView data(bytes, chunk->data_offset);
            const auto count = variable_count(*chunk, bytes);
            if (count > limits.max_entries)
                throw Error(ErrorKind::unsupported, "variable count exceeds limit", chunk->data_offset);
            for (std::size_t i = 0; i < count; ++i)
                add_occurrences(data.u32(24 + i * 20), chunk->data_offset + 24 + i * 20);
        }
        if (const auto *chunk = package.find_chunk("FUNC"); chunk && chunk->size) {
            if (chunk->size < 4)
                throw Error(ErrorKind::malformed, "truncated function table", chunk->data_offset);
            const auto bytes = package.read_bytes(chunk->data_offset, chunk->size);
            const BinaryView data(bytes, chunk->data_offset);
            const auto count = static_cast<std::size_t>(data.u32(0));
            if (count > limits.max_entries)
                throw Error(ErrorKind::unsupported, "function count exceeds limit", chunk->data_offset);
            if (count > (chunk->size - 4) / 12)
                throw Error(ErrorKind::malformed, "truncated function table", chunk->data_offset);
            for (std::size_t i = 0; i < count; ++i)
                add_occurrences(data.u32(8 + i * 12), chunk->data_offset + 8 + i * 12);
        }
        // don't let a bad occurrence count trigger a huge allocation before
        // we know those references are in the decoded instructions
        const auto reserve_hint =
            std::min({reference_count, package.code().size() * 64, std::size_t{512 * 1024}});
        reference_bindings.reserve(reserve_hint);
    }
    std::vector<ProgramRange> ranges;
    ranges.reserve(package.code().size());
    result.programs.reserve(package.code().size());
    std::map<std::pair<std::size_t, std::size_t>, std::size_t> shared_blobs;
    if (result.shared_code)
        result.code_to_program.reserve(package.code().size());
    for (std::size_t i = 0; i < package.code().size(); ++i) {
        const auto &entry = package.code()[i];
        if (result.shared_code) {
            const auto key = std::pair{entry.bytecode_offset, entry.bytecode_length};
            if (const auto found = shared_blobs.find(key); found != shared_blobs.end()) {
                result.code_to_program.push_back(found->second);
                continue;
            }
            const auto shared_index = result.programs.size();
            shared_blobs.emplace(key, shared_index);
            result.code_to_program.push_back(shared_index);
            result.program_code_entries.push_back(i);
        }
        const auto bytes = package.code_bytes(i);
        auto program = decode(BinaryView(bytes, entry.bytecode_offset), version,
                              {1'000'000, build_instruction_index});
        result.instruction_count += program.instructions.size();
        // check the total package size too; per-program limits aren't enough
        if (result.instruction_count > 10'000'000)
            throw Error(ErrorKind::unsupported, "package instruction limit exceeded");
        const auto program_index = result.shared_code ? result.code_to_program.back() : i;
        if (program.byte_length)
            ranges.push_back({program.origin, program.origin + program.byte_length, program_index});
        if (lazy_programs) {
            for (const auto &ins : program.instructions) {
                const bool variable_op = variable_reference(ins);
                const bool function_op = function_reference(ins, version);
                if (function_op || variable_op)
                    reference_bindings.push_back({static_cast<std::uint32_t>(program.origin + ins.offset),
                                                  (ins.reference_value() & reference_operand_mask) |
                                                      (variable_op ? reference_variable_flag : 0U) |
                                                      reference_site_flag,
                                                  optional_function_reference(ins, version)});
                if (ins.type1 == DataType::string && ins.has_reference() &&
                    ins.reference_value() >= package.strings().size())
                    throw Error(ErrorKind::malformed, "string immediate is out of range",
                                program.origin + ins.offset + 4);
            }
            Program skeleton;
            skeleton.origin = program.origin;
            skeleton.byte_length = program.byte_length;
            skeleton.bytecode_version = version;
            result.programs.push_back(std::move(skeleton));
        } else {
            result.programs.push_back(std::move(program));
        }
    }
    std::sort(ranges.begin(), ranges.end(),
              [](const auto &left, const auto &right) { return left.begin < right.begin; });
    for (std::size_t i = 1; i < ranges.size(); ++i)
        if (ranges[i].begin < ranges[i - 1].end)
            throw Error(ErrorKind::unsupported, "overlapping CODE blobs require a shared-code profile",
                        ranges[i].begin);
    if (lazy_programs)
        std::sort(reference_bindings.begin(), reference_bindings.end(),
                  [](const auto &left, const auto &right) { return left.address < right.address; });
    auto find_instruction = [&](std::size_t address) -> Instruction * {
        const auto it = std::upper_bound(
            ranges.begin(), ranges.end(), address,
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
            throw Error(ErrorKind::malformed, "symbol occurrence count exceeds instructions",
                        occurrence_field);
        if (symbol.occurrences == 0 && symbol.first_address != 0xffffffffU)
            throw Error(ErrorKind::malformed, "unused symbol must have sentinel first address",
                        occurrence_field + 4);
        return symbol;
    };
    if (const auto *chunk = package.find_chunk("VARI"); chunk && chunk->size) {
        const auto bytes = package.read_bytes(chunk->data_offset, chunk->size);
        const BinaryView data(bytes, chunk->data_offset);
        const auto count = variable_count(*chunk, bytes);
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
        // FUNC local records aren't part of chain resolution
    }
    auto resolve = [&](const std::vector<PackageSymbol> &symbols, bool variable) {
        auto &bindings = variable ? result.variable_at : result.function_at;
        for (std::size_t id = 0; id < symbols.size(); ++id) {
            const auto &symbol = symbols[id];
            std::size_t address = symbol.first_address;
            if (version == 17 && !variable) {
                if (address < 4)
                    throw Error(ErrorKind::malformed, "function reference precedes its instruction", address);
                address -= 4;
            }
            for (std::size_t n = 0; n < symbol.occurrences; ++n) {
                Instruction *instruction = nullptr;
                LinkedPackage::ReferenceBinding *site = nullptr;
                std::uint32_t next = 0;
                if (lazy_programs) {
                    const auto found = std::lower_bound(
                        reference_bindings.begin(), reference_bindings.end(), address,
                        [](const auto &entry, std::size_t key) { return entry.address < key; });
                    if (found != reference_bindings.end() && found->address == address)
                        site = &*found;
                    if (!site)
                        throw Error(ErrorKind::malformed, "reference chain does not target an instruction",
                                    address);
                    if ((site->symbol & reference_site_flag) == 0 ||
                        ((site->symbol & reference_variable_flag) != 0) != variable)
                        throw Error(ErrorKind::malformed, "reference chain targets wrong instruction kind",
                                    address);
                    if (site->symbol < reference_variable_flag)
                        throw Error(ErrorKind::malformed, "reference chain overlaps another occurrence",
                                    address);
                    if (id >= reference_variable_flag)
                        throw Error(ErrorKind::unsupported, "symbol ID exceeds compact reference range",
                                    address);
                    next = site->symbol & reference_operand_mask;
                    site->symbol = static_cast<std::uint32_t>(id);
                } else {
                    instruction = find_instruction(address);
                    if (!instruction)
                        throw Error(ErrorKind::malformed, "reference chain does not target an instruction",
                                    address);
                    const bool variable_op = variable_reference(*instruction);
                    const bool function_op = function_reference(*instruction, version);
                    const bool has_ref = instruction->has_reference() ||
                                         (!variable && optional_function_reference(*instruction, version));
                    if (!has_ref || (variable ? !variable_op : !function_op))
                        throw Error(ErrorKind::malformed, "reference chain targets wrong instruction kind",
                                    address);
                    if (instruction->symbol_id != Instruction::no_symbol)
                        throw Error(ErrorKind::malformed, "reference chain overlaps another occurrence",
                                    address);
                    next = instruction->reference_value();
                    instruction->symbol_id = static_cast<std::uint32_t>(id);
                }
                if (retain_audit_maps)
                    bindings.emplace(address, id);
                if (n + 1 < symbol.occurrences) {
                    next &= 0x07ffffffU;
                    if (next == 0 || next % 4 != 0 || next > package.size() ||
                        address > package.size() - next)
                        throw Error(ErrorKind::malformed, "invalid reference chain displacement",
                                    address + 4);
                    address += next;
                } else {
                    const auto string_id = next & 0xffffffU;
                    if (version != 17 &&
                        (string_id >= package.strings().size() || package.string_at(string_id) != symbol.name))
                        throw Error(ErrorKind::malformed,
                                    "reference chain terminal name does not match symbol", address + 4);
                }
            }
        }
    };
    resolve(result.variables, true);
    resolve(result.functions, false);
    if (lazy_programs) {
        for (const auto &reference : reference_bindings) {
            if (reference.symbol >= reference_variable_flag && !reference.optional_function)
                throw Error(ErrorKind::malformed,
                            "variable or function instruction missing from symbol chains", reference.address);
        }
    } else {
        for (const auto &program : result.programs)
            for (const auto &ins : program.instructions) {
                const auto address = program.origin + ins.offset;
                if (ins.opcode() == Opcode::call && ins.symbol_id == Instruction::no_symbol)
                    throw Error(ErrorKind::malformed, "CALL missing from FUNC chains", address);
                const bool variable_op =
                    (ins.opcode() == Opcode::pop && ins.type1 != DataType::int16) ||
                    ((ins.opcode() == Opcode::push || ins.opcode() == Opcode::push_local ||
                      ins.opcode() == Opcode::push_global || ins.opcode() == Opcode::push_builtin) &&
                     ins.type1 == DataType::variable);
                if (variable_op && ins.symbol_id == Instruction::no_symbol)
                    throw Error(ErrorKind::malformed, "variable instruction missing from VARI chains",
                                address);
                if (ins.type1 == DataType::string && ins.has_reference() &&
                    ins.reference_value() >= package.strings().size())
                    throw Error(ErrorKind::malformed, "string immediate is out of range", address + 4);
            }
    }
    if (lazy_programs) {
        result.program_reference_ranges.resize(result.programs.size());
        result.reference_symbols.reserve(reference_bindings.size());
        std::size_t reference_index = 0;
        for (const auto &range : ranges) {
            if (result.reference_symbols.size() > std::numeric_limits<std::uint32_t>::max())
                throw Error(ErrorKind::unsupported, "compact reference table exceeds 32-bit range");
            auto &program_range = result.program_reference_ranges[range.index];
            program_range.offset = static_cast<std::uint32_t>(result.reference_symbols.size());
            while (reference_index < reference_bindings.size() &&
                   reference_bindings[reference_index].address < range.begin)
                throw Error(ErrorKind::malformed, "reference chain does not target a code entry",
                            reference_bindings[reference_index].address);
            while (reference_index < reference_bindings.size() &&
                   reference_bindings[reference_index].address < range.end) {
                const auto &reference = reference_bindings[reference_index];
                result.reference_addresses.push_back(reference.address);
                result.reference_symbols.push_back(
                    reference.symbol < reference_variable_flag ? reference.symbol : Instruction::no_symbol);
                ++reference_index;
            }
            const auto count = result.reference_symbols.size() - program_range.offset;
            if (count > std::numeric_limits<std::uint32_t>::max())
                throw Error(ErrorKind::unsupported, "per-program reference table exceeds 32-bit range");
            program_range.count = static_cast<std::uint32_t>(count);
        }
        if (reference_index != reference_bindings.size())
            throw Error(ErrorKind::malformed, "reference chain does not target a code entry",
                        reference_bindings[reference_index].address);
        std::vector<LinkedPackage::ReferenceBinding>().swap(reference_bindings);
        result.lazy_programs = true;
        result.release_programs();
    }
    return result;
}
Program &LinkedPackage::program(const Package &package, std::size_t index) {
    const auto program_index = shared_code ? code_to_program.at(index) : index;
    if (lazy_programs) {
        recent_programs[recent_program_cursor] = program_index;
        recent_program_cursor = (recent_program_cursor + 1) % recent_programs.size();
        recent_program_count = std::min(recent_program_count + 1, recent_programs.size());
    }
    auto &target = programs.at(program_index);
    if (!lazy_programs || !target.instructions.empty() || target.byte_length == 0)
        return target;
    const auto code_index = shared_code ? program_code_entries.at(program_index) : index;
    const auto bytes = package.code_bytes(code_index);
    auto decoded = decode(BinaryView(bytes, target.origin), target.bytecode_version, {1'000'000, false});
    if (program_index >= program_reference_ranges.size())
        throw Error(ErrorKind::malformed, "program has no compact reference range", target.origin);
    const auto range = program_reference_ranges[program_index];
    if (range.offset > reference_symbols.size() || range.count > reference_symbols.size() - range.offset)
        throw Error(ErrorKind::malformed, "compact program reference range is invalid", target.origin);
    if (range.offset > reference_addresses.size() || range.count > reference_addresses.size() - range.offset)
        throw Error(ErrorKind::malformed, "compact program reference addresses are invalid", target.origin);
    auto binding = static_cast<std::size_t>(range.offset);
    const auto binding_end = binding + range.count;
    for (auto &ins : decoded.instructions) {
        const auto address = static_cast<std::uint32_t>(decoded.origin + ins.offset);
        if (binding < binding_end && reference_addresses[binding] < address)
            throw Error(ErrorKind::malformed, "compact program reference misses an instruction", address);
        if (binding < binding_end && reference_addresses[binding] == address) {
            const auto symbol = reference_symbols[binding];
            if (symbol != Instruction::no_symbol)
                ins.symbol_id = symbol;
            ++binding;
        } else if ((ins.opcode() == Opcode::call || variable_reference(ins)) &&
                   !optional_function_reference(ins, target.bytecode_version)) {
            throw Error(ErrorKind::malformed, "decoded code is missing a compact symbol reference", address);
        }
    }
    if (binding != binding_end)
        throw Error(ErrorKind::malformed, "decoded code has fewer references than its compact table",
                    target.origin);
    target = std::move(decoded);
    decoded_program_bytes += target.instructions.capacity() * sizeof(Instruction) +
                             target.index_by_word.capacity() * sizeof(std::uint32_t);
    return target;
}
void LinkedPackage::release_program(std::size_t program_index) {
    if (!lazy_programs || program_index >= programs.size())
        return;
    auto &target = programs[program_index];
    const auto bytes = target.instructions.capacity() * sizeof(Instruction) +
                       target.index_by_word.capacity() * sizeof(std::uint32_t);
    std::vector<Instruction>().swap(target.instructions);
    std::vector<std::uint32_t>().swap(target.index_by_word);
    decoded_program_bytes = bytes > decoded_program_bytes ? 0 : decoded_program_bytes - bytes;
}
void LinkedPackage::release_programs(std::span<const std::size_t> active_programs) {
    if (!lazy_programs)
        return;
#if REGML_WII || REGML_LOW_MEMORY
    constexpr std::size_t retained_program_limit = 1024 * 1024;
    if (decoded_program_bytes <= retained_program_limit)
        return;
    std::array<std::size_t, 192> retained{};
    std::size_t retained_count = 0, retained_bytes = 0;
    auto retain = [&](std::size_t index, bool required) {
        if (index >= programs.size() ||
            std::find(retained.begin(), retained.begin() + static_cast<std::ptrdiff_t>(retained_count), index) !=
                retained.begin() + static_cast<std::ptrdiff_t>(retained_count))
            return;
        const auto &program = programs[index];
        const auto bytes = program.instructions.capacity() * sizeof(Instruction) +
                           program.index_by_word.capacity() * sizeof(std::uint32_t);
        if (!bytes || (!required && (retained_bytes >= retained_program_limit ||
                                     bytes > retained_program_limit - retained_bytes)))
            return;
        retained[retained_count++] = index;
        retained_bytes += bytes;
    };
    for (const auto index : active_programs)
        retain(index, true);
    for (std::size_t age = 0; age < recent_program_count; ++age) {
        const auto slot = (recent_program_cursor + recent_programs.size() - 1 - age) % recent_programs.size();
        retain(recent_programs[slot], false);
    }
    std::sort(retained.begin(), retained.begin() + static_cast<std::ptrdiff_t>(retained_count));
    for (std::size_t i = 0; i < programs.size(); ++i)
        if (!programs[i].instructions.empty() &&
            !std::binary_search(retained.begin(), retained.begin() + static_cast<std::ptrdiff_t>(retained_count), i))
            release_program(i);
#else
    for (std::size_t i = 0; i < programs.size(); ++i)
        release_program(i);
#endif
}
} // namespace regml
