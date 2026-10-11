#pragma once
#include "regml/bytecode/decoder.hpp"
#include <functional>
#include <memory>
#include <string>
#include <utility>
namespace regml {
// Runtime scalars and legacy VM arrays.
struct ArrayValue;
// VM values stay on the simulation thread. Avoid platform atomic reference-count
// operations for arrays, including when unrelated audio worker threads exist.
class ArrayPtr {
  public:
    ArrayPtr() = default;
    explicit ArrayPtr(ArrayValue *value) noexcept;
    ArrayPtr(const ArrayPtr &other) noexcept;
    ArrayPtr(ArrayPtr &&other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    ~ArrayPtr();
    ArrayPtr &operator=(ArrayPtr other) noexcept {
        std::swap(value_, other.value_);
        return *this;
    }
    ArrayValue *get() const noexcept { return value_; }
    ArrayValue *operator->() const noexcept { return value_; }
    ArrayValue &operator*() const noexcept { return *value_; }
    explicit operator bool() const noexcept { return value_ != nullptr; }
    std::size_t use_count() const noexcept;

  private:
    ArrayValue *value_ = nullptr;
};
using Value = std::variant<std::int32_t, double, bool, std::string, std::monostate, ArrayPtr>;
struct ArrayValue {
    ArrayValue() = default;
    ArrayValue(const ArrayValue &other) : elements(other.elements), row_lengths(other.row_lengths) {}
    ArrayValue &operator=(const ArrayValue &other) {
        if (this != &other) {
            elements = other.elements;
            row_lengths = other.row_lengths;
        }
        return *this;
    }
    std::unordered_map<std::int32_t, Value> elements;
    std::unordered_map<std::int32_t, std::int32_t> row_lengths;

  private:
    friend class ArrayPtr;
    std::size_t references_ = 0;
};
inline ArrayPtr::ArrayPtr(ArrayValue *value) noexcept : value_(value) {
    if (value_)
        ++value_->references_;
}
inline ArrayPtr::ArrayPtr(const ArrayPtr &other) noexcept : ArrayPtr(other.value_) {}
inline ArrayPtr::~ArrayPtr() {
    if (value_ && --value_->references_ == 0)
        delete value_;
}
inline std::size_t ArrayPtr::use_count() const noexcept {
    return value_ ? value_->references_ : 0;
}
using VariableStore = std::unordered_map<std::string, Value>;
enum class VariableScope { local, self, global };
struct VariableBinding {
    VariableScope scope;
    std::string name;
};
// Bindings address instruction offsets, not raw occurrence-chain payloads.
// Package VARI/FUNC linking is separate; original fixtures supply their own bindings.
struct VmBindings {
    std::unordered_map<std::size_t, VariableBinding> variables;
    std::unordered_map<std::size_t, std::string> functions;
    std::unordered_map<std::uint32_t, std::string> strings;
};
struct ExecutionContext {
    const VmBindings *bindings = nullptr;
    VariableStore locals;
    VariableStore *self = nullptr;
    VariableStore *globals = nullptr;
    std::function<Value(std::string_view, std::span<const Value>)> call_builtin;
};
struct VmOptions {
    std::size_t instruction_budget = 100'000;
    std::size_t max_stack_values = 4096;
    std::function<void(const Instruction &, std::span<const Value>)> trace;
};
struct VmResult {
    std::optional<Value> returned;
    std::size_t instructions_executed = 0;
    std::size_t peak_stack_values = 0;
};
VmResult execute_numeric(const Program &program, VmOptions options = {});
VmResult execute(const Program &program, ExecutionContext &context, VmOptions options = {});
double value_number(const Value &value);
std::string value_text(const Value &value);
std::vector<std::uint8_t> demo_bytecode();
} // namespace regml
