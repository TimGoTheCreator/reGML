#pragma once
#include "gmrt_ir.hpp"
#include <functional>
#include <span>

namespace regml {
struct GmrtExecutionContext {
    Value self = std::monostate{};
    Value other = std::monostate{};
    Value scope = std::monostate{};
    StructPtr globals;
    std::vector<Value> arguments;
    std::function<Value(
        std::string_view, std::uint16_t, const Value &, const Value &, const Value &, std::span<const Value>)>
        call;
    std::function<Value(const Value &, std::string_view)> get_property;
    std::function<void(const Value &, std::string_view, const Value &)> set_property;
    std::function<Value(std::string_view)> resolve_global;
};

struct GmrtVmOptions {
    std::size_t instruction_budget = 1'000'000;
    std::size_t max_call_depth = 128;
    std::function<void(
        const GmrtFunction &, const GmrtBlock &, const GmrtInstruction &, std::span<const Value>)>
        trace;
};

struct GmrtVmResult {
    Value returned = std::monostate{};
    std::size_t instructions_executed = 0;
};

GmrtVmResult execute_gmrt(const GmrtModule &module,
                          std::size_t function_index,
                          GmrtExecutionContext context = {},
                          GmrtVmOptions options = {});
GmrtVmResult execute_gmrt(std::span<const GmrtModule> modules,
                          std::size_t module_index,
                          std::size_t function_index,
                          GmrtExecutionContext context = {},
                          GmrtVmOptions options = {});
} // namespace regml
