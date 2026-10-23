#include "gmrt_vm.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

namespace regml {
namespace {
class GmrtInterpreter {
  public:
    GmrtInterpreter(std::span<const GmrtModule> modules, GmrtVmOptions options)
        : modules_(modules), options_(std::move(options)) {}

    GmrtVmResult run(std::size_t module_index, std::size_t function_index, GmrtExecutionContext context) {
        if (module_index >= modules_.size() || function_index >= modules_[module_index].functions.size())
            throw Error(ErrorKind::usage, "GMIR function index is out of range");
        if (!context.globals)
            context.globals = std::make_shared<StructValue>();
        if (std::holds_alternative<std::monostate>(context.scope))
            context.scope = context.self;
        GmrtVmResult result;
        result.returned = run_function(module_index, function_index, context.arguments, context.self,
                                       context.other, context.scope, context, 0);
        result.instructions_executed = instructions_executed_;
        return result;
    }

  private:
    [[noreturn]] static void unsupported(const GmrtInstruction &instruction, std::string message) {
        throw Error(ErrorKind::unsupported, std::move(message), instruction.binary_offset);
    }

    static double number(const Value &value, const GmrtInstruction &instruction) {
        if (const auto *integer = std::get_if<std::int32_t>(&value))
            return static_cast<double>(*integer);
        if (const auto *real = std::get_if<double>(&value))
            return *real;
        if (const auto *boolean = std::get_if<bool>(&value))
            return *boolean ? 1.0 : 0.0;
        unsupported(instruction, "GMIR numeric operation received a non-numeric value");
    }

    static std::int32_t integer(const Value &value, const GmrtInstruction &instruction) {
        const auto value_number = number(value, instruction);
        if (!std::isfinite(value_number) || value_number < std::numeric_limits<std::int32_t>::min() ||
            value_number > std::numeric_limits<std::int32_t>::max())
            unsupported(instruction, "GMIR integer operation is outside the int32 range");
        return static_cast<std::int32_t>(value_number);
    }

    static bool truthy(const Value &value) {
        if (const auto *boolean = std::get_if<bool>(&value))
            return *boolean;
        if (const auto *integer = std::get_if<std::int32_t>(&value))
            return *integer != 0;
        if (const auto *real = std::get_if<double>(&value))
            return *real != 0.0;
        if (value_is_string(value))
            return !value_string_view(value).empty();
        if (std::holds_alternative<std::monostate>(value))
            return false;
        return true;
    }

    static Value numeric_result(double value, bool preserve_integer) {
        if (preserve_integer && std::isfinite(value) && value >= std::numeric_limits<std::int32_t>::min() &&
            value <= std::numeric_limits<std::int32_t>::max() && std::trunc(value) == value)
            return static_cast<std::int32_t>(value);
        return value;
    }

    static bool equal(const Value &left, const Value &right) {
        if ((std::holds_alternative<std::int32_t>(left) || std::holds_alternative<double>(left) ||
             std::holds_alternative<bool>(left)) &&
            (std::holds_alternative<std::int32_t>(right) || std::holds_alternative<double>(right) ||
             std::holds_alternative<bool>(right)))
            return std::get<double>(numeric_result(number_for_compare(left), false)) ==
                   std::get<double>(numeric_result(number_for_compare(right), false));
        if (value_is_string(left) && value_is_string(right))
            return value_string_view(left) == value_string_view(right);
        if (left.index() != right.index())
            return false;
        if (std::holds_alternative<std::monostate>(left))
            return true;
        return false;
    }

    static double number_for_compare(const Value &value) {
        if (const auto *integer = std::get_if<std::int32_t>(&value))
            return static_cast<double>(*integer);
        if (const auto *real = std::get_if<double>(&value))
            return *real;
        return std::get<bool>(value) ? 1.0 : 0.0;
    }

    static bool less(const Value &left, const Value &right, const GmrtInstruction &instruction) {
        if (value_is_string(left) && value_is_string(right))
            return value_string_view(left) < value_string_view(right);
        return number(left, instruction) < number(right, instruction);
    }

    struct FunctionTarget {
        std::size_t module_index;
        std::size_t function_index;
    };

    [[nodiscard]] std::optional<FunctionTarget> find_function(std::string_view name,
                                                              std::size_t preferred_module) const {
        for (std::size_t module_pass = 0; module_pass < modules_.size(); ++module_pass) {
            const auto module_index = module_pass == 0
                                          ? preferred_module
                                          : (module_pass <= preferred_module ? module_pass - 1 : module_pass);
            if (module_index >= modules_.size())
                continue;
            const auto &functions = modules_[module_index].functions;
            for (std::size_t function_index = 0; function_index < functions.size(); ++function_index) {
                const auto &function = functions[function_index];
                if (function.symbol == name || function.global_name == name || function.alias == name)
                    return FunctionTarget{module_index, function_index};
            }
        }
        return std::nullopt;
    }

    Value call(const GmrtInstruction &instruction,
               std::string_view name,
               std::uint16_t flags,
               std::span<const Value> arguments,
               const Value &self,
               const Value &other,
               const Value &scope,
               GmrtExecutionContext &context,
               std::size_t current_module,
               std::size_t depth) {
        if (name == "@@GML_GetUserGlobal@@")
            return context.globals;
        if (name == "@@GML_NewObject@@")
            return std::make_shared<StructValue>();
        if (name == "@@GML_NewArray@@") {
            ArrayPtr array(new ArrayValue());
            for (std::size_t i = 0; i < arguments.size(); ++i)
                array->elements.emplace(static_cast<std::int32_t>(i), arguments[i]);
            return array;
        }
        if (name == "variable_global_exists") {
            if (arguments.size() != 1 || !value_is_string(arguments.front()))
                unsupported(instruction, "variable_global_exists expects one string argument");
            return context.globals &&
                   context.globals->variables.contains(value_string_view(arguments.front()));
        }
        if (name == "array_create" || name == "array_create_ext") {
            if (arguments.empty() || arguments.size() > 2)
                unsupported(instruction, "array_create expects a length and optional initial value");
            const auto length = number(arguments.front(), instruction);
            if (!std::isfinite(length) || length < 0 || length > 1'000'000)
                unsupported(instruction, "array_create length is outside the supported range");
            const auto count = static_cast<std::size_t>(length);
            const Value initial = arguments.size() == 2 ? arguments[1] : Value(std::monostate{});
            ArrayPtr array(new ArrayValue());
            for (std::size_t i = 0; i < count; ++i)
                array->elements.emplace(static_cast<std::int32_t>(i), initial);
            return array;
        }
        if (name == "array_length" || name == "array_length_1d") {
            if (arguments.size() != 1)
                unsupported(instruction, "array_length expects one array argument");
            const auto *array = std::get_if<ArrayPtr>(&arguments.front());
            if (!array || !*array)
                return 0;
            std::int32_t length = 0;
            for (const auto &[index, value] : (*array)->elements) {
                (void)value;
                if (index >= 0 && index < std::numeric_limits<std::int32_t>::max())
                    length = std::max(length, index + 1);
            }
            return length;
        }
        if (const auto function = find_function(name, current_module)) {
            return run_function(function->module_index, function->function_index, arguments, self, other,
                                scope, context, depth + 1);
        }
        if (context.call)
            return context.call(name, flags, self, other, scope, arguments);
        unsupported(instruction, "GMIR call target is not implemented: " + std::string(name));
    }

    Value run_function(std::size_t module_index,
                       std::size_t function_index,
                       std::span<const Value> arguments,
                       const Value &self,
                       const Value &other,
                       const Value &scope,
                       GmrtExecutionContext &context,
                       std::size_t depth) {
        if (depth >= options_.max_call_depth)
            throw Error(ErrorKind::vm, "GMIR call depth exceeded");
        const auto &function = modules_[module_index].functions[function_index];
        std::vector<Value> registers(function.registers.size());
        if (!registers.empty())
            registers[0] = self;
        if (registers.size() > 1)
            registers[1] = other;
        if (registers.size() > 2)
            registers[2] = scope;
        bool condition = false;
        std::size_t block_index = 0;
        while (block_index < function.blocks.size()) {
            const auto &block = function.blocks[block_index];
            bool transferred = false;
            for (const auto &instruction : block.instructions) {
                if (instructions_executed_ >= options_.instruction_budget)
                    throw Error(ErrorKind::vm, "GMIR instruction budget exhausted",
                                instruction.binary_offset);
                ++instructions_executed_;
                if (options_.trace)
                    options_.trace(function, block, instruction, registers);

                const auto &a = instruction.arguments;
                const auto op = gmrt_opcode_name(instruction.opcode);
                auto require_args = [&](std::size_t count) {
                    if (a.size() < count)
                        throw Error(ErrorKind::malformed,
                                    "GMIR " + std::string(op) + " instruction has too few operands",
                                    instruction.binary_offset);
                };
                auto reg = [&](std::size_t operand) -> Value & {
                    if (operand >= a.size() || a[operand] >= registers.size())
                        throw Error(ErrorKind::malformed,
                                    "GMIR " + std::string(op) + " register index is out of range",
                                    instruction.binary_offset);
                    return registers[a[operand]];
                };
                auto constant = [&](std::size_t operand) -> const GmrtConstant & {
                    if (operand >= a.size() || a[operand] >= function.constants.size())
                        throw Error(ErrorKind::malformed,
                                    "GMIR " + std::string(op) + " constant index is out of range",
                                    instruction.binary_offset);
                    return function.constants[a[operand]];
                };
                auto property_name = [&](std::size_t operand) -> std::string_view {
                    const auto &entry = constant(operand);
                    if (entry.kind != GmrtConstantKind::symbol && entry.kind != GmrtConstantKind::string)
                        throw Error(ErrorKind::malformed, "GMIR property operand does not refer to a symbol",
                                    instruction.binary_offset);
                    return std::get<std::string>(entry.value);
                };
                auto jump = [&](std::size_t target) {
                    if (target >= function.blocks.size())
                        throw Error(ErrorKind::malformed, "GMIR branch target is out of range",
                                    instruction.binary_offset);
                    block_index = target;
                    transferred = true;
                };
                auto get_property = [&](const Value &object, std::string_view name) -> Value {
                    if (context.get_property)
                        return context.get_property(object, name);
                    if (const auto *structure = std::get_if<StructPtr>(&object); structure && *structure) {
                        const auto found = (*structure)->variables.find(name);
                        return found == (*structure)->variables.end() ? Value(std::monostate{})
                                                                      : found->second;
                    }
                    if (const auto *callable = std::get_if<CallablePtr>(&object);
                        callable && *callable && (*callable)->properties) {
                        const auto found = (*callable)->properties->variables.find(name);
                        return found == (*callable)->properties->variables.end() ? Value(std::monostate{})
                                                                                 : found->second;
                    }
                    unsupported(instruction, "GMIR property read needs a host object adapter (value type " +
                                                 std::to_string(object.index()) + ")");
                };
                auto set_property = [&](const Value &object, std::string_view name, const Value &value) {
                    if (context.set_property) {
                        context.set_property(object, name, value);
                        return;
                    }
                    if (const auto *structure = std::get_if<StructPtr>(&object); structure && *structure) {
                        (*structure)->variables[name] = value;
                        return;
                    }
                    if (const auto *callable = std::get_if<CallablePtr>(&object); callable && *callable) {
                        if (!(*callable)->properties)
                            (*callable)->properties = std::make_shared<StructValue>();
                        (*callable)->properties->variables[name] = value;
                        return;
                    }
                    unsupported(instruction, "GMIR property write needs a host object adapter");
                };

                switch (instruction.opcode) {
                case 0:    // ADD
                case 1:    // SUB
                case 2:    // MUL
                case 3:    // DIV
                case 4:    // MOD
                case 5:    // AND
                case 6:    // OR
                case 7:    // XOR
                case 12:   // SHL
                case 13:   // SHR
                case 14:   // SAR
                case 36: { // IDIV
                    require_args(3);
                    const auto left = reg(1);
                    const auto right = reg(2);
                    if (instruction.opcode == 0 && (value_is_string(left) || value_is_string(right))) {
                        reg(0) = value_text(left) + value_text(right);
                        break;
                    }
                    const auto lhs = number(left, instruction);
                    const auto rhs = number(right, instruction);
                    const bool preserve_integer = std::holds_alternative<std::int32_t>(left) &&
                                                  std::holds_alternative<std::int32_t>(right);
                    Value value;
                    switch (instruction.opcode) {
                    case 0:
                        if (value_is_string(left) || value_is_string(right))
                            value = value_text(left) + value_text(right);
                        else
                            value = numeric_result(lhs + rhs, preserve_integer);
                        break;
                    case 1:
                        value = numeric_result(lhs - rhs, preserve_integer);
                        break;
                    case 2:
                        value = numeric_result(lhs * rhs, preserve_integer);
                        break;
                    case 3:
                        value = lhs / rhs;
                        break;
                    case 4:
                        value = std::fmod(lhs, rhs);
                        break;
                    case 5:
                        value = static_cast<std::int32_t>(integer(left, instruction) &
                                                          integer(right, instruction));
                        break;
                    case 6:
                        value = static_cast<std::int32_t>(integer(left, instruction) |
                                                          integer(right, instruction));
                        break;
                    case 7:
                        value = static_cast<std::int32_t>(integer(left, instruction) ^
                                                          integer(right, instruction));
                        break;
                    case 12:
                        value =
                            static_cast<std::int32_t>(static_cast<std::uint32_t>(integer(left, instruction))
                                                      << (integer(right, instruction) & 31));
                        break;
                    case 13:
                        value = static_cast<std::int32_t>(
                            static_cast<std::uint32_t>(integer(left, instruction)) >>
                            (integer(right, instruction) & 31));
                        break;
                    case 14:
                        value = static_cast<std::int32_t>(integer(left, instruction) >>
                                                          (integer(right, instruction) & 31));
                        break;
                    default:
                        value = numeric_result(std::trunc(lhs / rhs), preserve_integer);
                        break;
                    }
                    reg(0) = std::move(value);
                    break;
                }
                case 8:    // NEG
                case 9:    // PLUS
                case 10: { // LNOT
                    require_args(2);
                    const auto source = reg(1);
                    if (instruction.opcode == 8)
                        reg(0) = numeric_result(-number(source, instruction),
                                                std::holds_alternative<std::int32_t>(source));
                    else if (instruction.opcode == 9)
                        reg(0) = number(source, instruction);
                    else
                        reg(0) = !truthy(source);
                    break;
                }
                case 11: { // BNOT
                    require_args(2);
                    reg(0) = static_cast<std::int32_t>(~integer(reg(1), instruction));
                    break;
                }
                case 15: // B
                    require_args(1);
                    jump(a[0]);
                    break;
                case 16: // BT
                case 17: // BF
                    require_args(1);
                    if ((instruction.opcode == 16 && condition) || (instruction.opcode == 17 && !condition))
                        jump(a[0]);
                    else
                        ++block_index;
                    transferred = true;
                    break;
                case 18: { // BJ: dense switch, last target is the default
                    require_args(2);
                    const auto selector = integer(reg(0), instruction);
                    const auto case_count = a.size() - 2;
                    const auto target_operand =
                        selector >= 0 && static_cast<std::size_t>(selector) < case_count
                            ? static_cast<std::size_t>(selector) + 1
                            : a.size() - 1;
                    jump(a[target_operand]);
                    break;
                }
                case 19: { // LDC
                    require_args(2);
                    const auto &entry = constant(1);
                    if (entry.kind == GmrtConstantKind::immutable_global && context.resolve_global)
                        reg(0) = context.resolve_global(std::get<std::string>(entry.value));
                    else
                        reg(0) = gmrt_constant_value(entry);
                    break;
                }
                case 20: // RET
                    return std::monostate{};
                case 21: // RETV
                    return a.empty() ? Value(std::monostate{}) : reg(0);
                case 22:   // CALL
                case 23: { // STATIC_CALL
                    require_args(2);
                    const auto destination = a[0];
                    if (destination >= registers.size())
                        throw Error(ErrorKind::malformed, "GMIR call destination is out of range",
                                    instruction.binary_offset);
                    std::string_view name;
                    Value dynamic_callee;
                    if (instruction.opcode == 23) {
                        const auto &target = constant(1);
                        if (target.kind != GmrtConstantKind::function &&
                            target.kind != GmrtConstantKind::immutable_global &&
                            target.kind != GmrtConstantKind::symbol)
                            throw Error(ErrorKind::malformed, "GMIR static call target is not a function",
                                        instruction.binary_offset);
                        name = std::get<std::string>(target.value);
                    } else {
                        dynamic_callee = reg(1);
                        if (const auto *text = std::get_if<std::string>(&dynamic_callee))
                            name = *text;
                    }
                    const auto flags = a.size() > 2 ? a[2] : 0;
                    const auto first_argument = a.size() > 2 ? 3U : 2U;
                    std::vector<Value> call_arguments;
                    call_arguments.reserve(a.size() - first_argument);
                    for (std::size_t i = first_argument; i < a.size(); ++i) {
                        if (a[i] >= registers.size())
                            throw Error(ErrorKind::malformed, "GMIR call argument register is out of range",
                                        instruction.binary_offset);
                        call_arguments.push_back(registers[a[i]]);
                    }
                    if (instruction.opcode == 22) {
                        if (const auto *callable = std::get_if<CallablePtr>(&dynamic_callee);
                            callable && *callable && (*callable)->invoke)
                            registers[destination] = (*callable)->invoke(call_arguments);
                        else if (!name.empty())
                            registers[destination] = call(instruction, name, flags, call_arguments, self,
                                                          other, scope, context, module_index, depth);
                        else
                            unsupported(instruction, "GMIR dynamic call target is not callable");
                    } else
                        registers[destination] = call(instruction, name, flags, call_arguments, self, other,
                                                      scope, context, module_index, depth);
                    break;
                }
                case 24:   // LT
                case 25:   // LE
                case 26:   // GT
                case 27:   // GE
                case 28:   // EQ
                case 29:   // NE
                case 30:   // IN
                case 31:   // TYPE_EQ
                case 32: { // TYPE_NE
                    require_args(2);
                    const auto &left = reg(0);
                    const auto &right = reg(1);
                    if (instruction.opcode == 24)
                        condition = less(left, right, instruction);
                    else if (instruction.opcode == 25)
                        condition = !less(right, left, instruction);
                    else if (instruction.opcode == 26)
                        condition = less(right, left, instruction);
                    else if (instruction.opcode == 27)
                        condition = !less(left, right, instruction);
                    else if (instruction.opcode == 28)
                        condition = equal(left, right);
                    else if (instruction.opcode == 29)
                        condition = !equal(left, right);
                    else if (instruction.opcode == 31)
                        condition = left.index() == right.index();
                    else if (instruction.opcode == 32)
                        condition = left.index() != right.index();
                    else if (value_is_string(left) && value_is_string(right))
                        condition =
                            value_string_view(left).find(value_string_view(right)) != std::string_view::npos;
                    else
                        unsupported(instruction, "GMIR IN currently supports string membership only");
                    break;
                }
                case 33: // MOV
                    require_args(2);
                    reg(0) = reg(1);
                    break;
                case 34: // MOVCC
                    require_args(1);
                    reg(0) = condition;
                    break;
                case 35: // TOCC
                    require_args(1);
                    condition = truthy(reg(0));
                    break;
                case 37:   // PGET_A
                case 38: { // PGET
                    require_args(3);
                    reg(0) = get_property(reg(1), property_name(2));
                    break;
                }
                case 39: // PSET_A
                case 40: // PSET
                    require_args(3);
                    set_property(reg(1), property_name(2), reg(0));
                    break;
                case 41: { // IGETG
                    require_args(4);
                    const auto *array = std::get_if<ArrayPtr>(&reg(2));
                    if (!array || !*array)
                        unsupported(instruction, "GMIR indexed read needs an array value");
                    const auto index = integer(reg(3), instruction);
                    const auto found = (*array)->elements.find(index);
                    reg(0) = found == (*array)->elements.end() ? Value(std::monostate{}) : found->second;
                    break;
                }
                case 42: { // ISETG
                    require_args(4);
                    auto *array = std::get_if<ArrayPtr>(&reg(2));
                    if (!array || !*array)
                        unsupported(instruction, "GMIR indexed write needs an array value");
                    (*array)->elements[integer(reg(3), instruction)] = reg(0);
                    break;
                }
                case 43: // IGETJ
                case 44: // ISETJ
                    unsupported(instruction, "GMIR indexed property operation is not implemented");
                case 45: { // PARRAY
                    require_args(3);
                    auto array = get_property(reg(1), property_name(2));
                    if (!std::holds_alternative<ArrayPtr>(array) || !std::get<ArrayPtr>(array)) {
                        array = ArrayPtr(new ArrayValue());
                        set_property(reg(1), property_name(2), array);
                    }
                    reg(0) = std::move(array);
                    break;
                }
                case 46: { // IARRAY
                    require_args(4);
                    auto *array = std::get_if<ArrayPtr>(&reg(2));
                    if (!array || !*array)
                        unsupported(instruction, "GMIR nested array access needs an array value");
                    const auto index = integer(reg(3), instruction);
                    auto &element = (*array)->elements[index];
                    if (!std::holds_alternative<ArrayPtr>(element) || !std::get<ArrayPtr>(element))
                        element = ArrayPtr(new ArrayValue());
                    reg(0) = element;
                    break;
                }
                case 47: // RARRAY
                    require_args(2);
                    reg(0) = reg(1);
                    break;
                case 49: // OTHER
                    require_args(1);
                    reg(0) = other;
                    break;
                case 52: // ARGINIT
                    require_args(2);
                    reg(0) = a[1] < arguments.size() ? arguments[a[1]] : Value(std::monostate{});
                    break;
                case 53: // ARGC
                    require_args(1);
                    reg(0) = static_cast<std::int32_t>(arguments.size());
                    break;
                case 61: // UNREACHABLE
                    unsupported(instruction, "GMIR reached an unreachable instruction");
                default:
                    unsupported(instruction, "GMIR opcode is not implemented: " + std::string(op));
                }

                if (transferred)
                    break;
            }
            if (!transferred)
                ++block_index;
        }
        return std::monostate{};
    }

    std::span<const GmrtModule> modules_;
    GmrtVmOptions options_;
    std::size_t instructions_executed_ = 0;
};
} // namespace

GmrtVmResult execute_gmrt(const GmrtModule &module,
                          std::size_t function_index,
                          GmrtExecutionContext context,
                          GmrtVmOptions options) {
    return execute_gmrt(std::span<const GmrtModule>(&module, 1), 0, function_index, std::move(context),
                        std::move(options));
}

GmrtVmResult execute_gmrt(std::span<const GmrtModule> modules,
                          std::size_t module_index,
                          std::size_t function_index,
                          GmrtExecutionContext context,
                          GmrtVmOptions options) {
    return GmrtInterpreter(modules, std::move(options)).run(module_index, function_index, std::move(context));
}
} // namespace regml
