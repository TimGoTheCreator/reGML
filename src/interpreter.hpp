#pragma once
#include "decoder.hpp"
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
namespace regml {
struct ArrayValue;
struct PackageString {
    std::string_view value;
    bool operator==(const PackageString &) const = default;
};
// Not thread-safe. Arrays are owned by the VM thread.
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
using Value = std::variant<std::int32_t, double, bool, std::string, PackageString, std::monostate, ArrayPtr>;
inline bool value_is_string(const Value &value) noexcept {
    return std::holds_alternative<std::string>(value) || std::holds_alternative<PackageString>(value);
}
inline std::string_view value_string_view(const Value &value) noexcept {
    if (const auto *string = std::get_if<std::string>(&value))
        return *string;
    if (const auto *string = std::get_if<PackageString>(&value))
        return string->value;
    return {};
}
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
struct VariableStoreHash {
    using is_transparent = void;
    [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
        return std::hash<std::string_view>{}(value);
    }
};
class VariableStore : public std::unordered_map<std::string_view, Value, VariableStoreHash, std::equal_to<>> {
    using Base = std::unordered_map<std::string_view, Value, VariableStoreHash, std::equal_to<>>;
    // Stores can be copied out of a session, so shared key backing stays valid
    // until process exit.
    static const std::string &intern(std::string_view key) {
        static std::unordered_set<std::string, VariableStoreHash, std::equal_to<>> keys;
        static std::mutex keys_mutex;
        const std::lock_guard lock(keys_mutex);
        const auto found = keys.find(key);
        return found == keys.end() ? *keys.emplace(key).first : *found;
    }

  public:
    using Base::Base;
    using Base::operator=;

    Value &at(std::string_view key) {
        const auto found = this->find(key);
        if (found == this->end())
            throw std::out_of_range("variable not found");
        return found->second;
    }
    const Value &at(std::string_view key) const {
        const auto found = this->find(key);
        if (found == this->end())
            throw std::out_of_range("variable not found");
        return found->second;
    }
    Value &at(const char *key) { return at(std::string_view(key)); }
    const Value &at(const char *key) const { return at(std::string_view(key)); }

    Value &operator[](std::string_view key) {
        const auto found = this->find(key);
        if (found != this->end())
            return found->second;
        return this->try_emplace(intern(key)).first->second;
    }
    Value &operator[](const char *key) { return (*this)[std::string_view(key)]; }
    std::pair<iterator, bool> emplace(std::string_view key, Value value) {
        return this->try_emplace(intern(key), std::move(value));
    }
};
enum class VariableScope { local, self, global };
struct VariableBinding {
    VariableScope scope;
    std::string name;
};
// Binding offsets refer to instructions, not occurrence-chain words.
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
bool compare_numbers(double left, double right, std::uint8_t kind, double epsilon = 0.00001);
std::vector<std::uint8_t> demo_bytecode();
} // namespace regml
