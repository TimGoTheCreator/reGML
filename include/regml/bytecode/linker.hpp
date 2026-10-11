#pragma once
#include "regml/bytecode/decoder.hpp"
#include "regml/formats/package.hpp"
#include <string>
namespace regml {
struct PackageSymbol {
    std::string name;
    std::int32_t instance_type = 0, variable_id = 0;
    std::uint32_t occurrences = 0, first_address = 0;
};
struct LinkedPackage {
    std::vector<Program> programs;
    std::vector<PackageSymbol> variables, functions;
    // Chain words contain addresses, not symbol IDs.
    std::unordered_map<std::size_t, std::size_t> variable_at, function_at;
    std::size_t instruction_count = 0;
};
// v15/v16 only. Scope and array handling belong to the VM.
LinkedPackage link_legacy_package(const Package &package, LoadLimits limits = {});
} // namespace regml
