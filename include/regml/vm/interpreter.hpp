#pragma once
#include "regml/bytecode/decoder.hpp"
#include <functional>
#include <string>
#include <memory>
namespace regml {
// Scalar values; arrays, structs, methods and the complete coercion model remain pending.
struct ArrayValue;
using Value = std::variant<std::int32_t, double, bool, std::string, std::monostate,std::shared_ptr<ArrayValue>>;
struct ArrayValue {std::unordered_map<std::int32_t,Value> elements;std::unordered_map<std::int32_t,std::int32_t> row_lengths;};
using VariableStore = std::unordered_map<std::string, Value>;
enum class VariableScope { local, self, global };
struct VariableBinding { VariableScope scope; std::string name; };
// Bindings address instruction offsets, not raw occurrence-chain payloads.
// Package VARI/FUNC linking is separate; original fixtures supply their own bindings.
struct VmBindings {
    std::unordered_map<std::size_t, VariableBinding> variables;
    std::unordered_map<std::size_t, std::string> functions;
    std::unordered_map<std::uint32_t, std::string> strings;
};
struct ExecutionContext {
    const VmBindings* bindings = nullptr;
    VariableStore locals;
    VariableStore* self = nullptr;
    VariableStore* globals = nullptr;
    std::function<Value(std::string_view, std::span<const Value>)> call_builtin;
};
struct VmOptions {
    std::size_t instruction_budget = 100'000;
    std::size_t max_stack_values = 4096;
    std::function<void(const Instruction&, std::span<const Value>)> trace;
};
struct VmResult {
    std::optional<Value> returned;
    std::size_t instructions_executed = 0;
    std::size_t peak_stack_values = 0;
};
VmResult execute_numeric(const Program& program, VmOptions options = {});
VmResult execute(const Program& program, ExecutionContext& context, VmOptions options = {});
double value_number(const Value& value);
std::string value_text(const Value& value);
std::vector<std::uint8_t> demo_bytecode();
} // namespace regml
