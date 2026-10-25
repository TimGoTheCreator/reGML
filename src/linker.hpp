#pragma once
#include "decoder.hpp"
#include "package.hpp"
#include <array>
#include <span>
#include <string>
#include <unordered_map>
namespace regml {
struct PackageSymbol {
    std::string name;
    std::int32_t instance_type = 0, variable_id = 0;
    std::uint32_t occurrences = 0, first_address = 0;
};
struct LinkedPackage {
    struct ReferenceBinding {
        std::uint32_t address, symbol;
        bool optional_function = false;
    };
    struct ProgramReferenceRange {
        std::uint32_t offset = 0, count = 0;
    };
    std::vector<Program> programs;
    // v17 puts several code entries in one shared CODE blob
    // these maps share the decoded program but keep each code entry's index
    std::vector<std::size_t> code_to_program;
    std::vector<std::size_t> program_code_entries;
    bool shared_code = false;
    std::vector<std::uint32_t> reference_symbols;
    std::vector<std::uint32_t> reference_addresses;
    std::vector<ProgramReferenceRange> program_reference_ranges;
    bool lazy_programs = false;
    std::size_t decoded_program_bytes = 0;
    std::array<std::size_t, 64> recent_programs{};
    std::size_t recent_program_count = 0, recent_program_cursor = 0;
    Program &program(const Package &package, std::size_t index);
    void release_program(std::size_t program_index);
    void release_programs(std::span<const std::size_t> active_programs = {});
    std::vector<PackageSymbol> variables, functions;
    // chain words store addresses, not symbol IDs
    std::unordered_map<std::size_t, std::size_t> variable_at, function_at;
    std::size_t instruction_count = 0;
};
// link package symbols for bytecode 15, 16, and 17
LinkedPackage link_legacy_package(const Package &package,
                                  LoadLimits limits = {},
                                  bool retain_audit_maps = true,
                                  bool build_instruction_index = true,
                                  bool lazy_programs = false);
} // namespace regml
