#include "regml/vm/interpreter.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
namespace regml {
namespace {
DataType runtime_type(const Value &value) {
    if (std::holds_alternative<std::int32_t>(value))
        return DataType::int32;
    if (std::holds_alternative<double>(value))
        return DataType::real;
    if (std::holds_alternative<bool>(value))
        return DataType::boolean;
    if (std::holds_alternative<std::string>(value))
        return DataType::string;
    return DataType::variable;
}
bool compare(double left, double right, std::uint8_t kind, std::size_t offset) {
    switch (kind) {
    case 1:
        return left < right;
    case 2:
        return left <= right;
    case 3:
        return left == right;
    case 4:
        return left != right;
    case 5:
        return left >= right;
    case 6:
        return left > right;
    default:
        throw Error(ErrorKind::malformed, "unknown comparison kind", offset);
    }
}
} // namespace
VmResult execute_numeric(const Program &program, VmOptions options) {
    ExecutionContext context;
    return execute(program, context, std::move(options));
}
VmResult execute(const Program &program, ExecutionContext &context, VmOptions options) {
    if (program.bytecode_version != 15 && program.bytecode_version != 16)
        throw Error(ErrorKind::unsupported, "numeric VM requires bytecode 15 or 16", program.origin);
    VmResult result;
    std::vector<Value> stack;
    std::size_t pc = 0;
    while (pc != program.byte_length) {
        const auto found = program.index_by_offset.find(pc);
        if (found == program.index_by_offset.end() || found->second >= program.instructions.size())
            throw Error(ErrorKind::vm, "program counter is not an instruction boundary", program.origin + pc);
        const auto &ins = program.instructions[found->second];
        const auto absolute = program.origin + pc;
        if (result.instructions_executed >= options.instruction_budget)
            throw Error(ErrorKind::vm, "instruction budget exhausted", absolute);
        ++result.instructions_executed;
        if (options.trace)
            options.trace(ins, stack);
        auto push = [&](Value value) {
            if (stack.size() >= options.max_stack_values)
                throw Error(ErrorKind::vm, "operand stack limit exceeded", absolute);
            stack.push_back(value);
            result.peak_stack_values = std::max(result.peak_stack_values, stack.size());
        };
        auto pop = [&]() -> Value {
            if (stack.empty())
                throw Error(ErrorKind::vm, "operand stack underflow", absolute);
            auto value = stack.back();
            stack.pop_back();
            return value;
        };
        auto check_type = [&](const Value &value, DataType type) {
            if (type == DataType::variable)
                return;
            if (runtime_type(value) != type)
                throw Error(ErrorKind::unsupported, "numeric VM requires explicit matching scalar types", absolute);
        };
        auto narrow = [&](std::int64_t value) -> std::int32_t {
            if (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max())
                throw Error(ErrorKind::unsupported, "integer overflow semantics are not implemented", absolute);
            return static_cast<std::int32_t>(value);
        };
        auto truth = [&](const Value &value) -> bool {
            if (const auto *integer = std::get_if<std::int32_t>(&value))
                return *integer != 0;
            if (const auto *boolean = std::get_if<bool>(&value))
                return *boolean;
            throw Error(ErrorKind::unsupported, "branch condition requires bool/int32 in the numeric VM", absolute);
        };
        auto variable_store = [&](const VariableBinding &binding) -> VariableStore & {
            switch (binding.scope) {
            case VariableScope::local:
                return context.locals;
            case VariableScope::self:
                if (context.self)
                    return *context.self;
                break;
            case VariableScope::global:
                if (context.globals)
                    return *context.globals;
                break;
            }
            throw Error(ErrorKind::vm, "variable scope is unavailable", absolute);
        };
        auto variable_binding = [&]() -> const VariableBinding & {
            if (context.bindings) {
                const auto it = context.bindings->variables.find(ins.offset);
                if (it != context.bindings->variables.end())
                    return it->second;
            }
            throw Error(ErrorKind::unsupported, "variable instruction has no resolved binding", absolute);
        };
        std::size_t next = pc + ins.size;
        switch (ins.opcode) {
        case Opcode::push_i:
        case Opcode::push:
        case Opcode::push_local:
        case Opcode::push_global:
        case Opcode::push_builtin:
            if (ins.type1 == DataType::variable) {
                const auto &binding = variable_binding();
                auto &store = variable_store(binding);
                const auto found_value = store.find(binding.name);
                if (found_value == store.end())
                    throw Error(ErrorKind::vm, "variable is not initialized: " + binding.name, absolute);
                push(found_value->second);
            } else if (ins.type1 == DataType::string && ins.reference && context.bindings) {
                const auto string = context.bindings->strings.find(*ins.reference);
                if (string == context.bindings->strings.end())
                    throw Error(ErrorKind::vm, "string constant is not resolved", absolute);
                push(string->second);
            } else if (const auto *value = std::get_if<std::int32_t>(&ins.immediate))
                push(*value);
            else if (const auto *value = std::get_if<double>(&ins.immediate))
                push(*value);
            else
                throw Error(ErrorKind::unsupported, "only int32/double constants are executable", absolute);
            break;
        case Opcode::pop: {
            if (ins.type1 == DataType::int16)
                throw Error(ErrorKind::unsupported, "stack swap pop variant is not implemented", absolute);
            const auto &binding = variable_binding();
            const auto value = pop();
            check_type(value, ins.type1);
            variable_store(binding).insert_or_assign(binding.name, value);
            break;
        }
        case Opcode::call: {
            if (!context.bindings || !context.call_builtin)
                throw Error(ErrorKind::unsupported, "call has no built-in host", absolute);
            const auto function = context.bindings->functions.find(ins.offset);
            if (function == context.bindings->functions.end())
                throw Error(ErrorKind::unsupported, "call has no resolved function binding", absolute);
            const auto count = static_cast<std::size_t>(ins.word & 0xffff);
            if (count > stack.size())
                throw Error(ErrorKind::vm, "not enough call arguments on the stack", absolute);
            std::vector<Value> arguments;
            arguments.reserve(count);
            // Argument zero is at the top of the stack.
            for (std::size_t i = 0; i < count; ++i)
                arguments.push_back(pop());
            try {
                push(context.call_builtin(function->second, arguments));
            } catch (const Error &error) {
                if (error.offset())
                    throw;
                throw Error(error.kind(), std::string(error.what()) + " in " + function->second, absolute);
            }
            break;
        }
        case Opcode::conv: {
            const auto value = pop();
            check_type(value, ins.type1);
            if (ins.type1 == ins.type2)
                push(value);
            else if (ins.type2 == DataType::variable)
                push(value);
            else if (ins.type1 == DataType::variable && ins.type2 == DataType::real)
                push(value_number(value));
            else if (ins.type2 == DataType::real && std::holds_alternative<std::int32_t>(value))
                push(static_cast<double>(std::get<std::int32_t>(value)));
            else if (ins.type2 == DataType::boolean && std::holds_alternative<std::int32_t>(value))
                push(std::get<std::int32_t>(value) != 0);
            else
                throw Error(ErrorKind::unsupported, "conversion is not implemented by the numeric VM", absolute);
            break;
        }
        case Opcode::add:
        case Opcode::sub:
        case Opcode::mul:
        case Opcode::div:
        case Opcode::cmp: {
            const auto right = pop();
            const auto left = pop();
            if (ins.opcode == Opcode::add && std::holds_alternative<std::string>(left) &&
                std::holds_alternative<std::string>(right)) {
                check_type(right, ins.type1);
                check_type(left, ins.type2);
                const auto &a = std::get<std::string>(left);
                const auto &b = std::get<std::string>(right);
                if (a.size() > 1'048'576 || b.size() > 1'048'576 - a.size())
                    throw Error(ErrorKind::vm, "string result exceeds scalar VM limit", absolute);
                push(a + b);
                break;
            }
            if (ins.type1 == DataType::variable || ins.type2 == DataType::variable) {
                check_type(right, ins.type1);
                check_type(left, ins.type2);
                const auto a = value_number(left);
                const auto b = value_number(right);
                if (ins.opcode == Opcode::cmp)
                    push(compare(a, b, ins.comparison, absolute));
                else if (ins.opcode == Opcode::add)
                    push(a + b);
                else if (ins.opcode == Opcode::sub)
                    push(a - b);
                else if (ins.opcode == Opcode::mul)
                    push(a * b);
                else {
                    if (b == 0)
                        throw Error(ErrorKind::unsupported, "division-by-zero compatibility is not implemented",
                                    absolute);
                    push(a / b);
                }
                break;
            }
            if (ins.type1 != ins.type2)
                throw Error(ErrorKind::unsupported, "mixed arithmetic operand types are not implemented", absolute);
            check_type(right, ins.type1);
            check_type(left, ins.type2);
            if (ins.type1 == DataType::int32) {
                const auto a = static_cast<std::int64_t>(std::get<std::int32_t>(left));
                const auto b = static_cast<std::int64_t>(std::get<std::int32_t>(right));
                if (ins.opcode == Opcode::cmp)
                    push(compare(static_cast<double>(a), static_cast<double>(b), ins.comparison, absolute));
                else if (ins.opcode == Opcode::add)
                    push(narrow(a + b));
                else if (ins.opcode == Opcode::sub)
                    push(narrow(a - b));
                else if (ins.opcode == Opcode::mul)
                    push(narrow(a * b));
                else
                    throw Error(ErrorKind::unsupported, "integer-typed div result semantics are not implemented",
                                absolute);
            } else if (ins.type1 == DataType::real) {
                const auto a = std::get<double>(left);
                const auto b = std::get<double>(right);
                if (ins.opcode == Opcode::cmp)
                    push(compare(a, b, ins.comparison, absolute));
                else if (ins.opcode == Opcode::add)
                    push(a + b);
                else if (ins.opcode == Opcode::sub)
                    push(a - b);
                else if (ins.opcode == Opcode::mul)
                    push(a * b);
                else {
                    if (b == 0)
                        throw Error(ErrorKind::unsupported, "division-by-zero compatibility is not implemented",
                                    absolute);
                    push(a / b);
                }
            } else
                throw Error(ErrorKind::unsupported, "arithmetic value type is not implemented", absolute);
            break;
        }
        case Opcode::bit_or:
        case Opcode::bit_and:
        case Opcode::bit_xor: {
            const auto right = pop();
            const auto left = pop();
            if (ins.type1 != DataType::boolean || ins.type2 != DataType::boolean)
                throw Error(ErrorKind::unsupported, "integer bit operations are not implemented", absolute);
            check_type(right, ins.type1);
            check_type(left, ins.type2);
            const bool a = std::get<bool>(left), b = std::get<bool>(right);
            if (ins.opcode == Opcode::bit_or)
                push(a || b);
            else if (ins.opcode == Opcode::bit_and)
                push(a && b);
            else
                push(a != b);
            break;
        }
        case Opcode::neg: {
            const auto value = pop();
            check_type(value, ins.type1);
            if (const auto *integer = std::get_if<std::int32_t>(&value))
                push(narrow(-static_cast<std::int64_t>(*integer)));
            else if (const auto *real = std::get_if<double>(&value))
                push(-*real);
            else
                throw Error(ErrorKind::unsupported, "negation requires a number", absolute);
            break;
        }
        case Opcode::dup:
            if (ins.extra != 0 || ((ins.word >> 8) & 0xff) != 0)
                throw Error(ErrorKind::unsupported, "only the basic dup variant is implemented", absolute);
            if (stack.empty())
                throw Error(ErrorKind::vm, "operand stack underflow", absolute);
            check_type(stack.back(), ins.type1);
            push(stack.back());
            break;
        case Opcode::popz: {
            const auto value = pop();
            check_type(value, ins.type1);
            break;
        }
        case Opcode::branch:
        case Opcode::branch_true:
        case Opcode::branch_false: {
            bool take = true;
            if (ins.opcode != Opcode::branch) {
                take = truth(pop());
                if (ins.opcode == Opcode::branch_false)
                    take = !take;
            }
            if (take) {
                if (!ins.branch_target)
                    throw Error(ErrorKind::vm, "branch has no decoded target", absolute);
                next = *ins.branch_target;
            }
            break;
        }
        case Opcode::ret: {
            const auto value = pop();
            check_type(value, ins.type1);
            result.returned = value;
            return result;
        }
        case Opcode::exit:
            return result;
        default:
            throw Error(ErrorKind::unsupported,
                        std::string(opcode_name(ins.opcode)) + " is decoded but not executable by the numeric VM",
                        absolute);
        }
        pc = next;
    }
    return result;
}
std::string value_text(const Value &value) {
    if (const auto *number = std::get_if<std::int32_t>(&value))
        return std::to_string(*number);
    if (const auto *boolean = std::get_if<bool>(&value))
        return *boolean ? "true" : "false";
    if (const auto *string = std::get_if<std::string>(&value))
        return *string;
    if (std::holds_alternative<std::monostate>(value))
        return "undefined";
    if (std::holds_alternative<ArrayPtr>(value))
        return "[array]";
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10) << std::get<double>(value);
    return out.str();
}
double value_number(const Value &value) {
    if (const auto *integer = std::get_if<std::int32_t>(&value))
        return *integer;
    if (const auto *real = std::get_if<double>(&value))
        return *real;
    if (const auto *boolean = std::get_if<bool>(&value))
        return *boolean ? 1 : 0;
    throw Error(ErrorKind::vm, "numeric argument requires a scalar number");
}
std::vector<std::uint8_t> demo_bytecode() {
    constexpr std::uint32_t words[] = {0x840f0007, 0x840f0005, 0x0d220000, 0x840f0015, 0x08220000, 0x9c020000};
    std::vector<std::uint8_t> bytes;
    for (auto word : words)
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<std::uint8_t>(word >> shift));
    return bytes;
}
} // namespace regml
