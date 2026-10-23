#include "gmrt_ir.hpp"
#include <array>
#include <fstream>
#include <limits>

namespace regml {
namespace {
class FlatBufferReader {
  public:
    explicit FlatBufferReader(std::span<const std::uint8_t> bytes) : view_(bytes) {}

    [[nodiscard]] std::size_t root_table() const {
        view_.require(0, 4);
        const auto offset = static_cast<std::size_t>(view_.u32(0));
        view_.require(offset, 4);
        return offset;
    }

    [[nodiscard]] std::optional<std::size_t> field(std::size_t table, std::size_t index) const {
        view_.require(table, 4);
        const auto vtable_distance = static_cast<std::int64_t>(view_.i32(table));
        const auto vtable_signed = static_cast<std::int64_t>(table) - vtable_distance;
        if (vtable_signed < 0 || static_cast<std::uint64_t>(vtable_signed) > view_.size())
            throw Error(ErrorKind::malformed, "GMIR table has an invalid vtable", table);
        const auto vtable = static_cast<std::size_t>(vtable_signed);
        view_.require(vtable, 4);
        const auto vtable_size = static_cast<std::size_t>(view_.u16(vtable));
        const auto object_size = static_cast<std::size_t>(view_.u16(vtable + 2));
        if (vtable_size < 4 || (vtable_size & 1) != 0 || object_size < 4)
            throw Error(ErrorKind::malformed, "GMIR table has invalid layout sizes", vtable);
        view_.require(vtable, vtable_size);
        view_.require(table, object_size);
        if (index >= (vtable_size - 4) / 2)
            return std::nullopt;
        const auto relative = static_cast<std::size_t>(view_.u16(vtable + 4 + index * 2));
        if (relative == 0)
            return std::nullopt;
        if (relative >= object_size)
            throw Error(ErrorKind::malformed, "GMIR table field exceeds its object", table + relative);
        return table + relative;
    }

    [[nodiscard]] std::int32_t i32(std::size_t table, std::size_t index, std::int32_t fallback = 0) const {
        const auto at = field(table, index);
        return at ? view_.i32(*at) : fallback;
    }
    [[nodiscard]] std::uint8_t u8(std::size_t table, std::size_t index, std::uint8_t fallback = 0) const {
        const auto at = field(table, index);
        return at ? view_.u8(*at) : fallback;
    }
    [[nodiscard]] bool boolean(std::size_t table, std::size_t index, bool fallback = false) const {
        return u8(table, index, fallback ? 1 : 0) != 0;
    }
    [[nodiscard]] std::optional<std::size_t> indirect_field(std::size_t table, std::size_t index) const {
        const auto at = field(table, index);
        return at ? indirect(*at) : std::nullopt;
    }
    [[nodiscard]] std::string string_field(std::size_t table, std::size_t index) const {
        const auto target = indirect_field(table, index);
        return target ? string(*target) : std::string{};
    }
    [[nodiscard]] std::optional<GmrtSourceLocation> location_field(std::size_t table,
                                                                   std::size_t index) const {
        const auto target = indirect_field(table, index);
        if (!target)
            return std::nullopt;
        GmrtSourceLocation location;
        location.line = i32(*target, 0);
        location.column = i32(*target, 1);
        location.file = string_field(*target, 2);
        return location;
    }
    [[nodiscard]] std::vector<std::size_t> table_vector(std::size_t table, std::size_t field_index) const {
        const auto vector = indirect_field(table, field_index);
        if (!vector)
            return {};
        const auto count = vector_length(*vector, sizeof(std::uint32_t));
        std::vector<std::size_t> result;
        result.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto element = *vector + 4 + i * 4;
            const auto target = indirect(element);
            if (!target)
                throw Error(ErrorKind::malformed, "GMIR table vector contains a null element", element);
            result.push_back(*target);
        }
        return result;
    }
    [[nodiscard]] std::vector<std::uint16_t> u16_vector(std::size_t table, std::size_t field_index) const {
        const auto vector = indirect_field(table, field_index);
        if (!vector)
            return {};
        const auto count = vector_length(*vector, sizeof(std::uint16_t));
        std::vector<std::uint16_t> result;
        result.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
            result.push_back(view_.u16(*vector + 4 + i * 2));
        return result;
    }
    [[nodiscard]] std::vector<std::uint8_t> u8_vector(std::size_t table, std::size_t field_index) const {
        const auto vector = indirect_field(table, field_index);
        if (!vector)
            return {};
        const auto count = vector_length(*vector, sizeof(std::uint8_t));
        std::vector<std::uint8_t> result;
        result.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
            result.push_back(view_.u8(*vector + 4 + i));
        return result;
    }
    [[nodiscard]] std::vector<std::uint64_t> u64_vector(std::size_t table, std::size_t field_index) const {
        const auto vector = indirect_field(table, field_index);
        if (!vector)
            return {};
        const auto count = vector_length(*vector, sizeof(std::uint64_t));
        std::vector<std::uint64_t> result;
        result.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
            result.push_back(view_.u64(*vector + 4 + i * 8));
        return result;
    }
    [[nodiscard]] std::vector<std::optional<std::size_t>> union_vector(std::size_t table,
                                                                       std::size_t field_index) const {
        const auto vector = indirect_field(table, field_index);
        if (!vector)
            return {};
        const auto count = vector_length(*vector, sizeof(std::uint32_t));
        std::vector<std::optional<std::size_t>> result;
        result.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
            result.push_back(indirect(*vector + 4 + i * 4));
        return result;
    }
    [[nodiscard]] std::optional<std::size_t> indirect(std::size_t field_offset) const {
        const auto distance = static_cast<std::size_t>(view_.u32(field_offset));
        if (distance == 0)
            return std::nullopt;
        if (distance > view_.size() - field_offset)
            throw Error(ErrorKind::malformed, "GMIR relative offset exceeds module bounds", field_offset);
        const auto target = field_offset + distance;
        view_.require(target, 1);
        return target;
    }
    [[nodiscard]] std::uint8_t read_u8(std::size_t offset) const { return view_.u8(offset); }
    [[nodiscard]] std::int32_t read_i32(std::size_t offset) const { return view_.i32(offset); }
    [[nodiscard]] std::int64_t read_i64(std::size_t offset) const { return view_.i64(offset); }
    [[nodiscard]] double read_f64(std::size_t offset) const { return view_.f64(offset); }
    [[nodiscard]] bool read_bool(std::size_t offset) const { return view_.u8(offset) != 0; }

  private:
    [[nodiscard]] std::size_t vector_length(std::size_t vector, std::size_t element_size) const {
        view_.require(vector, 4);
        const auto count = static_cast<std::size_t>(view_.u32(vector));
        if (count > (view_.size() - vector - 4) / element_size)
            throw Error(ErrorKind::malformed, "GMIR vector exceeds module bounds", vector);
        return count;
    }
    [[nodiscard]] std::string string(std::size_t string_offset) const {
        view_.require(string_offset, 4);
        const auto length = static_cast<std::size_t>(view_.u32(string_offset));
        view_.require(string_offset + 4, length);
        if (string_offset + 4 + length < view_.size() && view_.u8(string_offset + 4 + length) != 0)
            throw Error(ErrorKind::malformed, "GMIR string is missing its terminator", string_offset);
        return std::string(view_.text(string_offset + 4, length));
    }

    BinaryView view_;
};

GmrtConstant parse_constant(const FlatBufferReader &reader, GmrtConstantKind kind, std::size_t table) {
    GmrtConstant result;
    result.kind = kind;
    switch (kind) {
    case GmrtConstantKind::none:
        break;
    case GmrtConstantKind::real64: {
        const auto field = reader.field(table, 0);
        result.value = field ? reader.read_f64(*field) : 0.0;
        break;
    }
    case GmrtConstantKind::int32: {
        const auto field = reader.field(table, 0);
        result.value = field ? reader.read_i32(*field) : 0;
        break;
    }
    case GmrtConstantKind::int64: {
        const auto field = reader.field(table, 0);
        result.value = field ? reader.read_i64(*field) : std::int64_t{0};
        break;
    }
    case GmrtConstantKind::boolean: {
        const auto field = reader.field(table, 0);
        result.value = field ? reader.read_bool(*field) : false;
        break;
    }
    case GmrtConstantKind::string:
    case GmrtConstantKind::symbol:
    case GmrtConstantKind::immutable_global:
        result.value = reader.string_field(table, 0);
        break;
    case GmrtConstantKind::special: {
        const auto field = reader.field(table, 0);
        result.value = static_cast<std::int32_t>(field ? reader.read_u8(*field) : 0);
        break;
    }
    case GmrtConstantKind::function:
        result.value = reader.string_field(table, 0);
        break;
    }
    return result;
}

GmrtFunction parse_function(const FlatBufferReader &reader, std::size_t table) {
    GmrtFunction function;
    function.binary_offset = table;
    function.symbol = reader.string_field(table, 0);
    function.global_name = reader.string_field(table, 1);
    function.alias = reader.string_field(table, 2);
    function.flags = reader.i32(table, 3);
    function.argument_count = reader.i32(table, 4);
    function.try_catch_block_count = reader.i32(table, 5);
    function.signature_type = reader.u8(table, 6);
    function.dynamic_arguments = reader.boolean(table, 13);
    function.location = reader.location_field(table, 12);

    for (const auto register_table : reader.table_vector(table, 8)) {
        GmrtRegister reg;
        reg.name = reader.string_field(register_table, 0);
        reg.type = static_cast<GmrtValueType>(reader.u8(register_table, 1));
        function.registers.push_back(std::move(reg));
    }

    const auto constant_types = reader.u8_vector(table, 9);
    const auto constant_tables = reader.union_vector(table, 10);
    if (constant_types.size() != constant_tables.size())
        throw Error(ErrorKind::malformed, "GMIR constant type and value tables differ in length", table);
    function.constants.reserve(constant_types.size());
    for (std::size_t i = 0; i < constant_types.size(); ++i) {
        const auto kind = static_cast<GmrtConstantKind>(constant_types[i]);
        if (kind != GmrtConstantKind::none && !constant_tables[i])
            throw Error(ErrorKind::malformed, "GMIR constant union is missing its value", table);
        if (kind > GmrtConstantKind::function)
            throw Error(ErrorKind::unsupported, "unknown GMIR constant kind", table);
        function.constants.push_back(kind == GmrtConstantKind::none
                                         ? GmrtConstant{}
                                         : parse_constant(reader, kind, *constant_tables[i]));
    }

    for (const auto block_table : reader.table_vector(table, 11)) {
        GmrtBlock block;
        block.binary_offset = block_table;
        block.name = reader.string_field(block_table, 0);
        block.location = reader.location_field(block_table, 4);
        for (const auto instruction_table : reader.table_vector(block_table, 3)) {
            GmrtInstruction instruction;
            instruction.binary_offset = instruction_table;
            instruction.opcode = reader.u8(instruction_table, 0);
            instruction.arguments = reader.u16_vector(instruction_table, 1);
            instruction.location = reader.location_field(instruction_table, 2);
            block.instructions.push_back(std::move(instruction));
        }
        function.blocks.push_back(std::move(block));
    }
    return function;
}
} // namespace

GmrtModule GmrtModule::load(const std::filesystem::path &path, LoadLimits limits) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
        throw Error(ErrorKind::io, "cannot open GMIR module: " + path.string());
    const auto end = stream.tellg();
    if (end < 0)
        throw Error(ErrorKind::io, "cannot determine GMIR module size");
    const auto length = static_cast<std::uint64_t>(static_cast<std::streamoff>(end));
    if (length > limits.max_file_bytes || length > std::numeric_limits<std::size_t>::max() ||
        length > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()))
        throw Error(ErrorKind::unsupported, "GMIR module exceeds configured size limit");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    stream.seekg(0);
    if (!bytes.empty())
        stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream)
        throw Error(ErrorKind::io, "could not read GMIR module");
    return parse(bytes);
}

GmrtModule GmrtModule::parse(std::span<const std::uint8_t> bytes) {
    FlatBufferReader reader(bytes);
    const auto root = reader.root_table();
    GmrtModule module;
    module.language_version = reader.i32(root, 0);
    module.name = reader.string_field(root, 1);
    for (const auto function : reader.table_vector(root, 2))
        module.functions.push_back(parse_function(reader, function));
    module.global_startup_functions = reader.u64_vector(root, 3);
    return module;
}

std::string_view gmrt_opcode_name(std::uint8_t opcode) noexcept {
    static constexpr std::array<std::string_view, 62> names = {"ADD",
                                                               "SUB",
                                                               "MUL",
                                                               "DIV",
                                                               "MOD",
                                                               "AND",
                                                               "OR",
                                                               "XOR",
                                                               "NEG",
                                                               "PLUS",
                                                               "LNOT",
                                                               "BNOT",
                                                               "SHL",
                                                               "SHR",
                                                               "SAR",
                                                               "B",
                                                               "BT",
                                                               "BF",
                                                               "BJ",
                                                               "LDC",
                                                               "RET",
                                                               "RETV",
                                                               "CALL",
                                                               "STATIC_CALL",
                                                               "LT",
                                                               "LE",
                                                               "GT",
                                                               "GE",
                                                               "EQ",
                                                               "NE",
                                                               "IN",
                                                               "TYPE_EQ",
                                                               "TYPE_NE",
                                                               "MOV",
                                                               "MOVCC",
                                                               "TOCC",
                                                               "IDIV",
                                                               "PGET_A",
                                                               "PGET",
                                                               "PSET_A",
                                                               "PSET",
                                                               "IGETG",
                                                               "ISETG",
                                                               "IGETJ",
                                                               "ISETJ",
                                                               "PARRAY",
                                                               "IARRAY",
                                                               "RARRAY",
                                                               "CALLEE",
                                                               "OTHER",
                                                               "CLOSURE_OBJ",
                                                               "CLOSURE_NEW",
                                                               "ARGINIT",
                                                               "ARGC",
                                                               "ARGGET",
                                                               "ARGSET",
                                                               "THROW",
                                                               "TRY_ENTER",
                                                               "TRY_RESTORE",
                                                               "MAPPED_ARGS",
                                                               "UNMAPPED_ARGS",
                                                               "UNREACHABLE"};
    return opcode < names.size() ? names[opcode] : "UNKNOWN";
}

Value gmrt_constant_value(const GmrtConstant &constant) {
    auto value_as = [&]<typename T>() -> const T & {
        if (const auto *value = std::get_if<T>(&constant.value))
            return *value;
        throw Error(ErrorKind::malformed, "GMIR constant payload does not match constant kind " +
                                              std::to_string(static_cast<unsigned>(constant.kind)));
    };
    switch (constant.kind) {
    case GmrtConstantKind::none:
        return std::monostate{};
    case GmrtConstantKind::real64:
        return value_as.operator()<double>();
    case GmrtConstantKind::int32:
        return value_as.operator()<std::int32_t>();
    case GmrtConstantKind::int64:
        return static_cast<double>(value_as.operator()<std::int64_t>());
    case GmrtConstantKind::boolean:
        return value_as.operator()<bool>();
    case GmrtConstantKind::string:
    case GmrtConstantKind::symbol:
    case GmrtConstantKind::immutable_global:
    case GmrtConstantKind::function:
        return value_as.operator()<std::string>();
    case GmrtConstantKind::special:
        return std::monostate{};
    }
    return std::monostate{};
}
} // namespace regml
