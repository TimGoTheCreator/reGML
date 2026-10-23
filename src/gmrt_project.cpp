#include "gmrt_project.hpp"
#include "error.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <variant>

namespace regml {
namespace {
struct JsonValue {
    using Array = std::vector<JsonValue>;
    using Object = std::unordered_map<std::string, JsonValue>;
    std::variant<std::monostate, bool, double, std::string, Array, Object> value;

    [[nodiscard]] const JsonValue *get(std::string_view key) const {
        const auto *object = std::get_if<Object>(&value);
        if (!object)
            return nullptr;
        const auto found = object->find(std::string(key));
        return found == object->end() ? nullptr : &found->second;
    }
    [[nodiscard]] const Array *array() const { return std::get_if<Array>(&value); }
    [[nodiscard]] const Object *object() const { return std::get_if<Object>(&value); }
    [[nodiscard]] std::string_view string() const {
        const auto *text = std::get_if<std::string>(&value);
        return text ? std::string_view(*text) : std::string_view{};
    }
    [[nodiscard]] double number(double fallback = 0) const {
        const auto *n = std::get_if<double>(&value);
        return n ? *n : fallback;
    }
    [[nodiscard]] bool boolean(bool fallback = false) const {
        const auto *b = std::get_if<bool>(&value);
        return b ? *b : fallback;
    }
};

class JsonParser {
  public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    JsonValue parse() {
        auto result = parse_value(0);
        whitespace();
        if (position_ != input_.size())
            malformed("trailing content");
        return result;
    }

  private:
    [[noreturn]] void malformed(std::string_view reason) const {
        throw Error(ErrorKind::malformed,
                    "GMRT project JSON " + std::string(reason) + " at byte " +
                        std::to_string(position_));
    }
    void whitespace() {
        while (position_ < input_.size() &&
               (input_[position_] == ' ' || input_[position_] == '\t' || input_[position_] == '\r' ||
                input_[position_] == '\n'))
            ++position_;
    }
    bool consume(char ch) {
        whitespace();
        if (position_ >= input_.size() || input_[position_] != ch)
            return false;
        ++position_;
        return true;
    }
    static void append_utf8(std::string &out, std::uint32_t codepoint) {
        if (codepoint <= 0x7f)
            out.push_back(static_cast<char>(codepoint));
        else if (codepoint <= 0x7ff) {
            out.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else if (codepoint <= 0xffff) {
            out.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        }
    }
    std::uint32_t hex4() {
        if (input_.size() - position_ < 4)
            malformed("has a truncated Unicode escape");
        std::uint32_t codepoint = 0;
        for (int i = 0; i < 4; ++i) {
            const auto ch = input_[position_++];
            std::uint32_t digit;
            if (ch >= '0' && ch <= '9')
                digit = static_cast<std::uint32_t>(ch - '0');
            else if (ch >= 'a' && ch <= 'f')
                digit = static_cast<std::uint32_t>(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F')
                digit = static_cast<std::uint32_t>(ch - 'A' + 10);
            else
                malformed("has an invalid Unicode escape");
            codepoint = (codepoint << 4) | digit;
        }
        return codepoint;
    }
    std::string parse_string() {
        if (!consume('"'))
            malformed("expected a string");
        std::string result;
        while (position_ < input_.size()) {
            const auto ch = input_[position_++];
            if (ch == '"')
                return result;
            if (static_cast<unsigned char>(ch) < 0x20)
                malformed("contains a control character in a string");
            if (ch != '\\') {
                result.push_back(ch);
                continue;
            }
            if (position_ >= input_.size())
                malformed("has an incomplete escape");
            switch (input_[position_++]) {
            case '"':
                result.push_back('"');
                break;
            case '\\':
                result.push_back('\\');
                break;
            case '/':
                result.push_back('/');
                break;
            case 'b':
                result.push_back('\b');
                break;
            case 'f':
                result.push_back('\f');
                break;
            case 'n':
                result.push_back('\n');
                break;
            case 'r':
                result.push_back('\r');
                break;
            case 't':
                result.push_back('\t');
                break;
            case 'u': {
                auto codepoint = hex4();
                if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                    if (input_.size() - position_ < 6 || input_[position_] != '\\' ||
                        input_[position_ + 1] != 'u')
                        malformed("has an incomplete surrogate pair");
                    position_ += 2;
                    const auto low = hex4();
                    if (low < 0xdc00 || low > 0xdfff)
                        malformed("has an invalid surrogate pair");
                    codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff)
                    malformed("has an unpaired low surrogate");
                append_utf8(result, codepoint);
                break;
            }
            default:
                malformed("has an invalid escape");
            }
        }
        malformed("has an unterminated string");
    }
    JsonValue parse_number() {
        const auto start = position_;
        if (input_[position_] == '-')
            ++position_;
        if (position_ >= input_.size())
            malformed("has an invalid number");
        if (input_[position_] == '0')
            ++position_;
        else {
            if (input_[position_] < '1' || input_[position_] > '9')
                malformed("has an invalid number");
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9')
                ++position_;
        }
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            const auto fraction = position_;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9')
                ++position_;
            if (fraction == position_)
                malformed("has an invalid fractional number");
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-'))
                ++position_;
            const auto exponent = position_;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9')
                ++position_;
            if (exponent == position_)
                malformed("has an invalid exponent");
        }
        double value = 0;
        const auto text = input_.substr(start, position_ - start);
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(value))
            malformed("has a number outside the supported range");
        return JsonValue{value};
    }
    JsonValue parse_value(std::size_t depth) {
        if (depth > 64)
            malformed("exceeds the nesting limit");
        whitespace();
        if (position_ >= input_.size())
            malformed("ended early");
        const auto ch = input_[position_];
        if (ch == '"')
            return JsonValue{parse_string()};
        if (ch == '-' || (ch >= '0' && ch <= '9'))
            return parse_number();
        if (ch == '{') {
            ++position_;
            JsonValue::Object object;
            if (consume('}'))
                return JsonValue{std::move(object)};
            do {
                whitespace();
                if (position_ >= input_.size() || input_[position_] != '"')
                    malformed("expected an object key");
                auto key = parse_string();
                if (!consume(':'))
                    malformed("expected ':' after an object key");
                object.insert_or_assign(std::move(key), parse_value(depth + 1));
                if (object.size() > 1'000'000)
                    malformed("exceeds the object size limit");
            } while (consume(','));
            if (!consume('}'))
                malformed("expected '}'");
            return JsonValue{std::move(object)};
        }
        if (ch == '[') {
            ++position_;
            JsonValue::Array array;
            if (consume(']'))
                return JsonValue{std::move(array)};
            do {
                array.push_back(parse_value(depth + 1));
                if (array.size() > 1'000'000)
                    malformed("exceeds the array size limit");
            } while (consume(','));
            if (!consume(']'))
                malformed("expected ']'");
            return JsonValue{std::move(array)};
        }
        if (input_.substr(position_, 4) == "true") {
            position_ += 4;
            return JsonValue{true};
        }
        if (input_.substr(position_, 5) == "false") {
            position_ += 5;
            return JsonValue{false};
        }
        if (input_.substr(position_, 4) == "null") {
            position_ += 4;
            return JsonValue{};
        }
        malformed("has an invalid value");
    }

    std::string_view input_;
    std::size_t position_ = 0;
};

const JsonValue *field(const JsonValue &value, std::string_view name) { return value.get(name); }
std::string text_field(const JsonValue &value, std::string_view name, std::string fallback = {}) {
    const auto *item = field(value, name);
    return item ? std::string(item->string()) : std::move(fallback);
}
int int_field(const JsonValue &value, std::string_view name, int fallback = 0) {
    const auto *item = field(value, name);
    if (!item)
        return fallback;
    const auto number = item->number(fallback);
    if (!std::isfinite(number) || number < std::numeric_limits<int>::min() ||
        number > std::numeric_limits<int>::max())
        return fallback;
    return static_cast<int>(number);
}
double double_field(const JsonValue &value, std::string_view name, double fallback = 0) {
    const auto *item = field(value, name);
    return item ? item->number(fallback) : fallback;
}
bool bool_field(const JsonValue &value, std::string_view name, bool fallback = false) {
    const auto *item = field(value, name);
    return item ? item->boolean(fallback) : fallback;
}
const JsonValue::Array &array_field(const JsonValue &value, std::string_view name) {
    static const JsonValue::Array empty;
    const auto *item = field(value, name);
    const auto *array = item ? item->array() : nullptr;
    return array ? *array : empty;
}
const JsonValue &object_field(const JsonValue &value, std::string_view name) {
    static const JsonValue empty;
    const auto *item = field(value, name);
    return item && item->object() ? *item : empty;
}
int resource_id(const std::unordered_map<std::string, int> &refs,
                const std::string &name,
                int fallback = -1) {
    const auto found = refs.find(name);
    return found == refs.end() ? fallback : found->second;
}
std::filesystem::path find_index(const std::filesystem::path &wad_path) {
    auto base = wad_path.stem().string();
    if (const auto dot = base.find('.'); dot != std::string::npos)
        base.resize(dot);
    auto directory = wad_path.parent_path();
    for (int level = 0; level < 10 && !directory.empty(); ++level) {
        for (const auto &candidate : {directory / (base + ".wad.json"), directory / (base + ".json")})
            if (std::filesystem::is_regular_file(candidate))
                return candidate;
        const auto parent = directory.parent_path();
        if (parent == directory)
            break;
        directory = parent;
    }
    throw Error(ErrorKind::unsupported,
                "GMRT boot needs a companion <project>.wad.json resource index for " + wad_path.string());
}
JsonValue load_json(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        throw Error(ErrorKind::io, "cannot open GMRT resource index: " + path.string());
    const auto end = input.tellg();
    if (end < 0 || end > 64 * 1024 * 1024)
        throw Error(ErrorKind::unsupported, "GMRT resource index exceeds 64 MiB");
    std::string contents(static_cast<std::size_t>(end), '\0');
    input.seekg(0);
    if (!contents.empty())
        input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!input)
        throw Error(ErrorKind::io, "could not read GMRT resource index");
    return JsonParser(contents).parse();
}
void resize_for_id(auto &resources, int id) {
    if (id < 0 || id > 1'000'000)
        throw Error(ErrorKind::malformed, "GMRT resource index is outside supported range");
    if (resources.size() <= static_cast<std::size_t>(id))
        resources.resize(static_cast<std::size_t>(id) + 1);
}
} // namespace

GmrtProject GmrtProject::load_for_wad(const std::filesystem::path &wad_path) {
    GmrtProject project;
    project.index_path_ = find_index(wad_path);
    const auto root = load_json(project.index_path_);
    const auto &resources = array_field(root, "resources");
    if (resources.empty())
        throw Error(ErrorKind::malformed, "GMRT resource index has no resource package");
    const auto &elements = array_field(resources.front(), "elements");

    std::unordered_map<std::string, int> object_ids, room_ids, sprite_ids;
    std::vector<const JsonValue *> object_records, room_records, sprite_records, events;
    for (const auto &element : elements) {
        const auto type = text_field(element, "type");
        const auto name = text_field(element, "name");
        const auto &data = object_field(element, "data");
        if (type == "yyresourceref") {
            const int kind = int_field(data, "type", -1);
            const int id = int_field(data, "value", -1);
            if (!name.empty() && id >= 0) {
                project.asset_indices_.insert_or_assign(name, id);
                project.asset_types_.insert_or_assign(name, kind);
                if (kind == 1)
                    sprite_ids.insert_or_assign(name, id);
                else if (kind == 3)
                    object_ids.insert_or_assign(name, id);
                else if (kind == 4)
                    room_ids.insert_or_assign(name, id);
            }
        } else if (type == "yyproject") {
            project.start_room_ = int_field(data, "startRoomIndex", 0);
            project.width_ = std::clamp(int_field(data, "applicationWidth", 640), 1, 8192);
            project.height_ = std::clamp(int_field(data, "applicationHeight", 480), 1, 8192);
            for (const auto &entry : array_field(data, "roomOrder"))
                project.room_order_.push_back(static_cast<int>(entry.number(-1)));
        } else if (type == "yyobject")
            object_records.push_back(&element);
        else if (type == "yyroom")
            room_records.push_back(&element);
        else if (type == "yysprite")
            sprite_records.push_back(&element);
        else if (type == "yyevent")
            events.push_back(&element);
    }

    if (project.room_order_.empty())
        project.room_order_.push_back(project.start_room_);

    for (const auto *record : object_records) {
        const auto name = text_field(*record, "name");
        const auto id = resource_id(object_ids, name);
        resize_for_id(project.objects_, id);
        auto &object = project.objects_[static_cast<std::size_t>(id)];
        if (object.id >= 0)
            throw Error(ErrorKind::malformed, "duplicate GMRT object resource ID");
        object.name = name;
        object.id = id;
        const auto &data = object_field(*record, "data");
        object.parent = int_field(data, "parentIndex", -1);
        object.visible = bool_field(data, "visible", true);
        object.persistent = bool_field(data, "persistent", false);
        const auto &components = object_field(data, "components");
        const auto &sprite_component = object_field(components, "GM.Systems.spritemanager");
        const auto &collision_component = object_field(components, "GM.Systems.collision");
        object.sprite = int_field(sprite_component, "spriteIndex", -1);
        object.mask = int_field(sprite_component, "maskIndex", -1);
        object.solid = bool_field(collision_component, "solid", false);
        for (const auto &event : array_field(data, "eventList")) {
            const int script = int_field(event, "scriptIndex", -1);
            if (script < 0 || static_cast<std::size_t>(script) >= events.size())
                throw Error(ErrorKind::malformed, "GMRT object event refers to an invalid event index");
            const auto &event_data = object_field(*events[static_cast<std::size_t>(script)], "data");
            object.events.push_back({int_field(event, "eventType"), int_field(event, "eventNum"),
                                     text_field(event_data, "globalName")});
        }
    }

    for (const auto *record : sprite_records) {
        const auto name = text_field(*record, "name");
        const auto id = resource_id(sprite_ids, name);
        resize_for_id(project.sprites_, id);
        auto &sprite = project.sprites_[static_cast<std::size_t>(id)];
        if (sprite.id >= 0)
            throw Error(ErrorKind::malformed, "duplicate GMRT sprite resource ID");
        const auto &data = object_field(*record, "data");
        sprite.name = name;
        sprite.id = id;
        sprite.width = int_field(data, "width");
        sprite.height = int_field(data, "height");
        sprite.origin_x = int_field(data, "xOrig");
        sprite.origin_y = int_field(data, "yOrig");
        sprite.left = int_field(data, "bBoxLeft");
        sprite.right = int_field(data, "bBoxRight", sprite.width - 1);
        sprite.top = int_field(data, "bBoxTop");
        sprite.bottom = int_field(data, "bBoxBottom", sprite.height - 1);
        sprite.smooth = bool_field(data, "smooth", false);
        for (const auto &frame : array_field(data, "frames")) {
            const auto image = text_field(frame, "imagePath");
            sprite.frames.push_back(image.empty() ? std::filesystem::path{}
                                                  : std::filesystem::path(image));
        }
    }

    for (const auto *record : room_records) {
        const auto name = text_field(*record, "name");
        const auto id = resource_id(room_ids, name);
        resize_for_id(project.rooms_, id);
        auto &room = project.rooms_[static_cast<std::size_t>(id)];
        if (room.id >= 0)
            throw Error(ErrorKind::malformed, "duplicate GMRT room resource ID");
        const auto &data = object_field(*record, "data");
        room.name = name;
        room.id = id;
        room.width = std::clamp(int_field(data, "width", 640), 1, 8192);
        room.height = std::clamp(int_field(data, "height", 480), 1, 8192);
        room.speed = std::clamp(int_field(data, "speed", 60), 1, 1000);
        room.persistent = bool_field(data, "persistent", false);
        room.views_enabled = bool_field(data, "enableViews", false);
        for (const auto &instance : array_field(data, "instances")) {
            GmrtRoomPlacement placement;
            placement.object = int_field(instance, "index", -1);
            placement.id = int_field(instance, "id");
            placement.x = double_field(instance, "x");
            placement.y = double_field(instance, "y");
            placement.scale_x = double_field(instance, "scaleX", 1);
            placement.scale_y = double_field(instance, "scaleY", 1);
            placement.image_index = double_field(instance, "imageIndex");
            placement.image_speed = double_field(instance, "imageSpeed", 1);
            placement.rotation = double_field(instance, "rotation");
            const auto color = int_field(instance, "colour", -1);
            placement.color = color < 0 ? 0xffffffffU : static_cast<unsigned>(color);
            room.instances.push_back(placement);
        }
        std::unordered_map<int, int> instance_layers;
        for (const auto &layer_data : array_field(data, "layers")) {
            GmrtRoomLayer layer;
            layer.name = text_field(layer_data, "name");
            layer.id = int_field(layer_data, "id", -1);
            layer.depth = int_field(layer_data, "depth");
            layer.type = int_field(layer_data, "layerType");
            layer.x = double_field(layer_data, "xoffset");
            layer.y = double_field(layer_data, "yoffset");
            layer.visible = bool_field(layer_data, "visible", true);
            for (const auto &instance_id : array_field(layer_data, "instanceIDs")) {
                const auto value = static_cast<int>(instance_id.number(-1));
                layer.instances.push_back(value);
                instance_layers.insert_or_assign(value, layer.id);
            }
            for (const auto &sprite_data : array_field(layer_data, "sprites")) {
                GmrtRoomSprite sprite;
                sprite.sprite = int_field(sprite_data, "index", -1);
                sprite.id = int_field(sprite_data, "id");
                sprite.depth = layer.depth;
                sprite.x = double_field(sprite_data, "x");
                sprite.y = double_field(sprite_data, "y");
                sprite.scale_x = double_field(sprite_data, "xScale", 1);
                sprite.scale_y = double_field(sprite_data, "yScale", 1);
                sprite.frame = double_field(sprite_data, "headPosition");
                sprite.speed = double_field(sprite_data, "animationFPS", 1);
                sprite.rotation = double_field(sprite_data, "rotation");
                const auto color = int_field(sprite_data, "blend", 0xffffff);
                sprite.color = static_cast<unsigned>(color);
                sprite.alpha = double_field(sprite_data, "alpha", 1);
                layer.sprites.push_back(sprite);
            }
            room.layers.push_back(std::move(layer));
        }
        for (auto &placement : room.instances)
            if (const auto found = instance_layers.find(placement.id); found != instance_layers.end())
                placement.layer = found->second;
    }

    if (project.start_room_ < 0 || static_cast<std::size_t>(project.start_room_) >= project.rooms_.size() ||
        project.rooms_[static_cast<std::size_t>(project.start_room_)].id < 0)
        throw Error(ErrorKind::malformed, "GMRT start room is not present in the resource index");
    for (const auto room_id : project.room_order_)
        if (room_id < 0 || static_cast<std::size_t>(room_id) >= project.rooms_.size() ||
            project.rooms_[static_cast<std::size_t>(room_id)].id < 0)
            throw Error(ErrorKind::malformed, "GMRT room order contains an unknown room");
    return project;
}

int GmrtProject::asset_index(std::string_view name) const noexcept {
    const auto found = asset_indices_.find(std::string(name));
    return found == asset_indices_.end() ? -1 : found->second;
}

int GmrtProject::asset_type(std::string_view name) const noexcept {
    const auto found = asset_types_.find(std::string(name));
    return found == asset_types_.end() ? -1 : found->second;
}

bool GmrtProject::asset_exists(int index, int type) const noexcept {
    if (index < 0)
        return false;
    for (const auto &[name, asset_index] : asset_indices_) {
        if (asset_index == index) {
            const auto found = asset_types_.find(name);
            if (found != asset_types_.end() && found->second == type)
                return true;
        }
    }
    return false;
}
} // namespace regml
