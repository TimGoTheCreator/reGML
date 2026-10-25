#include "game.hpp"
#include "audio/sdl/audio.hpp"
#include "gmrt_ir.hpp"
#include "gmrt_project.hpp"
#include "gmrt_vm.hpp"
#include "gmrt_wad.hpp"
#include "instance_ptr.hpp"
#include "interpreter.hpp"
#include "linker.hpp"
#include "rendersoftware/assets.hpp"
#if REGML_WIIU
#include "platform/wiiu/paths.hpp"
#endif
#if REGML_WII
#include "platform/wii/host.hpp"
#endif
#if REGML_PS2
#include "platform/ps2/gpu.hpp"
#endif
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#if REGML_PS2
#include <cstdio>
#include <malloc.h>
extern "C" {
volatile std::uint32_t regml_ps2_active_pc = 0;
volatile std::uint32_t regml_ps2_active_opcode = 0;
volatile std::uint32_t regml_ps2_active_symbol = 0;
const char *regml_ps2_active_code_name = "";
volatile std::uint32_t regml_ps2_active_code_name_length = 0;
}
#endif
namespace regml {
namespace {
struct GameChangeRequested {};

#if REGML_PS2
void ps2_memory_trace(std::string_view stage, std::size_t position = 0, std::size_t map_entries = 0) {
    static const bool enabled = [] {
        auto *marker = std::fopen("host:reGML/regml-trace.enabled", "r");
        if (!marker)
            marker = std::fopen("host:reGML/regml-profile.enabled", "r");
        if (!marker)
            return false;
        std::fclose(marker);
        return true;
    }();
    if (!enabled)
        return;
    const auto memory = mallinfo();
    if (auto *log = std::fopen("host:reGML/regml-load.log", "a")) {
        std::fprintf(log, "%.*s at %u heap=%u free=%u arena=%u maps=%u\n", static_cast<int>(stage.size()),
                     stage.data(), static_cast<unsigned>(position), static_cast<unsigned>(memory.uordblks),
                     static_cast<unsigned>(memory.fordblks), static_cast<unsigned>(memory.arena),
                     static_cast<unsigned>(map_entries));
        std::fclose(log);
    }
}
void ps2_error_trace(std::string_view stage, std::size_t position, std::string_view message) {
    if (auto *log = std::fopen("host:reGML/regml-error.log", "a")) {
        std::fprintf(log, "%.*s at %u: %.*s\n", static_cast<int>(stage.size()), stage.data(),
                     static_cast<unsigned>(position), static_cast<int>(message.size()), message.data());
        std::fclose(log);
    }
}
void ps2_oom_trace(const char *stage, std::size_t frame, std::size_t map_entries) {
    const auto memory = mallinfo();
    if (auto *log = std::fopen("host:reGML/regml-error.log", "a")) {
        std::fprintf(log, "out of memory during %s frame=%u heap=%u free=%u arena=%u maps=%u\n", stage,
                     static_cast<unsigned>(frame), static_cast<unsigned>(memory.uordblks),
                     static_cast<unsigned>(memory.fordblks), static_cast<unsigned>(memory.arena),
                     static_cast<unsigned>(map_entries));
        std::fclose(log);
    }
}
#endif
#if REGML_WII || REGML_LOW_MEMORY
constexpr bool build_instruction_index = false;
constexpr bool lazy_programs = true;
#else
constexpr bool build_instruction_index = true;
constexpr bool lazy_programs = false;
#endif
template <class Factory> auto load_stage(std::string_view name, Factory factory) {
#if REGML_PS2
    ps2_memory_trace(name);
#endif
#if REGML_WII
    struct MemoryReport {
        std::string_view name;
        ~MemoryReport() { report_wii_memory(name); }
    } report{name};
    report_wii_memory(name);
#endif
    try {
        return factory();
    } catch (const std::bad_alloc &) {
        throw Error(ErrorKind::io, "out of memory while loading " + std::string(name));
    }
}
int number(const Value &v) {
    const auto n = value_number(v);
    if (!std::isfinite(n) || n < INT32_MIN || n > INT32_MAX)
        throw Error(ErrorKind::vm, "integer value out of range");
    return static_cast<int>(n);
}
std::filesystem::path default_save_root() {
#if REGML_WIIU
    return wiiu_save_directory();
#else
    return std::filesystem::current_path() / "build" / "saves";
#endif
}
std::string save_profile(const Package &package, const std::filesystem::path &package_path) {
    std::string profile;
    if (const auto &info = package.general_info(); info && info->name_pointer != 0)
        profile = package.string_pointer(info->name_pointer);
    if (profile.empty()) {
        profile = package_path.parent_path().filename().string();
        if (profile.empty())
            profile = package_path.stem().string();
    }
    std::string safe;
    safe.reserve(std::min<std::size_t>(profile.size(), 64));
    for (const unsigned char ch : profile) {
        if (safe.size() == 64)
            break;
        safe.push_back(std::isalnum(ch) || ch == '-' || ch == '_' ? static_cast<char>(ch) : '_');
    }
    return safe.empty() ? "game" : safe;
}
const std::array<std::array<std::string, 2>, 16> &argument_variable_names() {
    static const auto names = [] {
        std::array<std::array<std::string, 2>, 16> result;
        for (std::size_t i = 0; i < result.size(); ++i) {
            const auto suffix = std::to_string(i);
            result[i][0] = "argument" + suffix;
            result[i][1] = "argument[" + suffix + "]";
        }
        return result;
    }();
    return names;
}
Package load_game_package(const std::filesystem::path &path) {
    return Package::load(path);
}
struct Object {
    using EventKey = std::pair<int, int>;
    struct EventCode {
        EventKey key;
        std::vector<int> code;
    };
    struct EventOwner {
        EventKey key;
        int object = -1;
    };
    std::string_view name;
    int sprite = -1, mask = -1, parent = -1, depth = 0;
    bool visible = false, persistent = false, solid = false;
    bool has_collision_events = false, has_keyboard_events = false;
    std::vector<EventCode> events;
    std::vector<EventOwner> event_owners;
    std::array<int, 3> step_owners{-1, -1, -1};
    int draw_owner = -1;
};
const Object::EventCode *find_event(const Object &object, Object::EventKey key) {
    const auto found = std::lower_bound(
        object.events.begin(), object.events.end(), key,
        [](const Object::EventCode &event, Object::EventKey target) { return event.key < target; });
    return found != object.events.end() && found->key == key ? &*found : nullptr;
}
int find_event_owner(const Object &object, Object::EventKey key) {
    const auto found = std::lower_bound(
        object.event_owners.begin(), object.event_owners.end(), key,
        [](const Object::EventOwner &event, Object::EventKey target) { return event.key < target; });
    return found != object.event_owners.end() && found->key == key ? found->object : -1;
}
struct Placement {
    int object = -1, id = 0, create = -1, precreate = -1;
    int layer = -1;
    double x = 0, y = 0, sx = 1, sy = 1, image_speed = 1, image_index = 0, rotation = 0;
    unsigned color = 0xffffffff;
};
struct LayerSprite {
    int id = 0, sprite = -1;
    double x = 0, y = 0, sx = 1, sy = 1, frame = 0, speed = 1, rotation = 0;
    unsigned color = 0xffffffff;
    int speed_type = 1;
};
struct LayerBackground {
    int id = 0, sprite = -1;
    double frame = 0, speed = 1, sx = 1, sy = 1;
    unsigned color = 0xffffffff;
    bool visible = true, foreground = false, htiled = false, vtiled = false, stretch = false;
    int speed_type = 1;
};
struct RoomLayer {
    std::string name;
    int id = -1, depth = 0, type = 0, shader = -1;
    double x = 0, y = 0, origin_x = 0, origin_y = 0, hspeed = 0, vspeed = 0;
    bool visible = true;
    int tile_element_id = -1;
    Value begin_script = std::monostate{}, end_script = std::monostate{};
    std::vector<int> instances;
    std::vector<LayerSprite> sprites;
    std::vector<LayerBackground> backgrounds;
};
struct Background {
    int id = -1, x = 0, y = 0;
    bool foreground = false;
};
struct Tile {
    int id = -1, element_id = -1;
    double x = 0, y = 0;
    int source_x = 0, source_y = 0, width = 0, height = 0, depth = 0;
    double sx = 1, sy = 1;
    unsigned color = 0xffffffff;
    int layer_id = -1;
    std::uint32_t transform = 0;
    std::uint32_t tileset_index = 0;
    bool modern_tileset = false, visible = true;
};
struct Room {
    std::size_t source_offset = 0;
    bool payload_loaded = false, has_layers = false;
    std::string_view name;
    int width = 640, height = 480, create = -1;
    double speed = 30;
    bool persistent = false;
    unsigned color = 0;
    std::vector<Placement> instances;
    std::vector<RoomLayer> layers;
    std::unordered_map<int, std::size_t> layer_indices;
    VariableStore views;
    std::vector<Background> backgrounds;
    std::vector<Tile> tiles;
};
struct Camera {
    double x = 0, y = 0, width = 0, height = 0, angle = 0;
    double border_x = 0, border_y = 0, speed_x = -1, speed_y = -1;
    int target = -1;
};
struct PathPoint {
    double x = 0, y = 0, speed = 100, distance = 0;
};
struct Path {
    bool smooth = false;
    unsigned precision = 4;
    double length = 0;
    std::vector<PathPoint> points;
};
struct Instance {
    int id = 0, object = 0;
    std::size_t references = 0;
    bool alive = true, outside_room = false, deactivated = false;
    VariableStore vars;
};
using InstancePtr = regml::IntrusivePtr<Instance>;
struct GameBuffer {
    std::vector<std::uint8_t> bytes;
    std::size_t position = 0;
};
} // namespace
struct GameSession::Impl {
    Package package;
    std::optional<GmrtProject> gmrt_project;
    LinkedPackage linked;
    GameAssets assets;
    GameAudio audio;
    struct ShaderResource {
        std::string_view name;
        std::array<std::string_view, 6> sources{};
    };
    struct ShaderUniform {
        int shader = -1;
        std::string name;
        bool sampler = false;
        std::vector<double> values;
    };
    std::vector<ShaderResource> shaders;
    std::unordered_map<std::string, int> shader_uniform_ids;
    std::unordered_map<int, ShaderUniform> shader_uniforms;
    int next_shader_uniform = 1;
    std::map<std::pair<int, int>, int> sprite_texture_ids;
    std::unordered_map<int, std::pair<int, int>> sprite_textures;
    std::unordered_map<int, int> shader_texture_stages;
    int next_sprite_texture = 1;
    int active_shader = -1;
    int shoujo_shader = -1, shoujo_time_uniform = -1, shoujo_width_uniform = -1;
    std::array<int, 3> shoujo_sampler_uniforms{-1, -1, -1};
    std::array<bool, 3> shoujo_texture_failed{};
    std::atomic<float> audio_master_gain{1.0f};
    Canvas canvas{640, 480, true};
    std::vector<Path> paths;
    std::vector<Object> objects;
    struct GmrtCodeRef {
        std::size_t module = 0;
        std::size_t function = 0;
    };
    std::vector<GmrtModule> gmrt_modules;
    std::vector<GmrtCodeRef> gmrt_code_refs;
    std::unordered_map<std::string, GmrtCodeRef> gmrt_functions;
    std::unordered_map<std::string, StructPtr> gmrt_function_properties;
    std::unordered_map<const StructValue *, InstancePtr> gmrt_instance_values;
    StructPtr gmrt_global_value;
    struct CollisionRoute {
        int source = -1, target = -1;
    };
    std::unordered_map<std::uint64_t, CollisionRoute> collision_routes;
    std::vector<CollisionRoute> tick_collision_routes;
    std::vector<std::uint32_t> tick_collision_route_stamps;
    std::uint32_t tick_collision_route_generation = 0;
    std::vector<Room> rooms;
    std::unordered_map<int, Camera> cameras;
    int next_camera_id = 8;
    std::vector<int> order, scripts;
    std::unordered_map<std::string_view, int, VariableStoreHash, std::equal_to<>> code_ids;
    std::unordered_map<std::string, int, VariableStoreHash, std::equal_to<>> script_names;
    std::vector<InstancePtr> instances;
    std::vector<InstancePtr> tick_instances;
    std::vector<InstancePtr> frame_instances;
    std::vector<InstancePtr> target_fallback;
    std::vector<InstancePtr> placed_instances;
    struct DrawEntry {
        double depth;
        std::size_t order;
        const Instance *instance;
    };
    std::vector<DrawEntry> draw_entries;
    struct SavedRoom {
        std::vector<InstancePtr> instances;
        VariableStore engine;
    };
    std::unordered_map<int, SavedRoom> saved_rooms;
    VariableStore globals, engine;
    std::unordered_map<int, VariableStore> static_variables;
    KeyboardState held{}, pressed{}, previous_held{}, released{};
    // ds maps can have any game data as keys, so they need their own storage
    // instead of sharing the bytecode name pool
    struct DynamicValue {
        using Storage = std::variant<std::int32_t, double, bool, PackageString, std::monostate, ArrayPtr,
                                     FunctionReference, CallablePtr, StructPtr, std::shared_ptr<std::string>>;
        Storage stored = std::monostate{};

        DynamicValue() = default;
        explicit DynamicValue(const Value &value) {
            std::visit(
                [&](const auto &item) {
                    using T = std::decay_t<decltype(item)>;
                    if constexpr (std::is_same_v<T, std::string>)
                        stored = std::make_shared<std::string>(item);
                    else
                        stored = item;
                },
                value);
        }

        Value to_value() const {
            return std::visit(
                [](const auto &item) -> Value {
                    using T = std::decay_t<decltype(item)>;
                    if constexpr (std::is_same_v<T, std::shared_ptr<std::string>>)
                        return *item;
                    else
                        return item;
                },
                stored);
        }
    };
    struct DynamicMapKeyView {
        bool string_key;
        std::string_view value;
    };
    struct DynamicMapHash {
        using is_transparent = void;

        static constexpr std::size_t offset_basis() noexcept {
            if constexpr (sizeof(std::size_t) == 8)
                return static_cast<std::size_t>(14695981039346656037ULL);
            else
                return static_cast<std::size_t>(2166136261U);
        }

        static constexpr std::size_t prime() noexcept {
            if constexpr (sizeof(std::size_t) == 8)
                return static_cast<std::size_t>(1099511628211ULL);
            else
                return static_cast<std::size_t>(16777619U);
        }

        static std::size_t append(std::size_t hash, std::string_view value) noexcept {
            for (const auto ch : value) {
                hash ^= static_cast<unsigned char>(ch);
                hash *= prime();
            }
            return hash;
        }

        std::size_t operator()(std::string_view value) const noexcept {
            return append(offset_basis(), value);
        }

        std::size_t operator()(DynamicMapKeyView key) const noexcept {
            auto hash = append(offset_basis(), key.string_key ? "s:" : "n:");
            return append(hash, key.value);
        }
    };
    struct DynamicMapEqual {
        using is_transparent = void;

        bool operator()(std::string_view left, std::string_view right) const noexcept {
            return left == right;
        }

        bool operator()(std::string_view stored, DynamicMapKeyView query) const noexcept {
            return stored.size() == query.value.size() + 2 &&
                   stored[0] == (query.string_key ? 's' : 'n') && stored[1] == ':' &&
                   stored.substr(2) == query.value;
        }

        bool operator()(DynamicMapKeyView query, std::string_view stored) const noexcept {
            return (*this)(stored, query);
        }
    };
    struct DynamicMap
        : std::unordered_map<std::string, DynamicValue, DynamicMapHash, DynamicMapEqual> {
        using Base = std::unordered_map<std::string, DynamicValue, DynamicMapHash, DynamicMapEqual>;

        DynamicMap() { this->max_load_factor(1.5f); }

        bool set(DynamicMapKeyView key, const Value &value, bool only_if_absent) {
            if (const auto found = this->find(key); found != this->end()) {
                if (!only_if_absent)
                    found->second = DynamicValue(value);
                return false;
            }

            std::string stored_key;
            stored_key.reserve(key.value.size() + 2);
            stored_key.append(key.string_key ? "s:" : "n:");
            stored_key.append(key.value);
            this->emplace(std::move(stored_key), DynamicValue(value));
            return true;
        }
    };
    std::unordered_map<int, DynamicMap> maps;
    std::unordered_map<int, std::vector<Value>> lists;
    struct PriorityItem {
        Value value;
        double priority = 0;
    };
    std::unordered_map<int, std::vector<PriorityItem>> priorities;
    std::unordered_map<int, GameBuffer> buffers;
    std::unordered_map<int, Canvas> surfaces;
    int next_surface = 1, surface_target = 0;
    std::unordered_set<int> hidden_tile_layers;
    std::unordered_map<int, std::pair<double, double>> tile_layer_offsets;
    int next_layer_element_id = 1'000'000, next_dynamic_layer_id = 1'000'000;
    bool vertex_format_building = false, force_draw_depth = false;
    int next_vertex_format = 1, next_vertex_buffer = 1, forced_depth = 0;
    std::vector<int> vertex_format_attributes;
    std::unordered_set<int> vertex_formats, vertex_buffers;
    int next_ds = 0;
    int next_buffer = 1, next_async_request = 1;
    std::unordered_map<int, std::unique_ptr<std::fstream>> files;
    int next_file = 0;
    std::filesystem::path save_dir = default_save_root(), ini_path;
    std::map<std::string, std::map<std::string, std::string>> ini;
    bool ini_dirty = false;
    bool ini_opened = false;
    unsigned draw_color = 0xffffff;
    double draw_alpha = 1;
    std::array<bool, 4> color_write_enable{true, true, true, true};
    bool blend_enabled = true;
    bool alpha_test_enabled = false;
    unsigned alpha_test_reference = 0;
    double comparison_epsilon = 0.00001;
    int draw_font = -1, halign = 0, valign = 0;
    std::size_t frame = 0, instruction_count = 0, depth = 0, event_budget = 0;
    bool graphics_prefetch_pending = false;
    std::array<std::size_t, 128> active_program_stack{};
    bool profile_tick_timing = false;
    double last_logic_ms = 0, last_draw_ms = 0;
    double last_draw_event_ms = 0, last_draw_builtin_ms = 0;
    std::array<double, 4> last_draw_builtin_categories{};
    double max_draw_builtin_call_ms = 0;
    std::string_view slowest_draw_builtin;
    std::size_t draw_event_depth = 0;
    int current_code = -1;
    std::size_t current_vm_address = 0;
    int current_room = -1, pending_room = -1, next_id = 200000;
    std::mt19937 random{0x7265474d};
    bool game_started = false;
    struct EventFrame {
        int type = 0, subtype = 0, object = 0;
    };
    std::vector<EventFrame> event_stack;
    struct VmScratch {
        VariableStore locals;
        struct VariableCacheEntry {
            VariableStore *store = nullptr;
            Value *value = nullptr;
            std::uint32_t symbol = Instruction::no_symbol;
            std::uint32_t generation = 0;
            std::uint32_t engine_size = 0;
        };
        std::vector<Value> stack, arguments;
        std::vector<InstancePtr> targets;
        std::vector<DataType> stack_types;
        std::array<VariableCacheEntry, 64> variable_cache{};
        std::uint32_t variable_cache_generation = 0;
        struct Environment {
            InstancePtr saved, saved_other;
            std::vector<InstancePtr> choices;
            std::size_t index = 0;
        };
        std::vector<Environment> environments;
        std::size_t environment_count = 0;
    };
    std::array<VmScratch, 128> vm_scratch;
    std::filesystem::path game_dir, package_path;
    std::optional<std::uint32_t> fixed_seed;
    std::optional<std::filesystem::path> pending_game_change;
    mutable std::unordered_map<std::string, std::vector<std::uint8_t>> load_chunks;
    explicit Impl(const std::filesystem::path &path,
                  std::optional<std::uint32_t> seed,
                  const std::filesystem::path &save_directory)
        : package(load_stage("package metadata", [&] { return load_game_package(path); })),
          gmrt_project(package.is_gmrt() ? std::optional<GmrtProject>(GmrtProject::load_for_wad(path))
                                         : std::nullopt),
          linked(load_stage("bytecode",
                            [&] {
                                if (package.is_gmrt())
                                    return LinkedPackage{};
                                return link_legacy_package(package, {}, false, build_instruction_index,
                                                           lazy_programs);
                            })),
          assets(load_stage("graphics metadata", [&] {
              if (gmrt_project)
                  return GameAssets(package, *gmrt_project);
              return GameAssets(package);
          })),
          audio(load_stage("audio metadata", [&] { return GameAudio(package, path.parent_path()); })),
          game_dir(path.parent_path()), package_path(path), fixed_seed(seed) {
        if (gmrt_project) {
            const auto wad = load_stage("GMRT modules", [&] { return GmrtWad::load(path); });
            gmrt_modules.reserve(wad.modules().size());
            for (std::size_t i = 0; i < wad.modules().size(); ++i)
                gmrt_modules.push_back(load_stage("GMIR module", [&] {
                    return GmrtModule::parse(wad.read_module(i));
                }));
            for (std::size_t module = 0; module < gmrt_modules.size(); ++module)
                for (std::size_t function = 0; function < gmrt_modules[module].functions.size(); ++function) {
                    const auto &entry = gmrt_modules[module].functions[function];
                    const GmrtCodeRef reference{module, function};
                    for (const auto *name : {&entry.global_name, &entry.symbol, &entry.alias})
                        if (!name->empty())
                            gmrt_functions.try_emplace(*name, reference);
                }
        }
        for (std::size_t i = 0; i < package.code().size(); ++i) {
            const auto name = package.code_name(i);
            if (!name.empty())
                code_ids.try_emplace(name, static_cast<int>(i));
        }
        if (seed)
            random.seed(*seed);
        if (!save_directory.empty())
            save_dir = save_directory;
        else
            save_dir /= save_profile(package, path);
        load_stage("world metadata", [&] {
            load_world();
            load_shaders();
            std::unordered_map<std::string, std::vector<std::uint8_t>>{}.swap(load_chunks);
            return true;
        });
        ArrayPtr object_depths(new ArrayValue);
        object_depths->row_lengths[0] = static_cast<std::int32_t>(objects.size());
        for (std::size_t i = 0; i < objects.size(); ++i)
            object_depths->elements[static_cast<int>(i)] = objects[i].depth;
        globals["__objectID2Depth"] = object_depths;
        // instructions already have the IDs they need, so we can drop these lookup maps now
        std::unordered_map<std::size_t, std::size_t>{}.swap(linked.variable_at);
        std::unordered_map<std::size_t, std::size_t>{}.swap(linked.function_at);
        engine = {{"os_type", std::int32_t{0}},
                  {"os_version", std::int32_t{0}},
                  {"os_browser", std::int32_t{-1}},
                  {"debug_mode", std::int32_t{0}},
                  {"working_directory", game_dir.string() + "/"},
                  {"program_directory", game_dir.string() + "/"},
                  {"display_aa", std::int32_t{0}},
                  {"application_surface", std::int32_t{0}},
                  {"current_time", 0.0},
                  {"delta_time", 33333.0},
                  {"background_color", std::int32_t{0}},
                  {"fps", 30.0},
                  {"fps_real", 30.0},
                  {"keyboard_key", std::int32_t{0}},
                  {"keyboard_lastkey", std::int32_t{0}},
                  {"keyboard_string", std::string{}},
                  {"mouse_x", 0.0},
                  {"mouse_y", 0.0},
                  {"mouse_button", std::int32_t{0}}};
        load_stage("initial room", [&] {
            enter(order.at(0));
            return true;
        });
    }
    void load_shaders() {
        if (gmrt_project)
            return;
        const auto *chunk = package.find_chunk("SHDR");
        if (!chunk)
            return;
        const auto entries = list(chunk->data_offset, *chunk);
        shaders.reserve(entries.size());
        for (const auto offset : entries) {
            const auto shader_record = record(offset, *chunk, 32);
            ShaderResource shader;
            const auto name_pointer = shader_record.u32(0);
            if (name_pointer)
                shader.name = package.string_pointer(name_pointer);
            for (std::size_t source = 0; source < shader.sources.size(); ++source) {
                const auto source_pointer = shader_record.u32(8 + source * 4);
                if (source_pointer)
                    shader.sources[source] = package.string_pointer(source_pointer);
            }
            if (shader.name == "shd_shoujo")
                shoujo_shader = static_cast<int>(shaders.size());
            shaders.push_back(std::move(shader));
        }
        if (shoujo_shader >= 0) {
            shoujo_time_uniform = shader_uniform_handle(shoujo_shader, "time", false);
            shoujo_width_uniform = shader_uniform_handle(shoujo_shader, "appSurfWidth", false);
            shoujo_sampler_uniforms = {shader_uniform_handle(shoujo_shader, "texGradient", true),
                                       shader_uniform_handle(shoujo_shader, "texBubble", true),
                                       shader_uniform_handle(shoujo_shader, "texStars", true)};
        }
    }
    bool shader_source_has(int shader, std::string_view identifier) const {
        if (shader < 0 || static_cast<std::size_t>(shader) >= shaders.size() || identifier.empty())
            return false;
        const auto identifier_char = [](unsigned char ch) { return std::isalnum(ch) || ch == '_'; };
        for (const auto source : shaders[static_cast<std::size_t>(shader)].sources) {
            std::size_t offset = 0;
            while ((offset = source.find(identifier, offset)) != std::string::npos) {
                const bool left_boundary = offset == 0 || !identifier_char(source[offset - 1]);
                const auto end = offset + identifier.size();
                const bool right_boundary = end == source.size() || !identifier_char(source[end]);
                if (left_boundary && right_boundary)
                    return true;
                offset = end;
            }
        }
        return false;
    }
    int shader_uniform_handle(int shader, std::string_view name, bool sampler) {
        if (!shader_source_has(shader, name))
            return -1;
        std::string key = std::to_string(shader) + (sampler ? ":s:" : ":u:") + std::string(name);
        if (const auto found = shader_uniform_ids.find(key); found != shader_uniform_ids.end())
            return found->second;
        const int handle = next_shader_uniform++;
        shader_uniform_ids.emplace(std::move(key), handle);
        shader_uniforms.emplace(handle, ShaderUniform{shader, std::string(name), sampler, {}});
        return handle;
    }
    double shader_uniform_value(int handle, double fallback) const {
        const auto found = shader_uniforms.find(handle);
        return found == shader_uniforms.end() || found->second.values.empty() ? fallback
                                                                                : found->second.values.front();
    }
    int sprite_texture_handle(int sprite, double frame) {
        if (!assets.sprite_exists(sprite) || !std::isfinite(frame))
            return -1;
        const auto count = assets.sprite(sprite).frames.size();
        if (count == 0)
            return -1;
        const double floored = std::floor(frame);
        auto index = static_cast<long long>(std::fmod(floored, static_cast<double>(count)));
        if (index < 0)
            index += static_cast<long long>(count);
        const auto key = std::pair{sprite, static_cast<int>(index)};
        if (const auto found = sprite_texture_ids.find(key); found != sprite_texture_ids.end())
            return found->second;
        const int handle = next_sprite_texture++;
        sprite_texture_ids.emplace(key, handle);
        sprite_textures.emplace(handle, key);
        return handle;
    }
    std::uint32_t sample_shader_stage(int sampler, double u, double v) {
        const auto slot = std::find(shoujo_sampler_uniforms.begin(), shoujo_sampler_uniforms.end(), sampler);
        if (slot == shoujo_sampler_uniforms.end())
            return 0;
        const auto slot_index = static_cast<std::size_t>(slot - shoujo_sampler_uniforms.begin());
        if (shoujo_texture_failed[slot_index])
            return 0;
        const auto stage = shader_texture_stages.find(sampler);
        if (stage == shader_texture_stages.end())
            return 0;
        const auto texture = sprite_textures.find(stage->second);
        if (texture == sprite_textures.end())
            return 0;
        try {
            return assets.sample_sprite(texture->second.first, texture->second.second, u, v);
        } catch (...) {
            shoujo_texture_failed[slot_index] = true;
            return 0;
        }
    }
    std::uint32_t shade_pixel(int x, int y, std::uint32_t rgba) {
        if (active_shader != shoujo_shader || (rgba >> 24) == 0)
            return rgba;
        const double width = shader_uniform_value(shoujo_width_uniform, canvas.logical_width());
        const double time = shader_uniform_value(shoujo_time_uniform, 0);
        if (!std::isfinite(width) || width <= 0 || !std::isfinite(time))
            return rgba;
        const auto fract = [](double value) { return value - std::floor(value); };
        const double frag_x = static_cast<double>(x), frag_y = static_cast<double>(y);
        const double gradient_x = fract(frag_x / width + time);
        auto gradient = sample_shader_stage(shoujo_sampler_uniforms[0], gradient_x, 0);
        const auto next_gradient = sample_shader_stage(shoujo_sampler_uniforms[0], gradient_x + 0.5 / 128.0, 0);
        const double mix_amount = fract(frag_x / (width / 128.0));
        std::array<double, 3> color{};
        for (std::size_t channel = 0; channel < color.size(); ++channel) {
            const unsigned shift = static_cast<unsigned>((2 - channel) * 8);
            color[channel] = ((gradient >> shift) & 255) * (1 - mix_amount) +
                             ((next_gradient >> shift) & 255) * mix_amount;
            color[channel] += std::abs(-1.0 + fract(frag_x / width) * 2.0) * 127.5;
        }
        const double bubble_x = fract(frag_x / width * 4.0 + time * 1.25);
        const double bubble_y = fract(frag_y / width * 4.0 + time * 1.20);
        const auto bubble = sample_shader_stage(shoujo_sampler_uniforms[1], bubble_x, bubble_y);
        const double bubble2_x = fract(frag_x / width * 4.0 + time * 2.65);
        const double bubble2_y = fract(frag_y / width * 4.0 + time * 2.60);
        const auto bubble2 = sample_shader_stage(shoujo_sampler_uniforms[1], bubble2_x, bubble2_y);
        const double stars_x = fract(frag_x / width * 4.0 + time * 1.95);
        const double stars_y = fract(frag_y / width * 4.0 + time * 1.90);
        const auto stars = sample_shader_stage(shoujo_sampler_uniforms[2], stars_x, stars_y);
        for (std::size_t channel = 0; channel < color.size(); ++channel) {
            const unsigned shift = static_cast<unsigned>((2 - channel) * 8);
            color[channel] += ((bubble >> shift) & 255) + ((bubble2 >> shift) & 255) * 0.15 +
                              ((stars >> shift) & 255);
        }
        std::uint32_t result = rgba & 0xff000000;
        for (std::size_t channel = 0; channel < color.size(); ++channel) {
            const unsigned shift = static_cast<unsigned>((2 - channel) * 8);
            result |= static_cast<std::uint32_t>(std::clamp(std::lround(color[channel]), 0L, 255L)) << shift;
        }
        return result;
    }
    static std::uint32_t pixel_shader(void *context, int x, int y, std::uint32_t rgba) noexcept {
        auto &session = *static_cast<Impl *>(context);
        try {
            return session.shade_pixel(x, y, rgba);
        } catch (...) {
            return rgba;
        }
    }
    bool shader_is_supported(int shader) const noexcept { return shader == shoujo_shader && shader >= 0; }
    void configure_shader(Canvas &target) noexcept {
        if (shader_is_supported(active_shader) && !target.gpu_target())
            target.pixel_shader(&Impl::pixel_shader, this);
        else
            target.pixel_shader(nullptr);
    }
    void set_shader_uniform_values(std::span<const Value> args) {
        if (args.size() < 2)
            return;
        const auto handle = number(args[0]);
        const auto found = shader_uniforms.find(handle);
        if (found == shader_uniforms.end() || found->second.sampler)
            return;
        auto &values = found->second.values;
        values.clear();
        if (const auto *array = std::get_if<ArrayPtr>(&args[1]); array && *array) {
            const int offset = args.size() > 2 ? std::max(number(args[2]), 0) : 0;
            const auto row_length = (*array)->row_lengths.find(0);
            const int length = row_length == (*array)->row_lengths.end()
                                   ? 0
                                   : std::max<std::int32_t>(row_length->second -
                                                                static_cast<std::int32_t>(offset),
                                                            0);
            values.reserve(static_cast<std::size_t>(length));
            for (int i = 0; i < length; ++i) {
                const auto *item = (*array)->elements.find(offset + i);
                values.push_back(item ? value_number(*item) : 0);
            }
            return;
        }
        values.reserve(args.size() - 1);
        for (std::size_t i = 1; i < args.size(); ++i)
            values.push_back(value_number(args[i]));
    }
    std::vector<std::size_t> list(std::size_t at, const Chunk &c) const {
        if (at < c.data_offset || at > c.data_offset + c.size || c.data_offset + c.size - at < 4)
            throw Error(ErrorKind::malformed, "resource list outside chunk", at);
        const auto &bytes = load_chunk(c);
        const BinaryView data(bytes, c.data_offset);
        const auto local = at - c.data_offset;
        const auto count = static_cast<std::size_t>(data.u32(local));
        if (count > 1'000'000 || count > (c.data_offset + c.size - at - 4) / 4)
            throw Error(ErrorKind::malformed, "invalid resource list", at);
        std::vector<std::size_t> out;
        for (std::size_t i = 0; i < count; ++i) {
            const auto p = data.u32(local + 4 + i * 4);
            if (p < c.data_offset || p >= c.data_offset + c.size)
                throw Error(ErrorKind::malformed, "resource pointer outside chunk", at + 4 + i * 4);
            out.push_back(p);
        }
        return out;
    }
    const std::vector<std::uint8_t> &load_chunk(const Chunk &c) const {
        const auto key = std::string(c.tag.data(), c.tag.size());
        auto [it, inserted] = load_chunks.try_emplace(key);
        if (inserted)
            it->second = package.read_bytes(c.data_offset, c.size);
        return it->second;
    }
    BinaryView record(std::size_t at, const Chunk &c, std::size_t length) const {
        if (at < c.data_offset || at > c.data_offset + c.size || length > c.data_offset + c.size - at)
            throw Error(ErrorKind::malformed, "resource record outside chunk", at);
        return BinaryView(load_chunk(c), c.data_offset).slice(at - c.data_offset, length);
    }
    void code_valid(int id, std::size_t offset) const {
        const auto code_count = linked.shared_code ? linked.code_to_program.size() : linked.programs.size();
        if (id < -1 || (id >= 0 && static_cast<std::size_t>(id) >= code_count))
            throw Error(ErrorKind::malformed,
                        "invalid resource code ID " + std::to_string(id) + " (entry count " +
                            std::to_string(code_count) + ")",
                        offset);
    }
    int gmrt_code_for(std::string_view name) {
        const auto found = gmrt_functions.find(std::string(name));
        if (found == gmrt_functions.end())
            return -1;
        gmrt_code_refs.push_back(found->second);
        return static_cast<int>(gmrt_code_refs.size() - 1);
    }
    void load_gmrt_world() {
        const auto &project = *gmrt_project;
        objects.resize(project.objects().size());
        for (std::size_t i = 0; i < project.objects().size(); ++i) {
            const auto &source = project.objects()[i];
            if (source.id < 0)
                continue;
            auto &object = objects[i];
            object.name = source.name;
            object.sprite = source.sprite;
            object.mask = source.mask;
            object.parent = source.parent;
            object.depth = source.depth;
            object.visible = source.visible;
            object.persistent = source.persistent;
            object.solid = source.solid;
            for (const auto &event : source.events) {
                const int code = gmrt_code_for(event.global_name);
                if (code < 0)
                    continue;
                const Object::EventKey key{event.type, event.number};
                const auto found = std::find_if(object.events.begin(), object.events.end(),
                                                [key](const Object::EventCode &entry) {
                                                    return entry.key == key;
                                                });
                if (found == object.events.end())
                    object.events.push_back({key, {code}});
                else
                    found->code.push_back(code);
            }
            std::sort(object.events.begin(), object.events.end(),
                      [](const Object::EventCode &left, const Object::EventCode &right) {
                          return left.key < right.key;
                      });
        }
        for (auto &object : objects) {
            const Object *definition = &object;
            for (std::size_t guard = 0; definition && guard < objects.size(); ++guard) {
                for (const auto &event : definition->events) {
                    object.has_collision_events |= event.key.first == 4;
                    object.has_keyboard_events |= event.key.first == 5 || event.key.first == 9;
                    object.event_owners.push_back({event.key, static_cast<int>(definition - objects.data())});
                }
                const int parent = definition->parent;
                definition = parent >= 0 && static_cast<std::size_t>(parent) < objects.size()
                                 ? &objects[static_cast<std::size_t>(parent)]
                                 : nullptr;
            }
            std::stable_sort(object.event_owners.begin(), object.event_owners.end(),
                             [](const Object::EventOwner &left, const Object::EventOwner &right) {
                                 return left.key < right.key;
                             });
            object.event_owners.erase(
                std::unique(object.event_owners.begin(), object.event_owners.end(),
                            [](const Object::EventOwner &left, const Object::EventOwner &right) {
                                return left.key == right.key;
                            }),
                object.event_owners.end());
            for (int step = 0; step < 3; ++step)
                object.step_owners[static_cast<std::size_t>(step)] = find_event_owner(object, {3, step});
            object.draw_owner = find_event_owner(object, {8, 0});
        }
        rooms.resize(project.rooms().size());
        for (std::size_t i = 0; i < project.rooms().size(); ++i) {
            const auto &source = project.rooms()[i];
            if (source.id < 0)
                continue;
            auto &room = rooms[i];
            room.name = source.name;
            room.width = source.width;
            room.height = source.height;
            room.speed = source.speed;
            room.persistent = source.persistent;
            room.color = source.color;
            room.has_layers = !source.layers.empty();
            room.payload_loaded = true;
            room.views["view_enabled"] = std::int32_t{source.views_enabled};
            room.views["view_current"] = std::int32_t{0};
            for (int view = 0; view < 8; ++view) {
                const auto suffix = "[" + std::to_string(view) + "]";
                room.views["view_visible" + suffix] = std::int32_t{0};
                room.views["view_xview" + suffix] = std::int32_t{0};
                room.views["view_yview" + suffix] = std::int32_t{0};
                room.views["view_wview" + suffix] = std::int32_t{0};
                room.views["view_hview" + suffix] = std::int32_t{0};
                room.views["view_xport" + suffix] = std::int32_t{0};
                room.views["view_yport" + suffix] = std::int32_t{0};
                room.views["view_wport" + suffix] = std::int32_t{0};
                room.views["view_hport" + suffix] = std::int32_t{0};
                room.views["view_hborder" + suffix] = std::int32_t{0};
                room.views["view_vborder" + suffix] = std::int32_t{0};
                room.views["view_hspeed" + suffix] = std::int32_t{0};
                room.views["view_vspeed" + suffix] = std::int32_t{0};
                room.views["view_object" + suffix] = std::int32_t{-1};
            }
            for (const auto &entry : source.instances) {
                if (entry.object < 0 || static_cast<std::size_t>(entry.object) >= objects.size())
                    throw Error(ErrorKind::malformed, "GMRT room placement has an invalid object ID");
                Placement placement;
                placement.object = entry.object;
                placement.id = entry.id;
                placement.layer = entry.layer;
                placement.x = entry.x;
                placement.y = entry.y;
                placement.sx = entry.scale_x;
                placement.sy = entry.scale_y;
                placement.image_speed = entry.image_speed;
                placement.image_index = entry.image_index;
                placement.rotation = entry.rotation;
                placement.color = entry.color;
                room.instances.push_back(placement);
            }
            for (const auto &source_layer : source.layers) {
                RoomLayer layer;
                layer.name = source_layer.name;
                layer.id = source_layer.id;
                layer.depth = source_layer.depth;
                layer.type = source_layer.type;
                layer.x = source_layer.x;
                layer.y = source_layer.y;
                layer.origin_x = source_layer.x;
                layer.origin_y = source_layer.y;
                layer.visible = source_layer.visible;
                layer.instances = source_layer.instances;
                for (const auto &source_sprite : source_layer.sprites) {
                    LayerSprite sprite;
                    sprite.id = source_sprite.id;
                    sprite.sprite = source_sprite.sprite;
                    sprite.x = source_sprite.x;
                    sprite.y = source_sprite.y;
                    sprite.sx = source_sprite.scale_x;
                    sprite.sy = source_sprite.scale_y;
                    sprite.frame = source_sprite.frame;
                    sprite.speed = source_sprite.speed;
                    sprite.rotation = source_sprite.rotation;
                    sprite.color = source_sprite.color;
                    layer.sprites.push_back(sprite);
                }
                room.layers.push_back(std::move(layer));
                room.layer_indices.try_emplace(room.layers.back().id, room.layers.size() - 1);
            }
        }
        order = project.room_order();
    }
    void load_world() {
        if (gmrt_project) {
            load_gmrt_world();
            return;
        }
        const auto *sc = package.find_chunk("SCPT");
        const auto *ob = package.find_chunk("OBJT");
        const auto *ro = package.find_chunk("ROOM");
        const auto *ge = package.find_chunk("GEN8");
        if (!sc || !ob || !ro || !ge)
            throw Error(ErrorKind::unsupported, "game requires SCPT/OBJT/ROOM/GEN8 resources");
        const std::size_t object_field_shift = package.find_chunk("FEAT") ? 4 : 0;
        for (auto p : list(sc->data_offset, *sc)) {
            const auto r = record(p, *sc, 8);
            const auto name = std::string(package.string_pointer(r.u32(0)));
            auto id = r.i32(4);
            if (linked.shared_code && id < -1)
                id = static_cast<std::int32_t>(static_cast<std::uint32_t>(id) & 0x7fffffffU);
            code_valid(id, p + 4);
            scripts.push_back(id);
            script_names.emplace(name, id);
        }
        if (const auto *chunk = package.find_chunk("PATH"))
            for (auto p : list(chunk->data_offset, *chunk)) {
                const auto header = record(p, *chunk, 20);
                Path path;
                path.smooth = header.u32(4) != 0;
                path.precision = std::clamp<std::uint32_t>(header.u32(12), 1, 8);
                const auto count = header.u32(16);
                if (count > 65536)
                    throw Error(ErrorKind::malformed, "path point limit", p);
                const auto points = record(p + 20, *chunk, static_cast<std::size_t>(count) * 12);
                for (std::size_t j = 0; j < count; ++j) {
                    PathPoint point;
                    point.x = std::bit_cast<float>(points.u32(j * 12));
                    point.y = std::bit_cast<float>(points.u32(j * 12 + 4));
                    point.speed = std::bit_cast<float>(points.u32(j * 12 + 8));
                    if (!path.points.empty())
                        path.length +=
                            std::hypot(point.x - path.points.back().x, point.y - path.points.back().y);
                    point.distance = path.length;
                    path.points.push_back(point);
                }
                const bool closed = header.u32(8) != 0;
                if (closed && path.points.size() > 1)
                    path.points.push_back(path.points.front());
                // looking up distance along the curve keeps it moving at a steady speed
                if (path.smooth && path.points.size() > 1) {
                    const auto controls = std::move(path.points);
                    path.points.clear();
                    path.length = 0;
                    const int subdivisions = static_cast<int>(path.precision * 8);
                    path.points.reserve((controls.size() - 1) * static_cast<std::size_t>(subdivisions + 1) +
                                        1);
                    for (std::size_t segment = 0; segment + 1 < controls.size(); ++segment) {
                        const auto &p1 = controls[segment];
                        const auto &p2 = controls[segment + 1];
                        const auto &p0 =
                            segment ? controls[segment - 1] : (closed ? controls[controls.size() - 2] : p1);
                        const auto &p3 = segment + 2 < controls.size() ? controls[segment + 2]
                                                                       : (closed ? controls[1] : p2);
                        for (int sample = 0; sample < subdivisions; ++sample) {
                            const double t = static_cast<double>(sample) / subdivisions, t2 = t * t,
                                         t3 = t2 * t;
                            const double x = 0.5 * ((2 * p1.x) + (-p0.x + p2.x) * t +
                                                    (2 * p0.x - 5 * p1.x + 4 * p2.x - p3.x) * t2 +
                                                    (-p0.x + 3 * p1.x - 3 * p2.x + p3.x) * t3);
                            const double y = 0.5 * ((2 * p1.y) + (-p0.y + p2.y) * t +
                                                    (2 * p0.y - 5 * p1.y + 4 * p2.y - p3.y) * t2 +
                                                    (-p0.y + 3 * p1.y - 3 * p2.y + p3.y) * t3);
                            const double speed = p1.speed + (p2.speed - p1.speed) * t;
                            PathPoint point{x, y, speed, 0};
                            if (!path.points.empty())
                                path.length += std::hypot(x - path.points.back().x, y - path.points.back().y);
                            point.distance = path.length;
                            path.points.push_back(point);
                        }
                    }
                    const auto &last = controls.back();
                    if (!path.points.empty())
                        path.length +=
                            std::hypot(last.x - path.points.back().x, last.y - path.points.back().y);
                    path.points.push_back({last.x, last.y, last.speed, path.length});
                } else {
                    path.length = 0;
                    for (std::size_t j = 0; j < path.points.size(); ++j) {
                        if (j)
                            path.length += std::hypot(path.points[j].x - path.points[j - 1].x,
                                                      path.points[j].y - path.points[j - 1].y);
                        path.points[j].distance = path.length;
                    }
                }
                paths.push_back(std::move(path));
            }
        for (auto p : list(ob->data_offset, *ob)) {
            auto r = record(p, *ob, 80 + object_field_shift);
            Object o;
            o.name = package.string_pointer(r.u32(0));
            o.sprite = r.i32(4);
            o.visible = r.u32(8) != 0;
            o.solid = r.u32(12 + object_field_shift) != 0;
            o.depth = r.i32(16 + object_field_shift);
            o.persistent = r.u32(20 + object_field_shift) != 0;
            o.parent = r.i32(24 + object_field_shift);
            o.mask = r.i32(28 + object_field_shift);
            const auto vertices = r.u32(64 + object_field_shift);
            if (vertices > 4096)
                throw Error(ErrorKind::unsupported, "physics vertices exceed limit", p + 64 + object_field_shift);
            const auto groups = list(p + 80 + object_field_shift + static_cast<std::size_t>(vertices) * 8, *ob);
            for (std::size_t type = 0; type < groups.size(); ++type)
                for (auto e : list(groups[type], *ob)) {
                    const auto er = record(e, *ob, 8);
                    const auto subtype = er.i32(0);
                    for (auto a : list(e + 4, *ob)) {
                        const auto ar = record(a, *ob, 56);
                        const auto code = ar.i32(32);
                        code_valid(code, a + 32);
                        if (code >= 0) {
                            const Object::EventKey key{static_cast<int>(type), subtype};
                            const auto found = std::find_if(
                                o.events.begin(), o.events.end(),
                                [key](const Object::EventCode &entry) { return entry.key == key; });
                            if (found == o.events.end())
                                o.events.push_back({key, {code}});
                            else
                                found->code.push_back(code);
                        }
                    }
                }
            std::sort(o.events.begin(), o.events.end(),
                      [](const Object::EventCode &left, const Object::EventCode &right) {
                          return left.key < right.key;
                      });
            objects.push_back(std::move(o));
        }
        for (auto &object : objects) {
            const Object *definition = &object;
            for (std::size_t guard = 0; definition && guard < objects.size(); ++guard) {
                for (const auto &event : definition->events) {
                    object.has_collision_events |= event.key.first == 4;
                    object.has_keyboard_events |= event.key.first == 5 || event.key.first == 9;
                    object.event_owners.push_back({event.key, static_cast<int>(definition - objects.data())});
                }
                const int parent = definition->parent;
                definition = parent >= 0 && static_cast<std::size_t>(parent) < objects.size()
                                 ? &objects[static_cast<std::size_t>(parent)]
                                 : nullptr;
            }
            std::stable_sort(object.event_owners.begin(), object.event_owners.end(),
                             [](const Object::EventOwner &left, const Object::EventOwner &right) {
                                 return left.key < right.key;
                             });
            object.event_owners.erase(
                std::unique(object.event_owners.begin(), object.event_owners.end(),
                            [](const Object::EventOwner &left, const Object::EventOwner &right) {
                                return left.key == right.key;
                            }),
                object.event_owners.end());
            for (int step = 0; step < 3; ++step)
                object.step_owners[static_cast<std::size_t>(step)] = find_event_owner(object, {3, step});
            object.draw_owner = find_event_owner(object, {8, 0});
        }
        for (auto p : list(ro->data_offset, *ro)) {
            const auto r = record(p, *ro, 88);
            Room room;
            room.name = package.string_pointer(r.u32(0));
            room.width = r.i32(8);
            room.height = r.i32(12);
            room.speed = r.i32(16);
            const bool modern_room = (r.u32(36) & (0x20000U | 0x40000U)) != 0;
            if (modern_room && room.speed == 0)
                room.speed = package.general_info()->game_speed;
            room.color = r.u32(24);
            room.create = r.i32(32);
            code_valid(room.create, p + 32);
            room.persistent = r.u32(20) != 0;
            room.has_layers = modern_room;
            room.source_offset = p;
            if (!std::isfinite(room.speed) || room.speed < 1 || room.speed > 1000 || room.width < 1 ||
                room.height < 1)
                throw Error(ErrorKind::malformed, "invalid room dimensions/speed", p);
#if !REGML_LOW_MEMORY
            load_room_payload(room);
#endif
            rooms.push_back(std::move(room));
        }
        const auto gr = record(ge->data_offset, *ge, ge->size);
        const auto count = gr.u32(128);
        if (count == 0 || count > rooms.size())
            throw Error(ErrorKind::malformed, "invalid room order", ge->data_offset + 128);
        for (std::size_t i = 0; i < count; ++i) {
            const auto id = gr.i32(132 + i * 4);
            if (id < 0 || static_cast<std::size_t>(id) >= rooms.size())
                throw Error(ErrorKind::malformed, "invalid ordered room");
            order.push_back(id);
        }
        // world data is already in structs, so drop the old chunks and save some Wii RAM
        load_chunks.clear();
    }
    void load_room_payload(Room &room) {
        if (room.payload_loaded)
            return;
        const auto *ro = package.find_chunk("ROOM");
        const auto p = room.source_offset;
        const auto r = record(p, *ro, room.has_layers ? 96 : 88);
        room.views["view_enabled"] = std::int32_t{(r.u32(36) & 1) != 0};
        room.views["view_current"] = std::int32_t{0};
        const auto view_list = list(r.u32(44), *ro);
        const std::string_view fields[] = {"view_visible", "view_xview",   "view_yview",   "view_wview",
                                           "view_hview",   "view_xport",   "view_yport",   "view_wport",
                                           "view_hport",   "view_hborder", "view_vborder", "view_hspeed",
                                           "view_vspeed",  "view_object"};
        for (std::size_t v = 0; v < view_list.size(); ++v) {
            const auto vr = record(view_list[v], *ro, 56);
            for (std::size_t f = 0; f < 14; ++f)
                room.views[std::string(fields[f]) + "[" + std::to_string(v) + "]"] = vr.i32(f * 4);
        }
        for (auto q : list(r.u32(48), *ro)) {
            const auto ir = record(q, *ro, linked.shared_code ? 48 : 40);
            Placement placement;
            placement.x = ir.i32(0);
            placement.y = ir.i32(4);
            placement.object = ir.i32(8);
            placement.id = ir.i32(12);
            placement.create = ir.i32(16);
            placement.sx = std::bit_cast<float>(ir.u32(20));
            placement.sy = std::bit_cast<float>(ir.u32(24));
            if (linked.shared_code) {
                placement.image_speed = std::bit_cast<float>(ir.u32(28));
                placement.image_index = ir.i32(32);
                placement.color = ir.u32(36);
                placement.rotation = std::bit_cast<float>(ir.u32(40));
                placement.precreate = ir.i32(44);
            } else
                placement.precreate = ir.i32(36);
            if (placement.object < 0 || static_cast<std::size_t>(placement.object) >= objects.size())
                throw Error(ErrorKind::malformed, "invalid placed object", q + 8);
            code_valid(placement.create, q + 16);
            code_valid(placement.precreate, q + 36);
            room.instances.push_back(placement);
        }
        const std::size_t layer_list_offset =
            package.general_info() && package.general_info()->bytecode_version >= 17 ? 92 : 88;
        if (room.has_layers && r.u32(layer_list_offset)) {
            std::unordered_map<int, int> instance_layers;
            for (const auto layer_offset : list(r.u32(layer_list_offset), *ro)) {
                const auto header = record(layer_offset, *ro, 56);
                RoomLayer layer;
                layer.name = package.string_pointer(header.u32(0));
                layer.id = static_cast<int>(header.u32(4));
                layer.type = static_cast<int>(header.u32(8));
                layer.depth = header.i32(12);
                layer.x = std::bit_cast<float>(header.u32(16));
                layer.y = std::bit_cast<float>(header.u32(20));
                layer.origin_x = layer.x;
                layer.origin_y = layer.y;
                layer.hspeed = std::bit_cast<float>(header.u32(24));
                layer.vspeed = std::bit_cast<float>(header.u32(28));
                layer.visible = header.u32(32) != 0;
                const auto effect_count = header.u32(44);
                if (effect_count > 4096)
                    throw Error(ErrorKind::malformed, "room layer effect property limit", layer_offset + 44);
                const auto layer_data_offset = layer_offset + 48 + static_cast<std::size_t>(effect_count) * 12;
                if (effect_count)
                    (void)record(layer_offset + 48, *ro, static_cast<std::size_t>(effect_count) * 12);
                if (layer.type == 2) {
                    const auto data = record(layer_data_offset, *ro, 4);
                    const auto count = data.u32(0);
                    if (count > 1'000'000)
                        throw Error(ErrorKind::malformed, "room layer instance limit", layer_data_offset);
                    const auto ids = record(layer_data_offset, *ro, 4 + static_cast<std::size_t>(count) * 4);
                    layer.instances.reserve(count);
                    for (std::size_t i = 0; i < count; ++i) {
                        const int instance_id = static_cast<int>(ids.u32(4 + i * 4));
                        layer.instances.push_back(instance_id);
                        instance_layers[instance_id] = layer.id;
                    }
                } else if (layer.type == 3) {
                    const auto data = record(layer_data_offset, *ro, 8);
                    const auto sprite_list = data.u32(4);
                    if (sprite_list) {
                        const auto sprites = list(sprite_list, *ro);
                        layer.sprites.reserve(sprites.size());
                        for (const auto sprite_offset : sprites) {
                            const auto sprite_record = record(sprite_offset, *ro, 44);
                            LayerSprite sprite;
                            sprite.id = next_layer_element_id++;
                            sprite.sprite = sprite_record.i32(4);
                            sprite.x = sprite_record.i32(8);
                            sprite.y = sprite_record.i32(12);
                            sprite.sx = std::bit_cast<float>(sprite_record.u32(16));
                            sprite.sy = std::bit_cast<float>(sprite_record.u32(20));
                            sprite.color = sprite_record.u32(24);
                            sprite.speed = std::bit_cast<float>(sprite_record.u32(28));
                            sprite.speed_type = sprite_record.i32(32);
                            sprite.frame = std::bit_cast<float>(sprite_record.u32(36));
                            sprite.rotation = std::bit_cast<float>(sprite_record.u32(40));
                            layer.sprites.push_back(sprite);
                        }
                    }
                } else if (layer.type == 1) {
                    const auto data = record(layer_data_offset, *ro, 40);
                    LayerBackground background;
                    background.id = next_layer_element_id++;
                    background.visible = data.u32(0) != 0;
                    background.foreground = data.u32(4) != 0;
                    background.sprite = data.i32(8);
                    background.htiled = data.u32(12) != 0;
                    background.vtiled = data.u32(16) != 0;
                    background.stretch = data.u32(20) != 0;
                    background.color = data.u32(24);
                    background.frame = std::bit_cast<float>(data.u32(28));
                    background.speed = std::bit_cast<float>(data.u32(32));
                    background.speed_type = data.i32(36);
                    layer.backgrounds.push_back(background);
                } else if (layer.type == 4) {
                    layer.tile_element_id = next_layer_element_id++;
                    const auto tile_header = record(layer_data_offset, *ro, 12);
                    const int tileset = tile_header.i32(0);
                    const auto columns = tile_header.u32(4), rows = tile_header.u32(8);
                    const auto cell_count = static_cast<std::uint64_t>(columns) * rows;
                    if (cell_count > 1'000'000)
                        throw Error(ErrorKind::malformed, "room tilemap cell limit", layer_data_offset);
                    const auto data = record(layer_data_offset,
                                             *ro,
                                             12 + static_cast<std::size_t>(cell_count) * sizeof(std::uint32_t));
                    for (std::uint32_t y = 0; y < rows; ++y)
                        for (std::uint32_t x = 0; x < columns; ++x) {
                            const auto tile = data.u32(12 + (static_cast<std::size_t>(y) * columns + x) * 4);
                            const auto tile_index = tile & 0x7ffffU;
                            if (tile_index == 0)
                                continue;
                            const auto source = assets.background_tile_source(tileset, tile_index);
                            if (!source)
                                continue;
                            Tile entry;
                            entry.id = tileset;
                            entry.element_id = layer.tile_element_id;
                            entry.x = layer.x + static_cast<double>(x * source->width);
                            entry.y = layer.y + static_cast<double>(y * source->height);
                            entry.source_x = source->x;
                            entry.source_y = source->y;
                            entry.width = source->width;
                            entry.height = source->height;
                            entry.depth = layer.depth;
                            entry.layer_id = layer.id;
                            entry.transform = tile & ((1U << 28) | (1U << 29) | (1U << 30));
                            entry.tileset_index = tile_index;
                            entry.modern_tileset = true;
                            room.tiles.push_back(entry);
                        }
                }
                room.layers.push_back(std::move(layer));
                room.layer_indices.try_emplace(room.layers.back().id, room.layers.size() - 1);
            }
            for (auto &placement : room.instances)
                if (const auto found = instance_layers.find(placement.id); found != instance_layers.end())
                    placement.layer = found->second;
        }
        for (auto q : list(r.u32(40), *ro)) {
            const auto b = record(q, *ro, 40);
            if (b.i32(0) && b.i32(8) >= 0)
                room.backgrounds.push_back({b.i32(8), b.i32(12), b.i32(16), b.i32(4) != 0});
        }
        for (auto q : list(r.u32(52), *ro)) {
            const auto t = record(q, *ro, 48);
            Tile tile;
            tile.id = t.i32(8);
            tile.element_id = next_layer_element_id++;
            tile.x = t.i32(0);
            tile.y = t.i32(4);
            tile.source_x = t.i32(12);
            tile.source_y = t.i32(16);
            tile.width = t.i32(20);
            tile.height = t.i32(24);
            tile.depth = t.i32(28);
            tile.sx = std::bit_cast<float>(t.u32(36));
            tile.sy = std::bit_cast<float>(t.u32(40));
            tile.color = t.u32(44);
            room.tiles.push_back(tile);
        }
        std::stable_sort(room.tiles.begin(), room.tiles.end(),
                         [](const Tile &a, const Tile &b) { return a.depth > b.depth; });
        room.payload_loaded = true;
    }
    InstancePtr make(const Placement &p) {
        static constexpr std::array<std::string_view, 12> alarm_variables = {
            "alarm[0]", "alarm[1]", "alarm[2]", "alarm[3]", "alarm[4]",  "alarm[5]",
            "alarm[6]", "alarm[7]", "alarm[8]", "alarm[9]", "alarm[10]", "alarm[11]"};
        if (instances.size() >= 10000)
            throw Error(ErrorKind::vm, "instance limit reached");
        const auto &obj = objects.at(static_cast<std::size_t>(p.object));
        auto inst = InstancePtr(new Instance());
        inst->id = p.id;
        inst->object = p.object;
        inst->vars.reserve(48);
        inst->vars = {{"x", p.x},
                      {"y", p.y},
                      {"xprevious", p.x},
                      {"yprevious", p.y},
                      {"xstart", p.x},
                      {"ystart", p.y},
                      {"id", std::int32_t{p.id}},
                      {"object_index", std::int32_t{p.object}},
                      {"layer", std::int32_t{p.layer}},
                      {"sprite_index", std::int32_t{obj.sprite}},
                      {"mask_index", std::int32_t{obj.mask}},
                      {"visible", std::int32_t{obj.visible}},
                      {"persistent", std::int32_t{obj.persistent}},
                      {"solid", std::int32_t{obj.solid}},
                      {"depth", std::int32_t{obj.depth}},
                      {"image_index", p.image_index},
                      {"image_speed", p.image_speed},
                      {"image_xscale", p.sx},
                      {"image_yscale", p.sy},
                      {"image_angle", p.rotation},
                      {"image_alpha", static_cast<double>((p.color >> 24) & 0xff) / 255.0},
                      {"image_blend", static_cast<std::int32_t>(p.color & 0xffffffU)},
                      {"hspeed", 0.0},
                      {"vspeed", 0.0},
                      {"speed", 0.0},
                      {"direction", 0.0},
                      {"friction", 0.0},
                      {"gravity", 0.0},
                      {"gravity_direction", 270.0},
                      {"path_index", std::int32_t{-1}},
                      {"path_position", 0.0},
                      {"path_speed", 0.0}};
        for (const auto name : alarm_variables)
            inst->vars[name] = std::int32_t{-1};
        instances.push_back(inst);
        return inst;
    }
    struct Bounds {
        double left, right, top, bottom;
        bool valid;
    };
    Bounds bounds(const Instance &i, double x, double y, bool full_sprite = false) const {
        int id = full_sprite ? -1 : number(i.vars.at("mask_index"));
        if (id < 0)
            id = number(i.vars.at("sprite_index"));
        if (id < 0)
            return {x, x, y, y, false};
        const auto &s = assets.sprite(id);
        const double sx = value_number(i.vars.at("image_xscale")),
                     sy = value_number(i.vars.at("image_yscale"));
        const int left = full_sprite ? 0 : s.left, right = full_sprite ? s.width : s.right + 1,
                  top = full_sprite ? 0 : s.top, bottom = full_sprite ? s.height : s.bottom + 1;
        const double a = value_number(i.vars.at("image_angle")) * 3.141592653589793 / 180;
        if (a == 0) {
            const double x1 = x + (left - s.ox) * sx, x2 = x + (right - s.ox) * sx,
                         y1 = y + (top - s.oy) * sy, y2 = y + (bottom - s.oy) * sy;
            return {std::min(x1, x2), std::max(x1, x2), std::min(y1, y2), std::max(y1, y2), true};
        }
        const double c = std::cos(a), sn = std::sin(a);
        Bounds out{INFINITY, -INFINITY, INFINITY, -INFINITY, true};
        for (double px : {double(left - s.ox), double(right - s.ox)})
            for (double py : {double(top - s.oy), double(bottom - s.oy)}) {
                const double tx = x + px * sx * c + py * sy * sn, ty = y - px * sx * sn + py * sy * c;
                out.left = std::min(out.left, tx);
                out.right = std::max(out.right, tx);
                out.top = std::min(out.top, ty);
                out.bottom = std::max(out.bottom, ty);
            }
        return out;
    }
    Bounds bounds(const Instance &i) const {
        return bounds(i, value_number(i.vars.at("x")), value_number(i.vars.at("y")));
    }
    int collision_sprite(const Instance &i) const {
        const int mask = number(i.vars.at("mask_index"));
        return mask >= 0 ? mask : number(i.vars.at("sprite_index"));
    }
    struct CollisionMask {
        const GameAssets::Sprite *sprite = nullptr;
        std::span<const std::uint8_t> pixels;
        std::size_t stride = 0;
        double x = 0, y = 0, sx = 1, sy = 1, c = 1, sn = 0;
    };
    CollisionMask collision_mask(const Instance &i, double x, double y) const {
        CollisionMask mask;
        const int id = collision_sprite(i);
        if (id < 0)
            return mask;
        const auto &sprite = assets.sprite(id);
        const double sx = value_number(i.vars.at("image_xscale")),
                     sy = value_number(i.vars.at("image_yscale"));
        if (sx == 0 || sy == 0)
            return mask;
        const double angle = value_number(i.vars.at("image_angle")) * 3.141592653589793 / 180;
        mask.sprite = &sprite;
        mask.x = x;
        mask.y = y;
        mask.sx = sx;
        mask.sy = sy;
        mask.c = angle == 0 ? 1 : std::cos(angle);
        mask.sn = angle == 0 ? 0 : std::sin(angle);
        if (sprite.precise && sprite.mask_count != 0) {
            const double frame = value_number(i.vars.at("image_index"));
            const auto count = static_cast<double>(sprite.mask_count);
            const auto index =
                static_cast<std::size_t>(std::fmod(std::fmod(std::floor(frame), count) + count, count));
            mask.pixels = assets.mask_frame(id, index);
            mask.stride = (static_cast<std::size_t>(sprite.width) + 7) / 8;
        }
        return mask;
    }
    static bool mask_contains(const CollisionMask &mask, double world_x, double world_y) {
        if (!mask.sprite)
            return false;
        const auto &sprite = *mask.sprite;
        const double dx = world_x - mask.x, dy = world_y - mask.y;
        const int px = static_cast<int>(std::floor((dx * mask.c - dy * mask.sn) / mask.sx + sprite.ox));
        const int py = static_cast<int>(std::floor((dx * mask.sn + dy * mask.c) / mask.sy + sprite.oy));
        if (px < sprite.left || px > sprite.right || py < sprite.top || py > sprite.bottom || px < 0 ||
            py < 0 || px >= sprite.width || py >= sprite.height)
            return false;
        if (mask.pixels.empty())
            return true;
        const auto byte =
            mask.pixels[static_cast<std::size_t>(py) * mask.stride + static_cast<std::size_t>(px) / 8];
        return (byte & (0x80U >> (px & 7))) != 0;
    }
    bool collides(const Instance &a, double x, double y, const Instance &b) const {
        const auto first = bounds(a, x, y), second = bounds(b);
        if (!first.valid || !second.valid || first.left >= second.right || first.right <= second.left ||
            first.top >= second.bottom || first.bottom <= second.top)
            return false;
        if (!assets.sprite(collision_sprite(a)).precise && !assets.sprite(collision_sprite(b)).precise)
            return true;
        const double bx = value_number(b.vars.at("x")), by = value_number(b.vars.at("y"));
        const auto first_mask = collision_mask(a, x, y), second_mask = collision_mask(b, bx, by);
        const int left = static_cast<int>(std::floor(std::max(first.left, second.left))),
                  right = static_cast<int>(std::ceil(std::min(first.right, second.right))),
                  top = static_cast<int>(std::floor(std::max(first.top, second.top))),
                  bottom = static_cast<int>(std::ceil(std::min(first.bottom, second.bottom)));
        for (int py = top; py < bottom; ++py)
            for (int px = left; px < right; ++px)
                if (mask_contains(first_mask, px + 0.5, py + 0.5) &&
                    mask_contains(second_mask, px + 0.5, py + 0.5))
                    return true;
        return false;
    }
    bool matches(const Instance &i, int object) const {
        if (object == -3 || object == i.id)
            return true;
        int n = i.object;
        for (std::size_t guard = 0;
             n >= 0 && static_cast<std::size_t>(n) < objects.size() && guard < objects.size(); ++guard) {
            if (n == object)
                return true;
            n = objects[static_cast<std::size_t>(n)].parent;
        }
        return false;
    }
    std::vector<InstancePtr> &targets(int target, const InstancePtr &self, const InstancePtr &other) {
        auto &out = depth == 0 ? target_fallback : vm_scratch[depth - 1].targets;
        out.clear();
        if (target == -1) {
            if (self)
                out.push_back(self);
            return out;
        }
        if (target == -2) {
            if (other && other->alive)
                out.push_back(other);
            return out;
        }
        if (out.capacity() < std::min(instances.size(), std::size_t{64}))
            out.reserve(std::min(instances.size(), std::size_t{64}));
        for (auto &i : instances)
            if (i->alive && !i->deactivated && matches(*i, target))
                out.push_back(i);
        return out;
    }
    Instance *first_target(int target, const InstancePtr &self, const InstancePtr &other) {
        if (target == -1)
            return self.get();
        if (target == -2)
            return other && other->alive ? other.get() : nullptr;
        for (const auto &i : instances)
            if (i->alive && !i->deactivated && matches(*i, target))
                return i.get();
        return nullptr;
    }
    CollisionRoute collision_event(int source, int target) {
        const auto key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(source)) << 32) |
                         static_cast<std::uint32_t>(target);
        if (const auto found = collision_routes.find(key); found != collision_routes.end())
            return found->second;
        const auto remember = [&](CollisionRoute route) {
            if (collision_routes.size() < 65536)
                collision_routes.emplace(key, route);
            return route;
        };
        for (std::size_t guard = 0;
             source >= 0 && static_cast<std::size_t>(source) < objects.size() && guard < objects.size();
             ++guard) {
            int other = target;
            for (std::size_t tg = 0;
                 other >= 0 && static_cast<std::size_t>(other) < objects.size() && tg < objects.size();
                 ++tg) {
                if (find_event(objects[static_cast<std::size_t>(source)], {4, other}))
                    return remember({source, other});
                other = objects[static_cast<std::size_t>(other)].parent;
            }
            source = objects[static_cast<std::size_t>(source)].parent;
        }
        return remember({});
    }
    void event(InstancePtr inst, int type, int subtype, int object = -1, InstancePtr other = {}) {
        const bool profile_draw_event = profile_tick_timing && type == 8;
        const auto draw_event_start = profile_draw_event ? std::chrono::steady_clock::now()
                                                         : std::chrono::steady_clock::time_point{};
        struct DrawEventProfileGuard {
            double &total;
            std::size_t &depth;
            bool enabled;
            std::chrono::steady_clock::time_point start;
            DrawEventProfileGuard(double &total_value,
                                  std::size_t &depth_value,
                                  bool is_enabled,
                                  std::chrono::steady_clock::time_point start_value)
                : total(total_value), depth(depth_value), enabled(is_enabled), start(start_value) {
                if (enabled)
                    ++depth;
            }
            ~DrawEventProfileGuard() {
                if (enabled) {
                    --depth;
                    total += std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
                }
            }
        } draw_event_profile_guard{last_draw_event_ms, draw_event_depth, profile_draw_event, draw_event_start};
        if (!inst->alive || inst->deactivated)
            return;
        const int start = object < 0 ? inst->object : object;
        if (start < 0 || static_cast<std::size_t>(start) >= objects.size())
            return;
        const auto &definition = objects[static_cast<std::size_t>(start)];
        int owner = -1;
        if (type == 3 && subtype >= 0 && subtype < 3)
            owner = definition.step_owners[static_cast<std::size_t>(subtype)];
        else if (type == 8 && subtype == 0)
            owner = definition.draw_owner;
        else
            owner = find_event_owner(definition, {type, subtype});
        if (owner < 0)
            return;
        const auto *event_code = find_event(objects[static_cast<std::size_t>(owner)], {type, subtype});
        if (!event_code)
            throw Error(ErrorKind::vm, "event owner has no matching event code");
        event_stack.push_back({type, subtype, owner});
        struct Guard {
            std::vector<EventFrame> &frames;
            ~Guard() { frames.pop_back(); }
        } guard{event_stack};
        for (const auto entry : event_code->code)
            execute(entry, inst, {}, other);
    }
    void prefetch_room_graphics() {
#if REGML_PS2
        if (current_room < 0 || static_cast<std::size_t>(current_room) >= rooms.size())
            return;
        const auto &room = rooms[static_cast<std::size_t>(current_room)];
        struct PrefetchKey {
            int sprite;
            std::uint64_t frame;
            bool operator==(const PrefetchKey &) const = default;
        };
        struct PrefetchHash {
            std::size_t operator()(const PrefetchKey &key) const noexcept {
                const auto sprite = std::hash<int>{}(key.sprite);
                const auto frame = std::hash<std::uint64_t>{}(key.frame);
                return sprite ^ (frame + 0x9e3779b9U + (sprite << 6) + (sprite >> 2));
            }
        };
        std::unordered_set<PrefetchKey, PrefetchHash> prefetched;
        prefetched.reserve(instances.size() + room.layers.size());
        auto prefetch_sprite = [&](int sprite, double frame, double speed) {
            if (!assets.sprite_exists(sprite))
                return;
            const auto prefetch_frame = [&](double value) {
                if (!std::isfinite(value))
                    return;
                const PrefetchKey key{sprite, std::bit_cast<std::uint64_t>(value)};
                if (prefetched.insert(key).second)
                    assets.prefetch(sprite, value);
            };
            prefetch_frame(frame);
            if (speed != 0)
                prefetch_frame(frame + speed);
        };
        for (const auto &i : instances) {
            if (!i->alive || number(i->vars.at("visible")) == 0)
                continue;
            prefetch_sprite(number(i->vars.at("sprite_index")), value_number(i->vars.at("image_index")),
                            value_number(i->vars.at("image_speed")));
        }
        for (const auto &layer : room.layers) {
            if (!layer.visible)
                continue;
            for (const auto &sprite : layer.sprites)
                prefetch_sprite(sprite.sprite, sprite.frame, sprite.speed);
            for (const auto &background : layer.backgrounds)
                if (background.visible)
                    prefetch_sprite(background.sprite, background.frame, background.speed);
        }
#endif
    }
    void enter(int id) {
        if (id < 0 || static_cast<std::size_t>(id) >= rooms.size())
            throw Error(ErrorKind::vm, "room ID out of range");
        if (depth == 0)
            linked.release_programs();
        tick_instances.assign(instances.begin(), instances.end());
        const auto &previous = tick_instances;
        for (auto &i : previous)
            if (i->alive)
                event(i, 7, 5);
        if (current_room >= 0) {
            if (number(engine.at("room_persistent")) != 0) {
                SavedRoom saved;
                for (const auto &i : instances)
                    if (i->alive && number(i->vars.at("persistent")) == 0)
                        saved.instances.push_back(i);
                for (const auto &[key, value] : engine)
                    if (key.starts_with("view_") || key == "room_width" || key == "room_height" ||
                        key == "room_speed" || key == "room_persistent")
                        saved.engine[key] = value;
                saved_rooms[current_room] = std::move(saved);
            } else
                saved_rooms.erase(current_room);
        }
        std::erase_if(instances,
                      [](const auto &i) { return !i->alive || number(i->vars.at("persistent")) == 0; });
#if REGML_LOW_MEMORY
        // only the current room needs its placements, views, and tiles loaded
        if (current_room >= 0) {
            auto &previous_room = rooms[static_cast<std::size_t>(current_room)];
            std::vector<Placement>{}.swap(previous_room.instances);
            VariableStore{}.swap(previous_room.views);
            std::vector<Background>{}.swap(previous_room.backgrounds);
            std::vector<Tile>{}.swap(previous_room.tiles);
            previous_room.payload_loaded = false;
        }
        load_room_payload(rooms[static_cast<std::size_t>(id)]);
        std::unordered_map<std::string, std::vector<std::uint8_t>>{}.swap(load_chunks);
#endif
        current_room = id;
        pending_room = -1;
        const auto &room = rooms[static_cast<std::size_t>(id)];
        engine["room"] = std::int32_t{id};
        engine["room_width"] = std::int32_t{room.width};
        engine["room_height"] = std::int32_t{room.height};
        engine["room_speed"] = room.speed;
        engine["room_persistent"] = std::int32_t{room.persistent};
        for (const auto &[key, value] : room.views)
            engine[key] = value;
        for (int view = 0; view < 8; ++view) {
            const auto suffix = "[" + std::to_string(view) + "]";
            engine["view_camera" + suffix] = std::int32_t{view};
            auto &camera = cameras[view];
            auto view_number = [&](std::string_view field, double fallback = 0.0) {
                const auto found = engine.find(std::string(field) + suffix);
                return found == engine.end() ? fallback : value_number(found->second);
            };
            camera.x = view_number("view_xview");
            camera.y = view_number("view_yview");
            camera.width = view_number("view_wview");
            camera.height = view_number("view_hview");
            camera.angle = 0;
            camera.border_x = view_number("view_hborder");
            camera.border_y = view_number("view_vborder");
            camera.speed_x = view_number("view_hspeed");
            camera.speed_y = view_number("view_vspeed");
            camera.target = static_cast<int>(view_number("view_object", -1));
            if (!engine.contains("view_surface_id" + suffix))
                engine["view_surface_id" + suffix] = std::int32_t{-1};
        }
        event_budget = 0;
        placed_instances.clear();
        if (placed_instances.capacity() < room.instances.size())
            placed_instances.reserve(room.instances.size());
        if (instances.size() < 10000)
            instances.reserve(std::min<std::size_t>(10000, instances.size() + room.instances.size()));
        const auto saved = saved_rooms.find(id);
        if (saved != saved_rooms.end()) {
            instances.insert(instances.end(), saved->second.instances.begin(), saved->second.instances.end());
            for (const auto &[key, value] : saved->second.engine)
                engine[key] = value;
            saved_rooms.erase(saved);
        } else {
            for (const auto &p : room.instances)
                placed_instances.push_back(make(p));
            for (std::size_t i = 0; i < placed_instances.size(); ++i) {
                const auto &p = room.instances[i];
                event(placed_instances[i], 14, 0);
                if (p.precreate >= 0)
                    execute(p.precreate, placed_instances[i], {});
                event(placed_instances[i], 0, 0);
                if (p.create >= 0)
                    execute(p.create, placed_instances[i], {});
            }
            if (room.create >= 0 && !placed_instances.empty())
                execute(room.create, placed_instances[0], {});
        }
        tick_instances.assign(instances.begin(), instances.end());
        const auto &snapshot = tick_instances;
        if (!game_started) {
            game_started = true;
            for (auto &i : snapshot)
                event(i, 7, 2);
        }
        for (auto &i : snapshot)
            event(i, 7, 4);
        graphics_prefetch_pending = true;
        std::cout << "Entered room " << id << ": " << room.name << " (" << instances.size() << " instances)"
                  << std::endl;
    }
    Value execute(int code,
                  InstancePtr self,
                  std::span<const Value> arguments,
                  InstancePtr other = {},
                  StructPtr struct_self = {}) {
        if (code < 0)
            return std::monostate{};
        current_code = code;
#if REGML_PS2
        const auto active_code_name = package.code_name(static_cast<std::size_t>(code));
        regml_ps2_active_code_name = active_code_name.data();
        regml_ps2_active_code_name_length = static_cast<std::uint32_t>(active_code_name.size());
#endif
#if REGML_WII
        if (frame == 0 && depth < 4)
            report_wii_memory(package.code_name(static_cast<std::size_t>(code)));
#endif
        if (depth >= 128)
            throw Error(ErrorKind::vm, "script call depth exceeded");
        ++depth;
        const auto program_index = gmrt_project
                                       ? std::numeric_limits<std::size_t>::max()
                                       : (linked.shared_code ? linked.code_to_program.at(code)
                                                             : static_cast<std::size_t>(code));
        active_program_stack[depth - 1] = program_index;
        struct Guard {
            std::size_t &depth;
            LinkedPackage &linked;
            std::array<std::size_t, 128> &active_programs;
            ~Guard() {
                --depth;
                if (depth == 0)
                    linked.release_programs();
                else if (linked.lazy_programs && linked.decoded_program_bytes > 1024 * 1024)
                    linked.release_programs(std::span<const std::size_t>(active_programs.data(), depth));
            }
        } guard{depth, linked, active_program_stack};
        if (gmrt_project) {
            auto &target_scratch = vm_scratch[depth - 1].targets;
            struct TargetScratchGuard {
                std::vector<InstancePtr> &targets;
                ~TargetScratchGuard() { targets.clear(); }
            } target_scratch_guard{target_scratch};
            if (static_cast<std::size_t>(code) >= gmrt_code_refs.size())
                throw Error(ErrorKind::vm, "GMIR code reference is out of range");
            const auto reference = gmrt_code_refs[static_cast<std::size_t>(code)];
            std::vector<StructPtr> handles;
            auto instance_value = [&](const InstancePtr &instance) -> Value {
                if (!instance)
                    return std::monostate{};
                auto handle = std::make_shared<StructValue>();
                gmrt_instance_values.insert_or_assign(handle.get(), instance);
                handles.push_back(std::move(handle));
                return handles.back();
            };
            const Value self_value = instance_value(self);
            const Value other_value = instance_value(other);
            struct HandleGuard {
                std::unordered_map<const StructValue *, InstancePtr> &map;
                std::vector<StructPtr> &handles;
                ~HandleGuard() {
                    for (const auto &handle : handles)
                        map.erase(handle.get());
                }
            } handle_guard{gmrt_instance_values, handles};
            if (!gmrt_global_value)
                gmrt_global_value = std::make_shared<StructValue>();
            GmrtExecutionContext context;
            context.self = self_value;
            context.other = other_value;
            context.scope = self_value;
            context.globals = gmrt_global_value;
            context.arguments.assign(arguments.begin(), arguments.end());
            std::vector<std::string> recent_gmrt_instructions;
            GmrtVmOptions vm_options;
            if (std::getenv("REGML_GMRT_DEBUG"))
                vm_options.trace = [&](const GmrtFunction &function,
                                   const GmrtBlock &,
                                   const GmrtInstruction &instruction,
                                   std::span<const Value> registers) {
                std::string text = function.global_name.empty() ? function.symbol : function.global_name;
                text += " " + std::string(gmrt_opcode_name(instruction.opcode));
                for (const auto operand : instruction.arguments)
                    text += " " + std::to_string(operand);
                if (instruction.opcode == 19 && instruction.arguments.size() > 1 &&
                    instruction.arguments[1] < function.constants.size()) {
                    const auto &constant = function.constants[instruction.arguments[1]];
                    text += " const-kind=" + std::to_string(static_cast<unsigned>(constant.kind));
                    if (const auto *constant_text = std::get_if<std::string>(&constant.value))
                        text += ":" + *constant_text;
                    if (const auto *constant_number = std::get_if<std::int32_t>(&constant.value))
                        text += ":" + std::to_string(*constant_number);
                    if (const auto *constant_real = std::get_if<double>(&constant.value))
                        text += ":" + std::to_string(*constant_real);
                }
                if (instruction.opcode == 0 && instruction.arguments.size() >= 3 &&
                    instruction.arguments[1] < registers.size() && instruction.arguments[2] < registers.size()) {
                    const auto &left = registers[instruction.arguments[1]];
                    const auto &right = registers[instruction.arguments[2]];
                    text += " values=" + std::to_string(left.index()) + "," +
                            std::to_string(right.index());
                    if (value_is_string(left))
                        text += ":" + std::string(value_string_view(left));
                    if (value_is_string(right))
                        text += ":" + std::string(value_string_view(right));
                }
                recent_gmrt_instructions.push_back(std::move(text));
                if (recent_gmrt_instructions.size() > 8)
                    recent_gmrt_instructions.erase(recent_gmrt_instructions.begin());
                };
            auto instance_for_value = [&](const Value &value) -> InstancePtr {
                const auto *handle = std::get_if<StructPtr>(&value);
                if (!handle || !*handle)
                    return {};
                const auto found = gmrt_instance_values.find(handle->get());
                return found == gmrt_instance_values.end() ? InstancePtr{} : found->second;
            };
            auto struct_for_value = [&](const Value &value) -> StructPtr {
                const auto *structure = std::get_if<StructPtr>(&value);
                if (!structure || !*structure || structure->get() == gmrt_global_value.get() ||
                    gmrt_instance_values.contains(structure->get()))
                    return {};
                return *structure;
            };
            auto resolve_gmrt_global = [&](std::string_view name) -> Value {
                if (const int id = gmrt_project->asset_index(name); id >= 0)
                    return static_cast<std::int32_t>(id);
                if (name == "noone")
                    return std::int32_t{-4};
                if (name == "all")
                    return std::int32_t{-3};
                if (name == "gamespeed_fps")
                    return std::int32_t{0};
                if (name == "os_android")
                    return std::int32_t{1};
                if (name == "os_ios")
                    return std::int32_t{2};
                if (name == "os_gxgames")
                    return std::int32_t{3};
                if (name == "browser_not_a_browser")
                    return std::int32_t{0};
                if (name == "vk_enter")
                    return std::int32_t{13};
                if (name == "vk_space")
                    return std::int32_t{32};
                if (name == "vk_escape")
                    return std::int32_t{27};
                if (name == "vk_left")
                    return std::int32_t{37};
                if (name == "vk_up")
                    return std::int32_t{38};
                if (name == "vk_right")
                    return std::int32_t{39};
                if (name == "vk_down")
                    return std::int32_t{40};
                if (name == "mb_left")
                    return std::int32_t{1};
                if (name == "c_white")
                    return std::int32_t{0xffffff};
                if (name == "c_black")
                    return std::int32_t{0};
                if (name == "c_red")
                    return std::int32_t{0xff0000};
                if (name == "c_aqua")
                    return std::int32_t{0xffff00};
                if (name == "pi")
                    return 3.141592653589793;
                return std::monostate{};
            };
            context.resolve_global = [&](std::string_view name) -> Value {
                auto value = resolve_gmrt_global(name);
                return std::holds_alternative<std::monostate>(value) ? Value{std::string(name)} : value;
            };
            context.get_property = [&](const Value &object, std::string_view name) -> Value {
                if (const auto *function = std::get_if<std::string>(&object);
                    function && gmrt_functions.contains(*function)) {
                    const auto properties = gmrt_function_properties.find(*function);
                    if (properties == gmrt_function_properties.end())
                        return std::monostate{};
                    const auto property = properties->second->variables.find(name);
                    return property == properties->second->variables.end() ? Value(std::monostate{})
                                                                           : property->second;
                }
                if (const auto *structure = std::get_if<StructPtr>(&object); structure && *structure) {
                    if (structure->get() == gmrt_global_value.get()) {
                        if (const auto found = globals.find(name); found != globals.end())
                            return found->second;
                        return resolve_gmrt_global(name);
                    }
                    if (const auto found = gmrt_instance_values.find(structure->get());
                        found != gmrt_instance_values.end()) {
                        const auto variable = found->second->vars.find(name);
                        Value value = std::monostate{};
                        if (variable != found->second->vars.end())
                            value = variable->second;
                        else if (const auto builtin = engine.find(name); builtin != engine.end())
                            value = builtin->second;
                        else
                            value = resolve_gmrt_global(name);
                        return value;
                    }
                }
                if (const auto *structure = std::get_if<StructPtr>(&object); structure && *structure) {
                    const auto found = (*structure)->variables.find(name);
                    return found == (*structure)->variables.end() ? Value(std::monostate{}) : found->second;
                }
                return std::monostate{};
            };
            context.set_property = [&](const Value &object, std::string_view name, const Value &value) {
                if (const auto *function = std::get_if<std::string>(&object);
                    function && gmrt_functions.contains(*function)) {
                    auto &properties = gmrt_function_properties[*function];
                    if (!properties)
                        properties = std::make_shared<StructValue>();
                    properties->variables[name] = value;
                    return;
                }
                if (const auto *structure = std::get_if<StructPtr>(&object); structure && *structure) {
                    if (structure->get() == gmrt_global_value.get()) {
                        globals[name] = value;
                        gmrt_global_value->variables[name] = value;
                        return;
                    }
                    if (const auto found = gmrt_instance_values.find(structure->get());
                        found != gmrt_instance_values.end()) {
                        found->second->vars[name] = value;
                        return;
                    }
                    (*structure)->variables[name] = value;
                }
            };
            context.call = [&](std::string_view name,
                               std::uint16_t,
                               const Value &call_self,
                               const Value &call_other,
                               const Value &scope,
                               std::span<const Value> call_arguments) -> Value {
                try {
                    return call(name, call_arguments, instance_for_value(call_self),
                                instance_for_value(call_other), struct_for_value(scope));
                } catch (const Error &error) {
                    std::string message = "GMIR builtin " + std::string(name) + ": " + error.what() +
                                          " (argument types";
                    for (const auto &argument : call_arguments)
                        message += " " + std::to_string(argument.index());
                    message += ")";
                    message += " at";
                    for (const auto &instruction : recent_gmrt_instructions)
                        message += "\n  " + instruction;
                    throw Error(error.kind(), std::move(message), error.offset());
                } catch (const std::exception &error) {
                    throw Error(ErrorKind::vm,
                                "GMIR builtin " + std::string(name) + ": " + error.what());
                }
            };
            GmrtVmResult result;
            try {
                result = execute_gmrt(gmrt_modules, reference.module, reference.function,
                                      std::move(context), std::move(vm_options));
            } catch (const Error &error) {
                std::string message = "GMIR " + gmrt_modules.at(reference.module)
                                                   .functions.at(reference.function)
                                                   .global_name + ": " + error.what() + " at";
                for (const auto &instruction : recent_gmrt_instructions)
                    message += "\n  " + instruction;
                throw Error(error.kind(), std::move(message), error.offset());
            } catch (const std::exception &error) {
                std::string message = "GMIR " + gmrt_modules.at(reference.module)
                                                   .functions.at(reference.function)
                                                   .global_name + ": " + error.what() + " at";
                for (const auto &instruction : recent_gmrt_instructions)
                    message += "\n  " + instruction;
                throw Error(ErrorKind::vm, std::move(message));
            }
            instruction_count += result.instructions_executed;
            return result.returned;
        }
        const auto &program = linked.program(package, static_cast<std::size_t>(code));
        if (program.byte_length == 0)
            return std::monostate{};
        auto &scratch = vm_scratch[depth - 1];
        auto &locals = scratch.locals;
        locals.clear();
        if (locals.bucket_count() < 16)
            locals.reserve(16);
        struct ScratchGuard {
            VmScratch &scratch;
            ~ScratchGuard() {
                scratch.locals.clear();
                scratch.stack.clear();
                scratch.stack_types.clear();
                scratch.arguments.clear();
                scratch.targets.clear();
                for (std::size_t i = 0; i < scratch.environment_count; ++i) {
                    auto &environment = scratch.environments[i];
                    environment.choices.clear();
                    if (environment.choices.capacity() > 64)
                        std::vector<InstancePtr>{}.swap(environment.choices);
                    environment.saved.reset();
                    environment.saved_other.reset();
                }
                scratch.environment_count = 0;
            }
        } scratch_guard{scratch};
        if (!arguments.empty()) {
            locals.reserve(arguments.size() * 2);
            const auto &argument_names = argument_variable_names();
            for (std::size_t i = 0; i < arguments.size(); ++i) {
                if (i < argument_names.size()) {
                    const auto &names = argument_names[i];
                    locals.try_emplace(std::string_view(names[0]), arguments[i]);
                    locals.try_emplace(std::string_view(names[1]), arguments[i]);
                } else {
                    locals["argument" + std::to_string(i)] = arguments[i];
                    locals["argument[" + std::to_string(i) + "]"] = arguments[i];
                }
            }
        }
        auto &stack = scratch.stack;
        auto &stack_types = scratch.stack_types;
        if (stack.capacity() < 32)
            stack.reserve(32);
        if (stack_types.capacity() < 32)
            stack_types.reserve(32);
        if (++scratch.variable_cache_generation == 0) {
            scratch.variable_cache.fill({});
            scratch.variable_cache_generation = 1;
        }
        const auto variable_cache_generation = scratch.variable_cache_generation;
        std::size_t pc = package.code().at(static_cast<std::size_t>(code)).entry_offset;
        const auto entry_instruction = program.instruction_at(pc);
        if (!entry_instruction)
            throw Error(ErrorKind::vm, "code entry is not an instruction boundary", program.origin + pc);
        std::size_t instruction_index = *entry_instruction;
        auto &environments = scratch.environments;
        auto &environment_count = scratch.environment_count;
        DataType current_stack_type = DataType::variable;
        auto pop_entry = [&]() {
            if (stack.empty())
                throw Error(ErrorKind::vm, "native VM stack underflow", program.origin + pc);
            auto v = std::move(stack.back());
            stack.pop_back();
            const auto type = stack_types.back();
            stack_types.pop_back();
            return std::pair<Value, DataType>{std::move(v), type};
        };
        auto pop = [&]() {
            if (stack.empty())
                throw Error(ErrorKind::vm, "native VM stack underflow", program.origin + pc);
            auto value = std::move(stack.back());
            stack.pop_back();
            stack_types.pop_back();
            return value;
        };
        auto push = [&](Value v, std::optional<DataType> type = std::nullopt) {
            if (stack.size() >= 4096)
                throw Error(ErrorKind::vm, "native VM stack limit", program.origin + pc);
            stack.push_back(std::move(v));
            stack_types.push_back(type.value_or(current_stack_type));
        };
        std::size_t executed_in_code = 0;
        struct InstructionCountGuard {
            std::size_t &total;
            std::size_t &executed;
            ~InstructionCountGuard() { total += executed; }
        } instruction_count_guard{instruction_count, executed_in_code};
        std::size_t last_instruction_pc = pc;
        try {
            while (pc < program.byte_length) {
                last_instruction_pc = pc;
                if (++event_budget > 2'000'000)
                    throw Error(ErrorKind::vm, "event instruction budget exhausted", program.origin + pc);
                ++executed_in_code;
#if REGML_PS2
                if (frame == 0 && (instruction_count + executed_in_code) % 1000 == 0) {
                    std::size_t map_entries = 0;
                    for (const auto &[id, map] : maps) {
                        (void)id;
                        map_entries += map.size();
                    }
                    ps2_memory_trace(package.code_name(static_cast<std::size_t>(code)), pc, map_entries);
                }
#endif
                const auto &ins = program.instructions[instruction_index];
                const auto opcode = ins.opcode();
#if REGML_PS2
                regml_ps2_active_pc = static_cast<std::uint32_t>(pc);
                regml_ps2_active_opcode = static_cast<std::uint32_t>(opcode);
                regml_ps2_active_symbol = ins.symbol_id;
#endif
                current_stack_type = opcode == Opcode::call || opcode == Opcode::call_v ||
                                             opcode == Opcode::extended
                                          ? DataType::variable
                                          : ins.type1;
                std::size_t next = pc + ins.size;
                auto variable = [&](bool writing, Value value = std::monostate{}) -> Value {
                    const auto id = static_cast<std::size_t>(ins.symbol_id);
                    const auto kind = (ins.reference_value() >> 24) & 0xf8;
                    const int encoded_scope =
                        std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(ins.word));
                    if (ins.symbol_id != Instruction::no_symbol && kind == 0xa0) {
                        auto &cached = scratch.variable_cache[id % scratch.variable_cache.size()];
                        if (cached.generation == variable_cache_generation && cached.symbol == id &&
                            cached.value) {
                            if (encoded_scope == -7 && cached.store == &locals) {
                                if (writing) {
                                    *cached.value = value;
                                    return value;
                                }
                                return *cached.value;
                            }
                            if (!struct_self && self && (encoded_scope == -1 || encoded_scope == -6)) {
                                if (cached.store == &self->vars && cached.engine_size == engine.size()) {
                                    if (writing) {
                                        *cached.value = value;
                                        return value;
                                    }
                                    return *cached.value;
                                }
                                if (cached.store == &engine) {
                                    if (writing) {
                                        *cached.value = value;
                                        return value;
                                    }
                                    return *cached.value;
                                }
                            }
                        }
                    }
                    const auto &symbol = linked.variables[id];
                    std::string indexed_name;
                    std::string_view name = symbol.name;
                    int scope = encoded_scope;
                    int array_index = -1;
                    StructPtr explicit_struct;
                    const bool dynamic_local = name == "undefined" || name == "instance_count" ||
                                               name.starts_with("bbox_") || name.starts_with("argument[") ||
                                               name == "sprite_width" || name == "sprite_height" ||
                                               name == "sprite_xoffset" || name == "sprite_yoffset" ||
                                               name == "image_number";
                    if (kind == 0 || kind == 0x10 || kind == 0x90) {
                        array_index = number(pop());
                        if (array_index < 0 || array_index >= 32000 * 32000)
                            throw Error(ErrorKind::vm, "array index out of range");
                        scope = number(pop());
                        indexed_name = symbol.name + "[" + std::to_string(array_index) + "]";
                        name = indexed_name;
                    } else if (kind == 0x80)
                        scope = number(pop());
                    else if (kind != 0xa0)
                        throw Error(ErrorKind::unsupported,
                                    "unsupported variable reference kind " + std::to_string(kind));
                    if (scope == -9) {
                        const auto receiver = pop();
                        if (const auto *object = std::get_if<StructPtr>(&receiver))
                            explicit_struct = *object;
                        else if (const auto *callable = std::get_if<CallablePtr>(&receiver); callable && *callable) {
                            if (!(*callable)->properties)
                                (*callable)->properties = std::make_shared<StructValue>();
                            explicit_struct = (*callable)->properties;
                        } else {
                            scope = number(receiver);
                        }
                    }
                    if (!explicit_struct && !struct_self && !writing && name.starts_with("bbox_")) {
                        const auto *found = first_target(scope, self, other);
                        if (!found)
                            throw Error(ErrorKind::vm, "bounding box instance missing");
                        const auto b = bounds(*found);
                        if (name == "bbox_left")
                            return std::floor(b.left);
                        if (name == "bbox_right")
                            return std::ceil(b.right) - 1;
                        if (name == "bbox_top")
                            return std::floor(b.top);
                        if (name == "bbox_bottom")
                            return std::ceil(b.bottom) - 1;
                    }
                    if (!explicit_struct && !struct_self && !writing &&
                        (name == "sprite_width" || name == "sprite_height" || name == "sprite_xoffset" ||
                         name == "sprite_yoffset" || name == "image_number")) {
                        const auto *found = first_target(scope, self, other);
                        if (!found)
                            throw Error(ErrorKind::vm, "sprite variable instance missing");
                        const int id = number(found->vars.at("sprite_index"));
                        if (id < 0)
                            return 0.0;
                        const auto &spr = assets.sprite(id);
                        if (name == "sprite_width")
                            return static_cast<double>(spr.width) *
                                   std::abs(value_number(found->vars.at("image_xscale")));
                        if (name == "sprite_height")
                            return static_cast<double>(spr.height) *
                                   std::abs(value_number(found->vars.at("image_yscale")));
                        if (name == "sprite_xoffset")
                            return static_cast<double>(spr.ox);
                        if (name == "sprite_yoffset")
                            return static_cast<double>(spr.oy);
                        return static_cast<double>(spr.frames.size());
                    }
                    VariableStore *store = nullptr;
                    bool multi_target = false;
                    const bool struct_store = explicit_struct || (struct_self && scope == -1);
                    const bool static_store = scope == -16;
                    if (explicit_struct)
                        store = &explicit_struct->variables;
                    else if (struct_self && scope == -1)
                        store = &struct_self->variables;
                    else if (scope == -5)
                        store = &globals;
                    else if (static_store)
                        store = &static_variables[code];
                    else if (scope == -7 || name.starts_with("argument"))
                        store = &locals;
                    else if (engine.contains(name) || (kind == 0xa0 && opcode == Opcode::push_builtin &&
                                                       (!self || !self->vars.contains(name))))
                        store = &engine;
                    else if ((scope == -1 || scope == -6) && self)
                        store = &self->vars;
                    else if (scope == -2 && other && other->alive)
                        store = &other->vars;
                    else {
                        multi_target = writing;
                        if (!writing) {
                            auto *found = first_target(scope, self, other);
                            if (!found)
                                throw Error(ErrorKind::vm,
                                            "variable instance not found: " + std::string(name) + " scope=" +
                                                std::to_string(scope) + " word=" + std::to_string(ins.word) +
                                                " ref=" + std::to_string(ins.reference_value()));
                            store = &found->vars;
                        }
                    }
                    auto access = [&](VariableStore *store) -> Value {
                        if (!writing && name == "undefined")
                            return std::monostate{};
                        if (!writing && store == &locals && name == "argument_count" &&
                            !locals.contains(name))
                            return static_cast<std::int32_t>(arguments.size());
                        if (!writing && store == &locals && !locals.contains(name) &&
                            name.starts_with("argument") &&
                            name.size() > 8 &&
                            std::all_of(name.begin() + 8, name.end(), [](unsigned char ch) {
                                return std::isdigit(ch) != 0;
                            }))
                            return std::monostate{};
                        if (store == &locals && name.starts_with("argument[") && store->contains(name)) {
                            if (writing)
                                (*store)[name] = value;
                            return store->at(name);
                        }
                        if (array_index >= 0) {
                            auto base = store->find(symbol.name);
                            if (base == store->end()) {
                                if (const auto flat = store->find(name); flat != store->end()) {
                                    if (writing)
                                        flat->second = value;
                                    return writing ? value : flat->second;
                                }
                                if (!writing && kind != 0x90)
                                        return std::monostate{};
                                base = store->emplace(symbol.name, ArrayPtr(new ArrayValue)).first;
                            }
                            auto *array = std::get_if<ArrayPtr>(&base->second);
                            if ((writing || kind == 0x90) && (!array || !*array)) {
                                base->second = ArrayPtr(new ArrayValue);
                                array = std::get_if<ArrayPtr>(&base->second);
                            }
                            if (!array || !*array)
                                throw Error(ErrorKind::vm,
                                            "indexed variable is not an array: " + symbol.name);
                            const int row = array_index / 32000, col = array_index % 32000;
                            if (kind == 0x90) {
                                if (array->use_count() > 1)
                                    *array = ArrayPtr(new ArrayValue(**array));
                                auto *element = (*array)->elements.find(array_index);
                                if (!element || !std::holds_alternative<ArrayPtr>(*element) ||
                                    !std::get<ArrayPtr>(*element)) {
                                    (*array)->elements[array_index] = ArrayPtr(new ArrayValue);
                                    auto &length = (*array)->row_lengths[row];
                                    length = std::max<std::int32_t>(
                                        length, static_cast<std::int32_t>(col + 1));
                                }
                                return (*array)->elements.at(array_index);
                            }
                            if (writing) {
                                if (array->use_count() > 1)
                                    *array = ArrayPtr(new ArrayValue(**array));
                                if ((*array)->elements.size() >= 1'000'000)
                                    throw Error(ErrorKind::vm, "array element limit reached");
                                (*array)->elements[array_index] = value;
                                auto &length = (*array)->row_lengths[row];
                                length = std::max<std::int32_t>(
                                    length, static_cast<std::int32_t>(col + 1));
                                return value;
                            }
                            const auto length = (*array)->row_lengths.find(row);
                            if (length == (*array)->row_lengths.end() || col >= length->second)
                                return std::monostate{};
                            const auto *element = (*array)->elements.find(array_index);
                            return element ? *element : Value{std::int32_t{0}};
                        }
                        const bool cacheable =
                            !multi_target && !explicit_struct &&
                            (store != &engine || (name != "event_type" && name != "event_number"));
                        auto &cached = scratch.variable_cache[id % scratch.variable_cache.size()];
                        const bool cache_hit = cacheable && cached.generation == variable_cache_generation &&
                                               cached.symbol == id && cached.store == store && cached.value;
                        if (writing) {
                            Value &destination = cache_hit ? *cached.value : (*store)[name];
                            destination = value;
                            if (cacheable)
                                cached = {store, &destination, static_cast<std::uint32_t>(id),
                                          variable_cache_generation,
                                          static_cast<std::uint32_t>(engine.size())};
                            if (store == &engine && name == "room")
                                pending_room = number(value);
                            if (!struct_store && !static_store && store != &engine && store != &globals &&
                                store != &locals) {
                                if (name == "speed" || name == "direction") {
                                    const double rad = value_number(store->at("direction")) *
                                                       3.141592653589793 / 180,
                                                 speed = value_number(store->at("speed"));
                                    (*store)["hspeed"] = speed * std::cos(rad);
                                    (*store)["vspeed"] = -speed * std::sin(rad);
                                }
                                if (name == "hspeed" || name == "vspeed") {
                                    const double h = value_number(store->at("hspeed")),
                                                 v = value_number(store->at("vspeed"));
                                    (*store)["speed"] = std::hypot(h, v);
                                    if (h != 0 || v != 0)
                                        (*store)["direction"] =
                                            std::fmod(std::atan2(-v, h) * 180 / 3.141592653589793 + 360, 360);
                                }
                            }
                            return value;
                        }
                        if (name == "instance_count")
                            return static_cast<std::int32_t>(instances.size());
                        Value *found_value = cache_hit ? cached.value : nullptr;
                        if (!found_value) {
                            const auto it = store->find(name);
                            if (it != store->end()) {
                                found_value = &it->second;
                                if (cacheable)
                                    cached = {store, found_value, static_cast<std::uint32_t>(id),
                                              variable_cache_generation,
                                              static_cast<std::uint32_t>(engine.size())};
                            }
                        }
                        if (!found_value && (name.starts_with("u_") || name.starts_with("tex_")))
                            return std::int32_t{-1};
                        if (!found_value)
                            throw Error(ErrorKind::vm, "uninitialized variable: " + std::string(name));
                        return *found_value;
                    };
                    if (multi_target) {
                        bool found = false;
                        for (const auto &target : instances) {
                            if (target->alive && !target->deactivated && matches(*target, scope)) {
                                access(&target->vars);
                                found = true;
                            }
                        }
                        if (!found)
                            return value;
                        return value;
                    }
                    return access(store);
                };
                switch (opcode) {
                case Opcode::push_i:
                case Opcode::push:
                case Opcode::push_local:
                case Opcode::push_global:
                case Opcode::push_builtin:
                    if (ins.type1 == DataType::variable)
                        push(variable(false), ins.type1);
                    else if (ins.type1 == DataType::string)
                        push(PackageString{package.string_at(ins.reference_value())}, ins.type1);
                    else if (ins.immediate_kind == Instruction::ImmediateKind::int32 &&
                             ins.symbol_id != Instruction::no_symbol)
                        push(FunctionReference{ins.symbol_id}, DataType::variable);
                    else if (ins.immediate_kind == Instruction::ImmediateKind::int32)
                        push(ins.immediate_i32(), ins.type1);
                    else if (ins.immediate_kind == Instruction::ImmediateKind::real64)
                        push(ins.immediate_real(), ins.type1);
                    else if (ins.immediate_kind == Instruction::ImmediateKind::real32)
                        push(ins.immediate_real(), ins.type1);
                    else if (ins.immediate_kind == Instruction::ImmediateKind::int64)
                        push(static_cast<double>(ins.immediate_i64()), ins.type1);
                    else
                        throw Error(ErrorKind::unsupported, "unsupported native push");
                    break;
                case Opcode::pop: {
                    if (ins.type1 == DataType::int16)
                        throw Error(ErrorKind::unsupported, "native stack swap pending");
                    // store.v.* leaves the value on top of scope/index; store.*.v puts it underneath
                    if (ins.type1 != DataType::variable) {
                        auto v = pop();
                        variable(true, v);
                    } else {
                        const auto kind = (ins.reference_value() >> 24) & 0xf8;
                        std::array<std::pair<Value, DataType>, 3> address;
                        std::size_t address_count = 0;
                        if (kind == 0) {
                            address[1] = pop_entry();
                            address[0] = pop_entry();
                            address_count = 2;
                            if (number(address[0].first) == -9) {
                                address[2] = pop_entry();
                                address_count = 3;
                            }
                        } else if (kind == 0x80) {
                            address[0] = pop_entry();
                            address_count = 1;
                            if (number(address[0].first) == -9) {
                                address[1] = pop_entry();
                                address_count = 2;
                            }
                        }
                        auto v = pop();
                        if (kind == 0x80) {
                            if (address_count == 2)
                                push(std::move(address[1].first), address[1].second);
                            if (address_count > 0)
                                push(std::move(address[0].first), address[0].second);
                        } else if (kind == 0) {
                            if (address_count == 3)
                                push(std::move(address[2].first), address[2].second);
                            if (address_count > 1)
                                push(std::move(address[0].first), address[0].second);
                            if (address_count > 0)
                                push(std::move(address[1].first), address[1].second);
                        } else {
                            for (std::size_t i = 0; i < address_count; ++i)
                                push(std::move(address[i].first), address[i].second);
                        }
                        variable(true, v);
                    }
                    break;
                }
                case Opcode::conv: {
                    auto v = pop();
                    if (ins.type2() == DataType::int32)
                        push(static_cast<std::int32_t>(number(v)), ins.type2());
                    else if (ins.type2() == DataType::real)
                        push(value_number(v), ins.type2());
                    else if (ins.type2() == DataType::boolean)
                        push(!std::holds_alternative<std::monostate>(v) && value_number(v) > 0.5,
                             ins.type2());
                    else
                        push(v, ins.type2());
                    break;
                }
                case Opcode::dup: {
                    auto type = ins.type1;
                    auto duplicate_size = static_cast<std::size_t>(ins.extra());
                    auto swap_size = static_cast<std::size_t>((ins.comparison() & 0x7f) >> 3);
                    if (type == DataType::int16) {
                        type = DataType::variable;
                        std::swap(duplicate_size, swap_size);
                    }
                    const auto type_size = [](DataType value) -> std::size_t {
                        switch (value) {
                        case DataType::real:
                        case DataType::int64:
                            return 8;
                        case DataType::float32:
                        case DataType::int32:
                        case DataType::boolean:
                        case DataType::string:
                        case DataType::int16:
                            return 4;
                        case DataType::variable:
                            return 16;
                        }
                        return 0;
                    };
                    auto entries_for_size = [&](std::size_t end, std::size_t bytes) {
                        std::size_t count = 0;
                        while (bytes > 0 && count < end) {
                            const auto size = type_size(stack_types[end - count - 1]);
                            if (size == 0 || size > bytes)
                                throw Error(ErrorKind::vm, "DUP stack size mismatch", program.origin + pc);
                            bytes -= size;
                            ++count;
                        }
                        if (bytes != 0) {
                            throw Error(ErrorKind::vm, "DUP stack underflow", program.origin + pc);
                        }
                        return count;
                    };
                    if (swap_size != 0) {
                        const auto top_count =
                            entries_for_size(stack.size(), duplicate_size * type_size(type));
                        const auto bottom_count =
                            entries_for_size(stack.size() - top_count, swap_size * type_size(type));
                        const auto first = stack.size() - top_count - bottom_count;
                        std::rotate(stack.begin() + static_cast<std::ptrdiff_t>(first),
                                    stack.begin() + static_cast<std::ptrdiff_t>(first + bottom_count),
                                    stack.end());
                        std::rotate(stack_types.begin() + static_cast<std::ptrdiff_t>(first),
                                    stack_types.begin() + static_cast<std::ptrdiff_t>(first + bottom_count),
                                    stack_types.end());
                    } else {
                        const auto duplicate_count =
                            entries_for_size(stack.size(), (duplicate_size + 1) * type_size(type));
                        const auto first = stack.size() - duplicate_count;
                        const auto values = std::vector<Value>(
                            stack.begin() + static_cast<std::ptrdiff_t>(first), stack.end());
                        const auto types = std::vector<DataType>(
                            stack_types.begin() + static_cast<std::ptrdiff_t>(first), stack_types.end());
                        for (std::size_t i = 0; i < values.size(); ++i)
                            push(values[i], types[i]);
                    }
                    break;
                }
                case Opcode::popz:
                    pop();
                    break;
                case Opcode::add:
                case Opcode::sub:
                case Opcode::mul:
                case Opcode::div:
                case Opcode::rem:
                case Opcode::mod:
                case Opcode::cmp:
                case Opcode::bit_and:
                case Opcode::bit_or:
                case Opcode::bit_xor:
                case Opcode::shl:
                case Opcode::shr: {
                    const auto right = pop(), left = pop();
                    if (opcode == Opcode::cmp &&
                        (std::holds_alternative<std::monostate>(left) ||
                         std::holds_alternative<std::monostate>(right))) {
                        const bool equal = std::holds_alternative<std::monostate>(left) &&
                                           std::holds_alternative<std::monostate>(right);
                        if (ins.comparison() == 3)
                            push(equal);
                        else if (ins.comparison() == 4)
                            push(!equal);
                        else
                            push(false);
                        break;
                    }
                    if (value_is_string(left) && value_is_string(right)) {
                        const auto a = value_string_view(left);
                        const auto b = value_string_view(right);
                        if (opcode == Opcode::add) {
                            if (a.size() + b.size() > 1'048'576)
                                throw Error(ErrorKind::vm, "string limit");
                            std::string result;
                            result.reserve(a.size() + b.size());
                            result.append(a).append(b);
                            push(std::move(result));
                        } else if (opcode == Opcode::cmp) {
                            const int c = a.compare(b);
                            push(ins.comparison() == 1   ? c < 0
                                 : ins.comparison() == 2 ? c <= 0
                                 : ins.comparison() == 3 ? c == 0
                                 : ins.comparison() == 4 ? c != 0
                                 : ins.comparison() == 5 ? c >= 0
                                                         : c > 0);
                        } else
                            throw Error(ErrorKind::unsupported, "string arithmetic");
                        break;
                    }
                    if (opcode == Opcode::cmp &&
                        (ins.comparison() == 3 || ins.comparison() == 4)) {
                        const auto is_numeric = [](const Value &value) {
                            return std::holds_alternative<std::int32_t>(value) ||
                                   std::holds_alternative<double>(value) || std::holds_alternative<bool>(value);
                        };
                        bool equal = false;
                        if (is_numeric(left) && is_numeric(right)) {
                            equal = compare_numbers(value_number(left), value_number(right), 3,
                                                    comparison_epsilon);
                        } else if (const auto *array_left = std::get_if<ArrayPtr>(&left)) {
                            if (const auto *array_right = std::get_if<ArrayPtr>(&right))
                                equal = array_left->get() == array_right->get();
                        } else if (const auto *function_left = std::get_if<FunctionReference>(&left)) {
                            if (const auto *function_right = std::get_if<FunctionReference>(&right))
                                equal = function_left->symbol_id == function_right->symbol_id;
                        } else if (const auto *callable_left = std::get_if<CallablePtr>(&left)) {
                            if (const auto *callable_right = std::get_if<CallablePtr>(&right))
                                equal = callable_left->get() == callable_right->get();
                        } else if (const auto *struct_left = std::get_if<StructPtr>(&left)) {
                            if (const auto *struct_right = std::get_if<StructPtr>(&right))
                                equal = struct_left->get() == struct_right->get();
                        }
                        push(ins.comparison() == 3 ? equal : !equal);
                        break;
                    }
                    const double a = value_number(left), b = value_number(right);
                    switch (opcode) {
                    case Opcode::add:
                        push(a + b);
                        break;
                    case Opcode::sub:
                        push(a - b);
                        break;
                    case Opcode::mul:
                        push(a * b);
                        break;
                    case Opcode::div:
                        if (b == 0)
                            throw Error(ErrorKind::vm, "division by zero");
                        push(a / b);
                        break;
                    case Opcode::rem:
                        if (b == 0)
                            throw Error(ErrorKind::vm, "integer division by zero");
                        push(std::trunc(a / b));
                        break;
                    case Opcode::mod:
                        if (b == 0)
                            throw Error(ErrorKind::vm, "modulo by zero");
                        push(std::fmod(a, b));
                        break;
                    case Opcode::cmp:
                        push(compare_numbers(a, b, ins.comparison(), comparison_epsilon));
                        break;
                    case Opcode::bit_and:
                        push(static_cast<std::int32_t>(number(left) & number(right)));
                        break;
                    case Opcode::bit_or:
                        push(static_cast<std::int32_t>(number(left) | number(right)));
                        break;
                    case Opcode::bit_xor:
                        push(static_cast<std::int32_t>(number(left) ^ number(right)));
                        break;
                    case Opcode::shl:
                        push(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(number(left))
                                                         << (static_cast<unsigned>(number(right)) & 31)));
                        break;
                    case Opcode::shr:
                        push(static_cast<std::int32_t>(number(left) >>
                                                       (static_cast<unsigned>(number(right)) & 31)));
                        break;
                    default:
                        break;
                    }
                    break;
                }
                case Opcode::neg:
                    push(-value_number(pop()));
                    break;
                case Opcode::bit_not:
                    if (ins.type1 == DataType::boolean)
                        push(!(value_number(pop()) > 0.5));
                    else
                        push(static_cast<std::int32_t>(~number(pop())));
                    break;
                case Opcode::call: {
                    const auto &name = linked.functions.at(static_cast<std::size_t>(ins.symbol_id)).name;
                    const auto count = static_cast<std::size_t>(ins.word & 0xffff);
                    auto &args = scratch.arguments;
                    args.clear();
                    if (count > stack.size())
                        throw Error(ErrorKind::vm, "call argument stack underflow", program.origin + pc);
                    args.reserve(count);
                    for (std::size_t i = 0; i < count; ++i)
                        args.push_back(pop());
                    auto result = call(name, args, self, other, struct_self);
#if REGML_PS2
                    regml_ps2_active_code_name = active_code_name.data();
                    regml_ps2_active_code_name_length = static_cast<std::uint32_t>(active_code_name.size());
#endif
                    args.clear();
                    push(std::move(result));
                    break;
                }
                case Opcode::call_v: {
                    const auto count = static_cast<std::size_t>(ins.word & 0xff);
                    if (stack.size() < count + 2)
                        throw Error(ErrorKind::vm, "CALLV argument stack underflow", program.origin + pc);
                    const auto function = pop();
                    const auto receiver = pop();
                    auto &args = scratch.arguments;
                    args.clear();
                    args.reserve(count);
                    for (std::size_t i = 0; i < count; ++i)
                        args.push_back(pop());
                    Value result;
                    if (const auto *callable = std::get_if<CallablePtr>(&function)) {
                        if (!*callable || !(*callable)->invoke)
                            throw Error(ErrorKind::vm, "CALLV received an empty callable", program.origin + pc);
                        result = (*callable)->invoke(args);
                    } else if (const auto *reference = std::get_if<FunctionReference>(&function)) {
                        if (const auto *object = std::get_if<StructPtr>(&receiver))
                            result = invoke_function(reference->symbol_id, args, {}, other, *object);
                        else
                            result = invoke_function(reference->symbol_id, args,
                                                     instance_for_value(receiver, self, other), other);
                    } else {
                        throw Error(ErrorKind::unsupported, "CALLV requires a function value", program.origin + pc);
                    }
#if REGML_PS2
                    regml_ps2_active_code_name = active_code_name.data();
                    regml_ps2_active_code_name_length = static_cast<std::uint32_t>(active_code_name.size());
#endif
                    args.clear();
                    push(std::move(result));
                    break;
                }
                case Opcode::ret:
                    return pop();
                case Opcode::exit:
                    return std::monostate{};
                case Opcode::branch:
                    next = ins.branch_target_value();
                    break;
                case Opcode::branch_true:
                case Opcode::branch_false: {
                    const bool condition = value_number(pop()) > 0.5;
                    if (condition == (opcode == Opcode::branch_true))
                        next = ins.branch_target_value();
                    break;
                }
                case Opcode::push_env: {
                    const int scope = number(pop());
                    const int target = scope == -9 ? number(pop()) : scope;
                    if (environment_count == environments.size())
                        environments.emplace_back();
                    auto &environment = environments[environment_count];
                    environment.saved = self;
                    environment.saved_other = other;
                    environment.index = 0;
                    environment.choices.clear();
                    if (target == -1) {
                        if (self)
                            environment.choices.push_back(self);
                    } else if (target == -2) {
                        if (other && other->alive)
                            environment.choices.push_back(other);
                    } else {
                        for (const auto &instance : instances)
                            if (instance->alive && !instance->deactivated && matches(*instance, target))
                                environment.choices.push_back(instance);
                    }
                    if (environment.choices.empty()) {
                        environment.saved.reset();
                        environment.saved_other.reset();
                        next = ins.branch_target_value();
                        const auto found = program.instruction_at(next);
                        if (found) {
                            const auto &end = program.instructions.at(*found);
                            if (end.opcode() == Opcode::pop_env)
                                next += end.size;
                        }
                    } else {
                        ++environment_count;
                        other = self;
                        self = environment.choices[0];
                    }
                    break;
                }
                case Opcode::pop_env: {
                    if (environment_count == 0)
                        throw Error(ErrorKind::vm, "environment underflow");
                    auto &e = environments[environment_count - 1];
                    if (!ins.environment_exit() && ++e.index < e.choices.size()) {
                        self = e.choices[e.index];
                        next = ins.branch_target_value();
                    } else {
                        self = e.saved;
                        other = e.saved_other;
                        --environment_count;
                        e.choices.clear();
                        if (e.choices.capacity() > 64)
                            std::vector<InstancePtr>{}.swap(e.choices);
                        e.saved.reset();
                        e.saved_other.reset();
                    }
                    break;
                }
                case Opcode::extended: {
                    const auto sub = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(ins.word));
                    if (sub == -11) {
                        if (ins.immediate_kind != Instruction::ImmediateKind::int32)
                            throw Error(ErrorKind::malformed, "pushref has no 32-bit reference");
                        if (ins.symbol_id != Instruction::no_symbol)
                            push(FunctionReference{ins.symbol_id});
                        else
                            push(ins.immediate_i32());
                    } else if (sub == -1) {
                        if (stack.empty() || number(stack.back()) < 0 || number(stack.back()) >= 32000)
                            throw Error(ErrorKind::vm, "legacy two-dimensional array index out of range");
                    } else if (sub == -2 || sub == -4) {
                        const auto index = number(pop());
                        const auto array_value = pop();
                        const auto *array = std::get_if<ArrayPtr>(&array_value);
                        if (index < 0 || index >= 32000 * 32000)
                            throw Error(ErrorKind::vm, "invalid multi-dimensional array access");
                        if (!array || !*array) {
                            if (sub == -2) {
                                push(std::monostate{});
                                break;
                            }
                            throw Error(ErrorKind::vm, "invalid multi-dimensional array access");
                        }
                        auto *found = (*array)->elements.find(index);
                        if (sub == -2) {
                            push(found ? *found : Value{std::monostate{}});
                        } else {
                            if (!found || !std::holds_alternative<ArrayPtr>(*found) ||
                                !std::get<ArrayPtr>(*found)) {
                                (*array)->elements[index] = ArrayPtr(new ArrayValue);
                                auto &length = (*array)->row_lengths[index / 32000];
                                length = std::max<std::int32_t>(
                                    length, static_cast<std::int32_t>(index % 32000 + 1));
                            }
                            push((*array)->elements.at(index));
                        }
                    } else if (sub == -3) {
                        const auto index = number(pop());
                        const auto array_value = pop();
                        const auto value = pop();
                        const auto *array = std::get_if<ArrayPtr>(&array_value);
                        if (!array || !*array || index < 0 || index >= 32000 * 32000)
                            throw Error(ErrorKind::vm, "invalid multi-dimensional array assignment");
                        (*array)->elements[index] = value;
                        auto &length = (*array)->row_lengths[index / 32000];
                        length = std::max<std::int32_t>(
                            length, static_cast<std::int32_t>(index % 32000 + 1));
                    } else if (sub == -5) {
                        (void)pop();
                    } else
                        throw Error(ErrorKind::unsupported,
                                    "native extended opcode pending: " + std::to_string(sub));
                    break;
                }
                default:
                    throw Error(ErrorKind::unsupported,
                                "native opcode pending: " + std::string(opcode_name(opcode)));
                }
                if (next != pc + ins.size && next != program.byte_length) {
                    const auto target = program.instruction_at(next);
                    if (!target)
                        throw Error(ErrorKind::vm, "program counter is not an instruction boundary",
                                    program.origin + next);
                    instruction_index = *target;
                } else {
                    ++instruction_index;
                }
                pc = next;
            }
            current_code = code;
            current_vm_address = program.origin + last_instruction_pc;
        } catch (const Error &e) {
            if (!e.offset() || (*e.offset() >= program.origin &&
                                *e.offset() < program.origin + program.byte_length))
                current_code = code;
            current_vm_address = e.offset().value_or(program.origin + pc);
#if REGML_PS2
            ps2_error_trace(package.code_name(static_cast<std::size_t>(code)), pc, e.what());
#endif
            throw Error(e.kind(),
                        std::string(e.what()) + " in " +
                            std::string(package.code_name(static_cast<std::size_t>(code))),
                        e.offset().value_or(program.origin + pc));
        } catch (const std::bad_alloc &) {
            current_code = code;
            current_vm_address = program.origin + pc;
#if REGML_PS2
            std::size_t map_entries = 0;
            for (const auto &[id, map] : maps) {
                (void)id;
                map_entries += map.size();
            }
            ps2_memory_trace("bad_alloc", pc, map_entries);
#endif
            throw Error(ErrorKind::io,
                        "out of memory in " + std::string(package.code_name(static_cast<std::size_t>(code))),
                        program.origin + pc);
        } catch (const std::out_of_range &e) {
            current_code = code;
            current_vm_address = program.origin + pc;
            throw Error(ErrorKind::vm,
                        std::string("host lookup failed: ") + e.what() + " in " +
                            std::string(package.code_name(static_cast<std::size_t>(code))),
                        program.origin + pc);
        }
        return std::monostate{};
    }
    bool prepare_save_directory() const {
        std::error_code error;
        if (std::filesystem::is_directory(save_dir, error))
            return true;
        if (error)
            return false;
        error.clear();
        std::filesystem::create_directories(save_dir, error);
        if (error)
            return false;
        return std::filesystem::is_directory(save_dir, error) && !error;
    }
    InstancePtr instance_for_value(const Value &value, InstancePtr self, InstancePtr other) {
        const auto id = static_cast<int>(value_number(value));
        if (id == -1)
            return self;
        if (id == -2)
            return other;
        for (const auto &instance : instances)
            if (instance->alive && instance->id == id)
                return instance;
        return {};
    }
    Value invoke_function(std::uint32_t symbol_id,
                          std::span<const Value> args,
                          InstancePtr self,
                          InstancePtr other,
                          StructPtr struct_self = {}) {
        if (symbol_id >= linked.functions.size())
            throw Error(ErrorKind::vm, "function reference is out of range");
        const auto &name = linked.functions[symbol_id].name;
        if (const auto found = code_ids.find(name); found != code_ids.end())
            return execute(found->second, self, args, other, std::move(struct_self));
        return call(name, args, self, other, std::move(struct_self));
    }
    Value call(std::string_view name,
               std::span<const Value> args,
               InstancePtr self,
               InstancePtr other,
               StructPtr struct_self = {}) {
        int draw_builtin_category = -1;
        if (profile_tick_timing && draw_event_depth != 0) {
            if (name.starts_with("draw_text"))
                draw_builtin_category = 0;
            else if (name.starts_with("draw_sprite") || name.starts_with("draw_background") ||
                     name.starts_with("draw_surface") || name.starts_with("draw_tile") ||
                     name == "draw_self")
                draw_builtin_category = 1;
            else if (name.starts_with("draw_"))
                draw_builtin_category = 2;
            else if (name.starts_with("surface_"))
                draw_builtin_category = 3;
        }
        const bool profile_draw_builtin = draw_builtin_category >= 0;
        const auto draw_builtin_start = profile_draw_builtin ? std::chrono::steady_clock::now()
                                                             : std::chrono::steady_clock::time_point{};
        struct DrawBuiltinProfileGuard {
            double &total;
            std::array<double, 4> &categories;
            double &maximum;
            std::string_view &slowest_name;
            int category;
            bool enabled;
            std::chrono::steady_clock::time_point start;
            std::string_view name;
            ~DrawBuiltinProfileGuard() {
                if (enabled) {
                    const double elapsed = std::chrono::duration<double, std::milli>(
                                               std::chrono::steady_clock::now() - start)
                                               .count();
                    total += elapsed;
                    categories[static_cast<std::size_t>(category)] += elapsed;
                    if (elapsed > maximum) {
                        maximum = elapsed;
                        slowest_name = name;
                    }
                }
            }
        } draw_builtin_profile_guard{last_draw_builtin_ms, last_draw_builtin_categories,
                                     max_draw_builtin_call_ms, slowest_draw_builtin, draw_builtin_category,
                                     profile_draw_builtin, draw_builtin_start, name};
        auto n = [&](std::size_t i) {
            if (i >= args.size())
                throw Error(ErrorKind::vm, "missing built-in argument");
            if (!std::holds_alternative<std::int32_t>(args[i]) &&
                !std::holds_alternative<double>(args[i]) && !std::holds_alternative<bool>(args[i]))
                throw Error(ErrorKind::vm,
                            "numeric argument " + std::to_string(i) + " to " + std::string(name) +
                                " requires a scalar number (got " + value_text(args[i]) + ")");
            return value_number(args[i]);
        };
        auto text = [&](std::size_t i) {
            if (i >= args.size())
                throw Error(ErrorKind::vm, "missing built-in argument");
            return value_text(args[i]);
        };
        auto text_view = [&](std::size_t i, std::string &storage) -> std::string_view {
            if (i >= args.size())
                throw Error(ErrorKind::vm, "missing built-in argument");
            if (value_is_string(args[i]))
                return value_string_view(args[i]);
            storage = value_text(args[i]);
            return storage;
        };
        if (name == "game_change") {
            std::string request;
            std::vector<std::string> values;
            values.reserve(args.size());
            for (const auto &arg : args) {
                values.push_back(value_text(arg));
                if (!request.empty())
                    request += " | ";
                request += values.back();
            }

            auto base = game_dir.empty() ? std::filesystem::current_path() : game_dir;
            const auto folder_name = base.filename().string();
            if (folder_name.starts_with("chapter") && !base.parent_path().empty())
                base = base.parent_path();
            std::vector<std::filesystem::path> candidates;
            auto add_candidate = [&](std::filesystem::path path) {
                if (path.empty())
                    return;
                if (path.is_relative()) {
                    if (path.has_root_directory() && !path.has_root_name())
                        path = base / path.relative_path();
                    else
                        path = base / path;
                } else {
                    const auto normalized_base = base.lexically_normal().generic_string();
                    const auto normalized_path = path.lexically_normal().generic_string();
                    if (!normalized_path.starts_with(normalized_base) ||
                        (path.has_root_directory() && !path.has_root_name()))
                        path = base / path.relative_path();
                }
                path = path.lexically_normal();
                if (path != package_path.lexically_normal() &&
                    std::find(candidates.begin(), candidates.end(), path) == candidates.end())
                    candidates.push_back(std::move(path));
            };
            auto clean_path = [](std::string path) {
                const auto first = path.find_first_not_of(" \t\r\n\"'");
                if (first == std::string::npos)
                    return std::string{};
                path.erase(0, first);
                const auto last = path.find_last_not_of(" \t\r\n\"'");
                path.resize(last + 1);
                if (path.starts_with("rom:"))
                    path.erase(0, 4);
                if (path.starts_with("-game")) {
                    path.erase(0, 5);
                    const auto start = path.find_first_not_of(" \t");
                    if (start == std::string::npos)
                        return std::string{};
                    path.erase(0, start);
                }
                while (!path.empty() && (path.back() == '"' || path.back() == '\'' ||
                                         path.back() == ')' || path.back() == ','))
                    path.pop_back();
                return path;
            };

            std::optional<std::filesystem::path> working_directory;
            std::optional<std::filesystem::path> launch_package;
            std::optional<unsigned> chapter;
            for (const auto &value : values) {
                std::string path_text = value;
                if (const auto game = path_text.find("-game"); game != std::string::npos) {
                    path_text = path_text.substr(game);
                    path_text = clean_path(std::move(path_text));
                    if (!path_text.empty()) {
                        const auto end = path_text.find_first_of(" \t\r\n");
                        if (end != std::string::npos)
                            path_text.resize(end);
                        launch_package = std::filesystem::path(path_text);
                        add_candidate(*launch_package);
                        const auto marker = path_text.find("chapter");
                        if (marker != std::string::npos) {
                            auto chapter_path = clean_path(path_text.substr(marker));
                            const auto digit = chapter_path.find_first_of("0123456789");
                            if (digit != std::string::npos && !chapter)
                                chapter = static_cast<unsigned>(chapter_path[digit] - '0');
                            const std::filesystem::path requested_path(chapter_path);
                            if (requested_path.has_extension()) {
                                add_candidate(requested_path);
                                working_directory = requested_path.parent_path();
                            } else {
                                add_candidate(requested_path / "data.win");
                                working_directory = requested_path;
                            }
                        }
                    }
                } else {
                    const auto marker = path_text.find("chapter");
                    if (marker != std::string::npos) {
                        auto end = path_text.find_first_of(" \t\r\n\"'", marker);
                        if (end == std::string::npos)
                            end = path_text.size();
                        auto chapter_path = clean_path(path_text.substr(marker, end - marker));
                        const auto digit = chapter_path.find_first_of("0123456789");
                        if (digit != std::string::npos && !chapter)
                            chapter = static_cast<unsigned>(chapter_path[digit] - '0');
                        if (!chapter_path.empty()) {
                            std::filesystem::path requested_path(chapter_path);
                            if (requested_path.has_extension())
                                add_candidate(requested_path);
                            add_candidate(requested_path / "data.win");
                            add_candidate(requested_path / "game.win");
                            working_directory = requested_path.has_extension()
                                                    ? requested_path.parent_path()
                                                    : std::move(requested_path);
                        }
                    } else if (!value.empty() && !working_directory) {
                        auto requested_path = clean_path(value);
                        if (!requested_path.empty())
                            working_directory = std::filesystem::path(requested_path);
                    }
                }
            }
            if (working_directory) {
                const auto package_name = launch_package
                                              ? launch_package->filename()
                                              : std::filesystem::path("data.win");
                add_candidate(*working_directory / package_name);
                add_candidate(*working_directory / "data.win");
            }
            if (launch_package && working_directory)
                add_candidate(*working_directory / *launch_package);
            if (chapter) {
                const auto prefix = "chapter" + std::to_string(*chapter);
                for (const auto *suffix : {"_windows", "_switch", "_mac", "_ps4", "_ps5", ""}) {
                    const auto directory = std::filesystem::path(prefix + suffix);
                    add_candidate(directory / "data.win");
                    add_candidate(directory / "game.win");
                }
            }

            std::error_code error;
            const auto target = std::find_if(candidates.begin(), candidates.end(), [&](const auto &path) {
                error.clear();
                return std::filesystem::is_regular_file(path, error) && !error;
            });
            if (target == candidates.end())
                throw Error(ErrorKind::io, "game_change package not found for " + request);
            pending_game_change = *target;
            throw GameChangeRequested{};
        }
        auto find_layer = [&](int id) -> RoomLayer * {
            if (current_room < 0 || static_cast<std::size_t>(current_room) >= rooms.size())
                return nullptr;
            auto &current = rooms[static_cast<std::size_t>(current_room)];
            const auto found = current.layer_indices.find(id);
            return found == current.layer_indices.end() ? nullptr : &current.layers[found->second];
        };
        auto layer_id_argument = [&](std::size_t index) {
            if (index >= args.size())
                throw Error(ErrorKind::vm, "missing built-in argument");
            if (value_is_string(args[index])) {
                const auto layer_name = value_text(args[index]);
                if (current_room >= 0 && static_cast<std::size_t>(current_room) < rooms.size()) {
                    const auto &current = rooms[static_cast<std::size_t>(current_room)];
                    const auto found = std::find_if(
                        current.layers.begin(), current.layers.end(),
                        [&](const RoomLayer &layer) { return layer.name == layer_name; });
                    if (found != current.layers.end())
                        return found->id;
                }
                return -1;
            }
            return number(args[index]);
        };
        auto find_layer_sprite = [&](int id) -> std::pair<RoomLayer *, LayerSprite *> {
            if (current_room < 0 || static_cast<std::size_t>(current_room) >= rooms.size())
                return {};
            auto &current = rooms[static_cast<std::size_t>(current_room)];
            for (auto &layer : current.layers)
                for (auto &sprite : layer.sprites)
                    if (sprite.id == id)
                        return {&layer, &sprite};
            return {};
        };
        auto find_layer_background = [&](int id) -> std::pair<RoomLayer *, LayerBackground *> {
            if (current_room < 0 || static_cast<std::size_t>(current_room) >= rooms.size())
                return {};
            auto &current = rooms[static_cast<std::size_t>(current_room)];
            for (auto &layer : current.layers)
                for (auto &background : layer.backgrounds)
                    if (background.id == id)
                        return {&layer, &background};
            return {};
        };
        auto find_layer_tile = [&](int id) -> std::pair<RoomLayer *, Tile *> {
            if (current_room < 0 || static_cast<std::size_t>(current_room) >= rooms.size())
                return {};
            auto &current = rooms[static_cast<std::size_t>(current_room)];
            for (auto &tile : current.tiles) {
                if (tile.element_id != id)
                    continue;
                RoomLayer *layer = nullptr;
                if (const auto found = current.layer_indices.find(tile.layer_id);
                    found != current.layer_indices.end())
                    layer = &current.layers[found->second];
                return {layer, &tile};
            }
            return {};
        };
        auto move_layer = [&](RoomLayer &layer, double x, double y) {
            layer.x = x;
            layer.y = y;
        };
        auto view_suffix = [](int view) { return "[" + std::to_string(view) + "]"; };
        auto view_value = [&](std::string_view field, int view, Value fallback = std::monostate{}) -> Value {
            const auto key = std::string(field) + view_suffix(view);
            const auto found = engine.find(key);
            return found == engine.end() ? fallback : found->second;
        };
        auto view_index = [&](std::size_t argument = 0) {
            const int view = static_cast<int>(n(argument));
            if (view < 0 || view >= 8)
                throw Error(ErrorKind::vm, "view index out of range");
            return view;
        };
        auto camera_for_id = [&](int id) -> Camera & {
            auto &camera = cameras.try_emplace(id).first->second;
            for (int view = 0; view < 8; ++view) {
                if (number(view_value("view_camera", view, std::int32_t{view})) != id)
                    continue;
                camera.x = value_number(view_value("view_xview", view));
                camera.y = value_number(view_value("view_yview", view));
                camera.width = value_number(view_value("view_wview", view));
                camera.height = value_number(view_value("view_hview", view));
                camera.border_x = value_number(view_value("view_hborder", view));
                camera.border_y = value_number(view_value("view_vborder", view));
                camera.speed_x = value_number(view_value("view_hspeed", view, -1.0));
                camera.speed_y = value_number(view_value("view_vspeed", view, -1.0));
                camera.target = static_cast<int>(value_number(view_value("view_object", view, -1.0)));
                break;
            }
            return camera;
        };
        auto camera_for_view = [&](int view) -> int {
            const auto key = "view_camera" + view_suffix(view);
            const auto found = engine.find(key);
            const int id = found == engine.end() ? view : number(found->second);
            engine[key] = std::int32_t{id};
            (void)camera_for_id(id);
            return id;
        };
        auto sync_camera_to_views = [&](int id) {
            const auto found = cameras.find(id);
            if (found == cameras.end())
                return;
            const auto &camera = found->second;
            for (int view = 0; view < 8; ++view) {
                if (number(view_value("view_camera", view, std::int32_t{view})) != id)
                    continue;
                const auto suffix = view_suffix(view);
                engine["view_xview" + suffix] = camera.x;
                engine["view_yview" + suffix] = camera.y;
                engine["view_wview" + suffix] = camera.width;
                engine["view_hview" + suffix] = camera.height;
                engine["view_hborder" + suffix] = camera.border_x;
                engine["view_vborder" + suffix] = camera.border_y;
                engine["view_hspeed" + suffix] = camera.speed_x;
                engine["view_vspeed" + suffix] = camera.speed_y;
                engine["view_object" + suffix] = std::int32_t{camera.target};
            }
        };
        auto make_id_array = [](const std::vector<int> &ids) {
            ArrayPtr result(new ArrayValue);
            result->row_lengths[0] = static_cast<std::int32_t>(ids.size());
            for (std::size_t i = 0; i < ids.size(); ++i)
                result->elements[static_cast<int>(i)] = std::int32_t{ids[i]};
            return result;
        };
        auto set_velocity = [&](std::string_view component, double value) {
            self->vars[std::string(component)] = value;
            const double h = value_number(self->vars.at("hspeed")), v = value_number(self->vars.at("vspeed"));
            self->vars["speed"] = std::hypot(h, v);
            if (h != 0 || v != 0)
                self->vars["direction"] = std::fmod(std::atan2(-v, h) * 180 / 3.141592653589793 + 360, 360);
        };
        Canvas &canvas = surface_target == 0 ? this->canvas : surfaces.at(surface_target);
        configure_shader(canvas);
        if (name == "@@This@@") {
            if (struct_self)
                return struct_self;
            return self ? Value{static_cast<std::int32_t>(self->id)} : Value{std::int32_t{-1}};
        }
        if (name == "@@NullObject@@")
            return std::monostate{};
        if (name == "@@NewGMLArray@@") {
            if (args.size() > 32000)
                throw Error(ErrorKind::vm, "array literal exceeds the one-dimensional array limit");
            ArrayPtr array(new ArrayValue);
            array->elements.reserve_dense(args.size());
            for (std::size_t i = 0; i < args.size(); ++i)
                array->elements[static_cast<std::int32_t>(i)] = args[i];
            array->row_lengths[0] = static_cast<std::int32_t>(args.size());
            return array;
        }
        if (name == "@@GML_HasOwnProperty@@") {
            if (args.size() != 2 || !value_is_string(args[1]))
                throw Error(ErrorKind::vm, "@@GML_HasOwnProperty@@ requires a value and property name");
            const auto property_name = value_string_view(args[1]);
            if (const auto *structure = std::get_if<StructPtr>(&args[0]); structure && *structure) {
                if (structure->get() == gmrt_global_value.get())
                    return gmrt_global_value->variables.contains(property_name);
                if (const auto instance = gmrt_instance_values.find(structure->get());
                    instance != gmrt_instance_values.end())
                    return instance->second->vars.contains(property_name);
                return (*structure)->variables.contains(property_name);
            }
            if (const auto *callable = std::get_if<CallablePtr>(&args[0]); callable && *callable)
                return (*callable)->properties && (*callable)->properties->variables.contains(property_name);
            if (const auto *array = std::get_if<ArrayPtr>(&args[0]); array && *array) {
                const auto &elements = (*array)->elements;
                if (property_name == "length")
                    return true;
                int index = -1;
                const auto parsed = std::from_chars(property_name.data(),
                                                    property_name.data() + property_name.size(), index);
                return parsed.ec == std::errc{} && parsed.ptr == property_name.data() + property_name.size() &&
                       index >= 0 && elements.contains(index);
            }
            return false;
        }
        if (name == "method") {
            if (args.size() < 2)
                throw Error(ErrorKind::vm, "method requires a receiver and function");
            std::uint32_t symbol_id = Instruction::no_symbol;
            CallablePtr existing_callable;
            if (const auto *function = std::get_if<FunctionReference>(&args[1]))
                symbol_id = function->symbol_id;
            else if (const auto *function = std::get_if<CallablePtr>(&args[1]); function && *function) {
                existing_callable = *function;
                symbol_id = existing_callable->symbol_id;
            }
            if (symbol_id == Instruction::no_symbol &&
                (!existing_callable || !existing_callable->invoke))
                throw Error(ErrorKind::unsupported, "method requires a function or callable value");
            InstancePtr target;
            StructPtr target_struct;
            if (const auto *value = std::get_if<StructPtr>(&args[0]))
                target_struct = *value;
            else if (!std::holds_alternative<std::monostate>(args[0]))
                target = instance_for_value(args[0], self, other);
            auto callable = std::make_shared<CallableValue>();
            callable->symbol_id = symbol_id;
            callable->bound_struct = target_struct;
            if (symbol_id != Instruction::no_symbol)
                callable->invoke =
                    [this, symbol_id, target, target_struct, other](std::span<const Value> call_args) {
                        return invoke_function(symbol_id, call_args, target, other, target_struct);
                    };
            else
                callable->invoke = [existing_callable](std::span<const Value> call_args) {
                    return existing_callable->invoke(call_args);
                };
            return callable;
        }
        if (name == "@@NewGMLObject@@") {
            if (args.empty())
                throw Error(ErrorKind::vm, "@@NewGMLObject@@ requires a constructor");
            std::uint32_t symbol_id = Instruction::no_symbol;
            if (const auto *reference = std::get_if<FunctionReference>(&args[0]))
                symbol_id = reference->symbol_id;
            else if (const auto *callable = std::get_if<CallablePtr>(&args[0]); callable && *callable)
                symbol_id = (*callable)->symbol_id;
            if (symbol_id == Instruction::no_symbol)
                throw Error(ErrorKind::unsupported, "@@NewGMLObject@@ requires a constructor function");
            auto object = std::make_shared<StructValue>();
            invoke_function(symbol_id, args.subspan(1), {}, other, object);
            return object;
        }
        if (name == "audio_destroy_stream") {
            return std::int32_t{audio.destroy_stream(static_cast<int>(n(0))) ? 1 : -1};
        }
        if (name == "@@Other@@")
            return std::int32_t{other ? other->id : -1};
        if (name == "@@GetInstance@@") {
            if (args.empty())
                return std::int32_t{-1};
            auto instance = instance_for_value(args[0], self, other);
            if (!instance) {
                if (const auto *object = std::get_if<std::int32_t>(&args[0]);
                    object && *object >= 0 && static_cast<std::size_t>(*object) < objects.size())
                    instance.reset(first_target(*object, self, other));
            }
            return std::int32_t{instance ? instance->id : -1};
        }
        if (gmrt_project && name == "external_define")
            return std::int32_t{-1};
        if (gmrt_project && name == "external_call")
            return 0.0;
        if (const auto it = script_names.find(name); it != script_names.end()) {
            return execute(it->second, self, args, other, struct_self);
        }
        if (name == "script_execute") {
            if (args.empty())
                throw Error(ErrorKind::vm, "script_execute requires a script or function");
            if (const auto *function = std::get_if<FunctionReference>(&args[0]))
                return invoke_function(function->symbol_id, args.subspan(1), self, other, struct_self);
            if (const auto *callable = std::get_if<CallablePtr>(&args[0])) {
                if (!*callable || !(*callable)->invoke)
                    throw Error(ErrorKind::vm, "script_execute received an empty callable");
                return (*callable)->invoke(args.subspan(1));
            }
            const auto id = static_cast<std::size_t>(n(0));
            if (id >= scripts.size())
                throw Error(ErrorKind::vm, "script ID out of range");
            return execute(scripts[id], self, args.subspan(1), other, struct_self);
        }
        if (name == "os_get_language")
            return std::string("en");
        if (name == "date_current_datetime") {
            const auto now = std::chrono::system_clock::now().time_since_epoch();
            return std::chrono::duration<double>(now).count() / 86400.0 + 25569.0;
        }
        if (name == "os_get_region")
            return std::string("US");
        if (name == "parameter_count")
            return std::int32_t{0};
        if (name == "parameter_string")
            return std::string{};
        if (name == "environment_get_variable") {
            const std::string variable = text(0);
            const char *value = std::getenv(variable.c_str());
            return std::string(value ? value : "");
        }
        if (name == "os_get_info") {
            auto info = std::make_shared<StructValue>();
#if defined(_WIN32)
            info->variables["platform"] = std::string("Windows");
#elif defined(__APPLE__)
            info->variables["platform"] = std::string("macOS");
#elif defined(__linux__)
            info->variables["platform"] = std::string("Linux");
#else
            info->variables["platform"] = std::string("Unknown");
#endif
            info->variables["device"] = std::string("Desktop");
            info->variables["version"] = std::string("reGML");
            info->variables["language"] = std::string("en");
            return info;
        }
        if (name == "show_error") {
            const auto message = text(0);
            std::cerr << "Game error: " << message << '\n';
            if (args.size() > 1 && n(1) != 0)
                throw Error(ErrorKind::vm, message);
            return std::monostate{};
        }
        if (name == "string")
            return text(0);
        if (name == "real") {
            if (value_is_string(args[0])) {
                // older runners treat an empty string like zero
                if (text(0).empty())
                    return 0.0;
                try {
                    return std::stod(text(0));
                } catch (const std::exception &) {
                    throw Error(ErrorKind::vm, "real conversion requires numeric string");
                }
            }
            return n(0);
        }
        if (name == "is_method")
            return !args.empty() && std::holds_alternative<CallablePtr>(args[0]) &&
                   static_cast<bool>(std::get<CallablePtr>(args[0]));
        if (name == "is_undefined") {
            if (args.empty())
                throw Error(ErrorKind::vm, "is_undefined argument missing");
            return std::holds_alternative<std::monostate>(args[0]);
        }
        if (name == "is_string")
            return value_is_string(args[0]);
        if (name == "is_array")
            return std::holds_alternative<ArrayPtr>(args[0]);
        if (name == "is_struct")
            return std::holds_alternative<StructPtr>(args[0]);
        if (name == "asset_get_index") {
            const auto requested = text(0);
            if (gmrt_project)
                return static_cast<std::int32_t>(gmrt_project->asset_index(requested));
            for (std::size_t i = 0; i < objects.size(); ++i)
                if (objects[i].name == requested)
                    return static_cast<std::int32_t>(i);
            for (std::size_t i = 0; i < rooms.size(); ++i)
                if (rooms[i].name == requested)
                    return static_cast<std::int32_t>(i);
            for (std::size_t i = 0; i < assets.sprite_count(); ++i)
                if (assets.sprite_name(static_cast<int>(i)) == requested)
                    return static_cast<std::int32_t>(i);
            if (const auto sound = audio.find_sound_id(requested))
                return static_cast<std::int32_t>(*sound);
            return std::int32_t{-1};
        }
        if (name == "script_exists") {
            const int id = static_cast<int>(n(0));
            if (gmrt_project)
                return gmrt_project->asset_exists(id, 6);
            return id >= 0 && static_cast<std::size_t>(id) < scripts.size();
        }
        if (name == "room_exists" || name == "object_exists") {
            const auto id = static_cast<int>(n(0));
            if (id < 0)
                return false;
            if (name == "room_exists")
                return static_cast<std::size_t>(id) < rooms.size() && !rooms[static_cast<std::size_t>(id)].name.empty();
            return static_cast<std::size_t>(id) < objects.size() && !objects[static_cast<std::size_t>(id)].name.empty();
        }
        if (name == "object_get_sprite") {
            const int id = static_cast<int>(n(0));
            return id >= 0 && static_cast<std::size_t>(id) < objects.size()
                       ? Value{std::int32_t{objects[static_cast<std::size_t>(id)].sprite}}
                       : Value{std::int32_t{-1}};
        }
        if (name == "variable_struct_set") {
            if (args.size() < 3 || !value_is_string(args[1]))
                throw Error(ErrorKind::vm, "variable_struct_set requires a struct, name, and value");
            const auto key = value_string_view(args[1]);
            if (const auto *structure = std::get_if<StructPtr>(&args[0]); structure && *structure) {
                (*structure)->variables[key] = args[2];
                return std::monostate{};
            }
            if (const auto *callable = std::get_if<CallablePtr>(&args[0]); callable && *callable) {
                if (!(*callable)->properties)
                    (*callable)->properties = std::make_shared<StructValue>();
                (*callable)->properties->variables[key] = args[2];
                return std::monostate{};
            }
            throw Error(ErrorKind::vm, "variable_struct_set received a non-struct value");
        }
        if (name == "array_create") {
            const int length = number(n(0));
            if (length < 0 || length > 32000)
                throw Error(ErrorKind::vm, "array_create length must be between 0 and 32000");
            ArrayPtr result(new ArrayValue);
            result->row_lengths[0] = length;
            result->elements.reserve_dense(static_cast<std::size_t>(length));
            const Value initial = args.size() > 1 ? args[1] : Value{std::int32_t{0}};
            for (int i = 0; i < length; ++i)
                result->elements.emplace(i, initial);
            return result;
        }
        if (name == "array_length" || name == "array_length_1d") {
            const auto *array = std::get_if<ArrayPtr>(&args[0]);
            if (!array || !*array)
                return std::int32_t{0};
            if (const auto found = (*array)->row_lengths.find(0); found != (*array)->row_lengths.end())
                return found->second;
            std::int32_t length = 0;
            (*array)->elements.for_each([&](std::int32_t index, const Value &) {
                if (index >= 0 && index < 32000)
                    length = std::max<std::int32_t>(length, index + 1);
            });
            return length;
        }
        if (name == "array_get") {
            if (args.size() < 2)
                throw Error(ErrorKind::vm, "array_get requires an array and index");
            const auto *array = std::get_if<ArrayPtr>(&args[0]);
            const int index = number(args[1]);
            if (!array || !*array || index < 0 || index >= 32000 * 32000)
                return std::monostate{};
            const auto *found = (*array)->elements.find(index);
            return found ? *found : Value{std::monostate{}};
        }
        if (name == "array_set") {
            if (args.size() < 3)
                throw Error(ErrorKind::vm, "array_set requires an array, index, and value");
            const auto *array = std::get_if<ArrayPtr>(&args[0]);
            const int index = number(args[1]);
            if (!array || !*array || index < 0 || index >= 32000 * 32000)
                throw Error(ErrorKind::vm, "array_set received an invalid array or index");
            ArrayPtr result = *array;
            if (result.use_count() > 1)
                result = ArrayPtr(new ArrayValue(**array));
            result->elements[index] = args[2];
            const int row = index / 32000;
            const int col = index % 32000;
            auto &length = result->row_lengths[row];
            length = std::max<std::int32_t>(length, static_cast<std::int32_t>(col + 1));
            return result;
        }
        if (name == "array_push") {
            if (args.size() < 2)
                throw Error(ErrorKind::vm, "array_push requires an array and at least one value");
            auto *array = std::get_if<ArrayPtr>(&args[0]);
            if (!array || !*array)
                throw Error(ErrorKind::vm, "array_push requires an array");
            auto &elements = (*array)->elements;
            auto &length = (*array)->row_lengths[0];
            if (length == 0) {
                elements.for_each([&](std::int32_t index, const Value &) {
                    if (index >= 0 && index < 32000)
                        length = std::max<std::int32_t>(length, index + 1);
                });
            }
            if (args.size() - 1 > static_cast<std::size_t>(32000 - length))
                throw Error(ErrorKind::vm, "array_push exceeds the one-dimensional array limit");
            for (std::size_t i = 1; i < args.size(); ++i)
                elements[length++] = args[i];
            return std::monostate{};
        }
        if (name == "array_delete") {
            if (args.size() != 3)
                throw Error(ErrorKind::vm, "array_delete requires an array, index, and count");
            auto *array = std::get_if<ArrayPtr>(&args[0]);
            if (!array || !*array)
                throw Error(ErrorKind::vm, "array_delete requires an array");
            auto &elements = (*array)->elements;
            auto &length = (*array)->row_lengths[0];
            if (length == 0)
                elements.for_each([&](std::int32_t element_index, const Value &) {
                    if (element_index >= 0 && element_index < 32000)
                        length = std::max(length, element_index + 1);
                });
            const int index = number(args[1]);
            const int count = number(args[2]);
            if (index < 0 || count < 0)
                throw Error(ErrorKind::vm, "array_delete index and count must be nonnegative");
            if (index >= length || count == 0)
                return *array;
            const int removed = static_cast<int>(std::min<std::int64_t>(count, length - index));
            const int new_length = length - removed;
            for (int destination = index; destination < new_length; ++destination) {
                const auto *source = elements.find(destination + removed);
                if (!source)
                    elements.erase(destination);
                else {
                    const Value value = *source;
                    elements[destination] = value;
                }
            }
            for (int element_index = new_length; element_index < length; ++element_index)
                elements.erase(element_index);
            length = new_length;
            return *array;
        }
        if (name == "is_real")
            return !value_is_string(args[0]) && !std::holds_alternative<std::monostate>(args[0]);
        if (name == "string_upper" || name == "string_lower") {
            auto str = text(0);
            for (auto &c : str)
                c = static_cast<char>(name == "string_upper" ? std::toupper(static_cast<unsigned char>(c))
                                                             : std::tolower(static_cast<unsigned char>(c)));
            return str;
        }
        if (name == "string_hash_to_newline") {
            auto str = text(0);
            std::replace(str.begin(), str.end(), '#', '\n');
            return str;
        }
        if (name == "string_split") {
            const std::string source = text(0), delimiter = text(1);
            const int max_splits = args.size() > 2 ? number(n(2)) : -1;
            ArrayPtr result(new ArrayValue);
            if (max_splits >= 0)
                result->elements.reserve_dense(static_cast<std::size_t>(max_splits) + 1);
            int count = 0;
            std::size_t start = 0;
            while ((max_splits < 0 || count < max_splits) && !delimiter.empty()) {
                const auto found = source.find(delimiter, start);
                if (found == std::string::npos)
                    break;
                result->elements[count++] = source.substr(start, found - start);
                start = found + delimiter.size();
            }
            if (delimiter.empty()) {
                for (std::size_t i = 0; i < source.size() && (max_splits < 0 || count < max_splits); ++i)
                    result->elements[count++] = source.substr(i, 1);
                start = source.size();
            }
            result->elements[count++] = source.substr(start);
            result->row_lengths[0] = count;
            return result;
        }
        if (name == "string_copy") {
            const auto str = text(0);
            const auto start = std::clamp(static_cast<int>(n(1)) - 1, 0, static_cast<int>(str.size()));
            return str.substr(static_cast<std::size_t>(start), static_cast<std::size_t>(std::max(0.0, n(2))));
        }
        if (name == "string_length" || name == "string_byte_length" || name == "strlen")
            return static_cast<double>(text(0).size());
        if (name == "string_replace_all" || name == "string_replace") {
            auto str = text(0);
            const auto from = text(1), to = text(2);
            if (from.empty())
                return str;
            std::size_t at = 0;
            while ((at = str.find(from, at)) != std::string::npos) {
                str.replace(at, from.size(), to);
                at += to.size();
                if (str.size() > 1'048'576)
                    throw Error(ErrorKind::vm, "string replacement exceeds limit");
                if (name == "string_replace")
                    break;
            }
            return str;
        }
        if (name == "string_pos") {
            const auto pos = text(1).find(text(0));
            return pos == std::string::npos ? 0.0 : static_cast<double>(pos + 1);
        }
        if (name == "string_delete") {
            auto str = text(0);
            const int at = std::max(0, static_cast<int>(n(1)) - 1);
            if (static_cast<std::size_t>(at) < str.size())
                str.erase(static_cast<std::size_t>(at), static_cast<std::size_t>(std::max(0.0, n(2))));
            return str;
        }
        if (name == "string_insert") {
            auto str = text(1);
            const auto at =
                static_cast<std::size_t>(std::clamp(n(2) - 1, 0.0, static_cast<double>(str.size())));
            str.insert(at, text(0));
            return str;
        }
        if (name == "string_char_at") {
            auto str = text(0);
            const int at = std::max(0, static_cast<int>(n(1)) - 1);
            return at >= 0 && static_cast<std::size_t>(at) < str.size()
                       ? str.substr(static_cast<std::size_t>(at), 1)
                       : std::string{};
        }
        if (name == "string_repeat") {
            const auto count = std::clamp(static_cast<int>(n(1)), 0, 1'000'000);
            const auto source = text(0);
            if (source.size() * static_cast<std::size_t>(count) > 16 * 1024 * 1024)
                throw Error(ErrorKind::vm, "string_repeat result exceeds limit");
            std::string result;
            result.reserve(source.size() * static_cast<std::size_t>(count));
            for (int i = 0; i < count; ++i)
                result += source;
            return result;
        }
        if (name == "ord") {
            const auto str = text(0);
            return static_cast<std::int32_t>(str.empty() ? 0 : static_cast<unsigned char>(str[0]));
        }
        if (name == "chr")
            return std::string(1, static_cast<char>(static_cast<int>(n(0))));
        if (name == "variable_global_exists")
            return globals.contains(text(0));
        if (name == "variable_instance_exists") {
            const auto &found = targets(static_cast<int>(n(0)), self, other);
            return !found.empty() && found[0]->vars.contains(text(1));
        }
        if (name == "variable_instance_get") {
            const auto found = first_target(static_cast<int>(n(0)), self, other);
            if (!found)
                return std::monostate{};
            const auto variable = found->vars.find(text(1));
            return variable == found->vars.end() ? Value{std::monostate{}} : variable->second;
        }
        if (name == "variable_instance_set") {
            const auto found = first_target(static_cast<int>(n(0)), self, other);
            if (found)
                found->vars[text(1)] = args[2];
            return std::monostate{};
        }
        if (name == "variable_global_get")
            return globals.at(text(0));
        if (name == "variable_global_set") {
            globals[text(0)] = args[1];
            return std::monostate{};
        }
        if (name == "ds_priority_create" || name == "ds_priority_create_managed") {
            const int id = next_ds++;
            priorities[id] = {};
            return std::int32_t{id};
        }
        if (name == "ds_priority_add") {
            priorities.at(static_cast<int>(n(0))).push_back({args[1], n(2)});
            return std::monostate{};
        }
        if (name == "ds_priority_clear") {
            priorities.at(static_cast<int>(n(0))).clear();
            return std::monostate{};
        }
        if (name == "ds_priority_destroy") {
            priorities.erase(static_cast<int>(n(0)));
            return std::monostate{};
        }
        if (name == "ds_priority_copy") {
            priorities.at(static_cast<int>(n(0))) = priorities.at(static_cast<int>(n(1)));
            return std::monostate{};
        }
        if (name == "ds_priority_empty")
            return priorities.at(static_cast<int>(n(0))).empty();
        if (name == "ds_priority_delete_min" || name == "ds_priority_delete_max") {
            auto &queue = priorities.at(static_cast<int>(n(0)));
            if (queue.empty())
                return std::int32_t{0};
            const auto found = name == "ds_priority_delete_min"
                                   ? std::min_element(queue.begin(), queue.end(), [](const auto &a, const auto &b) {
                                         return a.priority < b.priority;
                                     })
                                   : std::max_element(queue.begin(), queue.end(), [](const auto &a, const auto &b) {
                                         return a.priority < b.priority;
                                     });
            Value value = found->value;
            queue.erase(found);
            return value;
        }
        if (name == "ds_map_create") {
            const int id = next_ds++;
            maps.try_emplace(id);
            return std::int32_t{id};
        }
        if (name == "ds_map_set" || name == "ds_map_replace" || name == "ds_map_add" ||
            name == "ds_map_set_post") {
            auto &map = maps.at(static_cast<int>(n(0)));
            const bool string_key = value_is_string(args[1]);
            std::string key_storage;
            const auto key_value = text_view(1, key_storage);
            const bool inserted = map.set({string_key, key_value}, args[2], name == "ds_map_add");
            if (name == "ds_map_add")
                return inserted;
            return true;
        }
        if (name == "ds_map_exists" || name == "ds_map_find_value") {
            const auto &map = maps.at(static_cast<int>(n(0)));
            std::string key_storage;
            const DynamicMapKeyView key{value_is_string(args[1]), text_view(1, key_storage)};
            if (name == "ds_map_exists")
                return map.contains(key);
            const auto it = map.find(key);
            return it == map.end() ? Value{std::monostate{}} : it->second.to_value();
        }
        if (name == "ds_map_destroy") {
            maps.erase(static_cast<int>(n(0)));
            return std::monostate{};
        }
        if (name == "ds_map_delete") {
            auto &map = maps.at(static_cast<int>(n(0)));
            const auto key = (value_is_string(args[1]) ? "s:" : "n:") + text(1);
            return map.erase(key) != 0;
        }
        if (name == "ds_map_size")
            return static_cast<std::int32_t>(maps.at(static_cast<int>(n(0))).size());
        if (name == "ds_map_keys_to_array") {
            const auto &map = maps.at(static_cast<int>(n(0)));
            std::vector<std::string> keys;
            keys.reserve(map.size());
            for (const auto &[key, value] : map) {
                (void)value;
                keys.emplace_back(key);
            }
            std::sort(keys.begin(), keys.end());
            ArrayPtr result(new ArrayValue);
            result->row_lengths[0] = static_cast<std::int32_t>(keys.size());
            for (std::size_t i = 0; i < keys.size(); ++i) {
                if (keys[i].starts_with("s:"))
                    result->elements[static_cast<std::int32_t>(i)] = keys[i].substr(2);
                else {
                    try {
                        result->elements[static_cast<std::int32_t>(i)] = std::stod(keys[i].substr(2));
                    } catch (...) {
                        result->elements[static_cast<std::int32_t>(i)] = std::int32_t{0};
                    }
                }
            }
            return result;
        }
        if (name == "ds_map_find_first" || name == "ds_map_find_next") {
            if (name == "ds_map_find_next" && args.size() < 2)
                throw Error(ErrorKind::vm, "ds_map_find_next requires a map ID and key");
            const auto &map = maps.at(static_cast<int>(n(0)));
            std::vector<std::string> keys;
            keys.reserve(map.size());
            for (const auto &[key, value] : map) {
                (void)value;
                keys.emplace_back(key);
            }
            std::sort(keys.begin(), keys.end());
            auto key_value = [](const std::string &key) -> Value {
                if (key.starts_with("s:"))
                    return key.substr(2);
                try {
                    return std::stod(key.substr(2));
                } catch (...) {
                    return std::int32_t{0};
                }
            };
            if (name == "ds_map_find_first")
                return keys.empty() ? Value{std::int32_t{-1}} : key_value(keys.front());
            const std::string current = (value_is_string(args[1]) ? "s:" : "n:") + text(1);
            const auto found = std::find(keys.begin(), keys.end(), current);
            return found == keys.end() || found + 1 == keys.end() ? Value{std::int32_t{-1}}
                                                                    : key_value(*(found + 1));
        }
        if (name == "json_encode") {
            const auto &map = maps.at(static_cast<int>(n(0)));
            auto quote = [](std::string_view value) {
                std::string out = "\"";
                for (const unsigned char c : value) {
                    if (c == '"' || c == '\\') {
                        out += '\\';
                        out += static_cast<char>(c);
                    } else if (c == '\n')
                        out += "\\n";
                    else if (c == '\r')
                        out += "\\r";
                    else if (c == '\t')
                        out += "\\t";
                    else if (c >= 0x20)
                        out += static_cast<char>(c);
                }
                return out + '"';
            };
            std::string json = "{";
            bool first = true;
            for (const auto &[key, value] : map) {
                if (!first)
                    json += ',';
                first = false;
                json += quote(key.size() >= 2 ? std::string_view(key).substr(2) : std::string_view(key));
                json += ':';
                const auto decoded = value.to_value();
                if (value_is_string(decoded))
                    json += quote(value_string_view(decoded));
                else if (const auto *boolean = std::get_if<bool>(&decoded))
                    json += *boolean ? "true" : "false";
                else if (std::holds_alternative<std::monostate>(decoded))
                    json += "null";
                else
                    json += value_text(decoded);
            }
            return json + '}';
        }
        if (name == "json_decode") {
            const std::string json = text(0);
            const int id = next_ds++;
            auto &map = maps[id];
            std::size_t i = json.find('{');
            if (i == std::string::npos)
                return std::int32_t{id};
            ++i;
            auto whitespace = [&] {
                while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i])))
                    ++i;
            };
            auto parse_string = [&]() {
                std::string out;
                if (i >= json.size() || json[i++] != '"')
                    return out;
                while (i < json.size() && json[i] != '"') {
                    char c = json[i++];
                    if (c == '\\' && i < json.size()) {
                        c = json[i++];
                        if (c == 'n')
                            c = '\n';
                        else if (c == 'r')
                            c = '\r';
                        else if (c == 't')
                            c = '\t';
                    }
                    out += c;
                }
                if (i < json.size())
                    ++i;
                return out;
            };
            whitespace();
            while (i < json.size() && json[i] == '"') {
                auto key = parse_string();
                whitespace();
                if (i >= json.size() || json[i++] != ':')
                    break;
                whitespace();
                Value value;
                if (i < json.size() && json[i] == '"')
                    value = parse_string();
                else {
                    const auto start = i;
                    while (i < json.size() && json[i] != ',' && json[i] != '}' &&
                           !std::isspace(static_cast<unsigned char>(json[i])))
                        ++i;
                    const auto token = std::string_view(json).substr(start, i - start);
                    if (token == "true")
                        value = true;
                    else if (token == "false")
                        value = false;
                    else if (token == "null")
                        value = std::monostate{};
                    else {
                        try {
                            value = std::stod(std::string(token));
                        } catch (...) {
                            value = std::string(token);
                        }
                    }
                }
                map.set({true, key}, value, false);
                whitespace();
                if (i >= json.size() || json[i++] != ',')
                    break;
                whitespace();
            }
            return std::int32_t{id};
        }
        if (name == "ds_list_create") {
            const int id = next_ds++;
            lists[id] = {};
            return std::int32_t{id};
        }
        if (name == "ds_list_add") {
            auto &list = lists.at(static_cast<int>(n(0)));
            list.insert(list.end(), args.begin() + 1, args.end());
            return std::monostate{};
        }
        if (name == "ds_list_write") {
            const auto &list = lists.at(static_cast<int>(n(0)));
            std::string encoded = "RGML1:" + std::to_string(list.size()) + ":";
            for (const auto &value : list) {
                char tag = 's';
                std::string payload;
                if (std::holds_alternative<std::monostate>(value)) {
                    tag = 'u';
                } else if (const auto *integer = std::get_if<std::int32_t>(&value)) {
                    tag = 'i';
                    payload = std::to_string(*integer);
                } else if (const auto *real = std::get_if<double>(&value)) {
                    tag = 'd';
                    std::ostringstream number_text;
                    number_text.precision(std::numeric_limits<double>::max_digits10);
                    number_text << *real;
                    payload = number_text.str();
                } else if (const auto *boolean = std::get_if<bool>(&value)) {
                    tag = 'b';
                    payload = *boolean ? "1" : "0";
                } else
                    payload = value_text(value);
                encoded += tag + std::to_string(payload.size()) + ":" + payload;
            }
            return encoded;
        }
        if (name == "ds_list_read") {
            auto &list = lists.at(static_cast<int>(n(0)));
            list.clear();
            const auto serialized = text(1);
            if (!serialized.starts_with("RGML1:"))
                return std::monostate{};
            std::size_t cursor = 6;
            const auto count_end = serialized.find(':', cursor);
            if (count_end == std::string::npos)
                return std::monostate{};
            std::size_t count = 0;
            try {
                count = std::stoull(serialized.substr(cursor, count_end - cursor));
            } catch (...) {
                return std::monostate{};
            }
            if (count > 1'000'000)
                throw Error(ErrorKind::vm, "ds_list_read entry limit exceeded");
            cursor = count_end + 1;
            list.reserve(count);
            for (std::size_t i = 0; i < count && cursor < serialized.size(); ++i) {
                const char tag = serialized[cursor++];
                const auto length_end = serialized.find(':', cursor);
                if (length_end == std::string::npos)
                    break;
                std::size_t length = 0;
                try {
                    length = std::stoull(serialized.substr(cursor, length_end - cursor));
                } catch (...) {
                    break;
                }
                cursor = length_end + 1;
                if (length > serialized.size() - cursor)
                    break;
                const auto payload = serialized.substr(cursor, length);
                cursor += length;
                Value value;
                try {
                    if (tag == 'i')
                        value = static_cast<std::int32_t>(std::stol(payload));
                    else if (tag == 'd')
                        value = std::stod(payload);
                    else if (tag == 'b')
                        value = payload == "1";
                    else if (tag == 'u')
                        value = std::monostate{};
                    else
                        value = payload;
                } catch (...) {
                    value = std::monostate{};
                }
                list.push_back(std::move(value));
            }
            return std::monostate{};
        }
        if (name == "ds_list_size")
            return static_cast<double>(lists.at(static_cast<int>(n(0))).size());
        if (name == "ds_list_find_value")
            return lists.at(static_cast<int>(n(0))).at(static_cast<std::size_t>(n(1)));
        if (name == "ds_list_destroy") {
            lists.erase(static_cast<int>(n(0)));
            return std::monostate{};
        }
        if (name == "ds_exists") {
            const int id = static_cast<int>(n(0)), type = static_cast<int>(n(1));
            if (type == 1)
                return maps.contains(id);
            if (type == 2)
                return lists.contains(id);
            if (type == 5)
                return priorities.contains(id);
            return false;
        }
        if (name == "buffer_create") {
            const auto size = static_cast<std::size_t>(std::clamp(n(0), 0.0, 64.0 * 1024 * 1024));
            const int id = next_buffer++;
            buffers.emplace(id, GameBuffer{std::vector<std::uint8_t>(size), 0});
            return std::int32_t{id};
        }
        if (name == "buffer_load") {
            const std::filesystem::path requested(text(0));
            std::vector<std::filesystem::path> candidates;
            if (requested.is_absolute())
                candidates.push_back(requested);
            else {
                if (!requested.has_parent_path())
                    candidates.push_back(save_dir / requested);
                candidates.push_back(game_dir / requested);
            }
            std::ifstream input;
            for (const auto &candidate : candidates) {
                input.open(candidate, std::ios::binary);
                if (input)
                    break;
                input.clear();
            }
            if (!input)
                return std::int32_t{-1};
            input.seekg(0, std::ios::end);
            const auto end = input.tellg();
            if (end < 0 || static_cast<std::uint64_t>(end) > 64ULL * 1024 * 1024)
                return std::int32_t{-1};
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
            input.seekg(0, std::ios::beg);
            if (!bytes.empty() &&
                !input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
                return std::int32_t{-1};
            const int id = next_buffer++;
            buffers.emplace(id, GameBuffer{std::move(bytes), 0});
            return std::int32_t{id};
        }
        if (name == "buffer_get_size")
            return static_cast<double>(buffers.at(static_cast<int>(n(0))).bytes.size());
        if (name == "buffer_delete") {
            buffers.erase(static_cast<int>(n(0)));
            return std::monostate{};
        }
        if (name == "buffer_write") {
            auto &buffer = buffers.at(static_cast<int>(n(0)));
            const int type = static_cast<int>(n(1));
            const Value value = args.size() > 2 ? args[2] : Value{std::int32_t{0}};
            std::vector<std::uint8_t> encoded;
            auto write_integer = [&](std::uint64_t number, std::size_t bytes) {
                for (std::size_t i = 0; i < bytes; ++i)
                    encoded.push_back(static_cast<std::uint8_t>(number >> (8 * i)));
            };
            if (type == 11 || type == 12) {
                const auto string = value_text(value);
                encoded.assign(string.begin(), string.end());
                if (type == 11)
                    encoded.push_back(0);
            } else if (type == 8)
                write_integer(std::bit_cast<std::uint32_t>(static_cast<float>(value_number(value))), 4);
            else if (type == 9)
                write_integer(std::bit_cast<std::uint64_t>(value_number(value)), 8);
            else {
                const std::size_t width = (type == 1 || type == 2 || type == 10)  ? 1
                                          : (type == 3 || type == 4 || type == 7) ? 2
                                          : (type == 5 || type == 6)              ? 4
                                                                                  : 0;
                if (!width)
                    throw Error(ErrorKind::vm, "unsupported buffer_write data type");
                write_integer(static_cast<std::uint64_t>(static_cast<std::int64_t>(value_number(value))),
                              width);
            }
            if (buffer.position + encoded.size() > 64 * 1024 * 1024)
                throw Error(ErrorKind::vm, "buffer exceeds size limit");
            if (buffer.position + encoded.size() > buffer.bytes.size())
                buffer.bytes.resize(buffer.position + encoded.size());
            std::copy(encoded.begin(), encoded.end(),
                      buffer.bytes.begin() + static_cast<std::ptrdiff_t>(buffer.position));
            buffer.position += encoded.size();
            return std::monostate{};
        }
        if (name == "buffer_read") {
            auto &buffer = buffers.at(static_cast<int>(n(0)));
            const int type = static_cast<int>(n(1));
            auto read_integer = [&](std::size_t width) {
                if (buffer.position + width > buffer.bytes.size())
                    throw Error(ErrorKind::vm, "buffer_read past end of buffer");
                std::uint64_t value = 0;
                for (std::size_t i = 0; i < width; ++i)
                    value |= static_cast<std::uint64_t>(buffer.bytes[buffer.position++]) << (8 * i);
                return value;
            };
            if (type == 11 || type == 12) {
                std::string value;
                while (buffer.position < buffer.bytes.size()) {
                    const auto ch = buffer.bytes[buffer.position++];
                    if (ch == 0 && type == 11)
                        break;
                    value.push_back(static_cast<char>(ch));
                }
                return value;
            }
            if (type == 8)
                return static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(read_integer(4))));
            if (type == 9)
                return std::bit_cast<double>(read_integer(8));
            const std::size_t width = (type == 1 || type == 2 || type == 10)  ? 1
                                      : (type == 3 || type == 4 || type == 7) ? 2
                                      : (type == 5 || type == 6)              ? 4
                                                                              : 0;
            if (!width)
                throw Error(ErrorKind::vm, "unsupported buffer_read data type");
            const auto value = read_integer(width);
            if (type == 2 || type == 4 || type == 6) {
                const unsigned bits = static_cast<unsigned>(width * 8);
                const auto signed_value = static_cast<std::int64_t>(value << (64 - bits)) >> (64 - bits);
                return static_cast<double>(signed_value);
            }
            return static_cast<double>(value);
        }
        auto save_path = [&](std::string str) {
            const std::filesystem::path p(str);
            if (p.is_absolute() || p.has_parent_path())
                throw Error(ErrorKind::unsupported, "save path must be a filename");
            return save_dir / p;
        };
        auto text_file = [&](std::size_t index) -> std::fstream * {
            if (index >= args.size())
                throw Error(ErrorKind::vm, "missing built-in argument");
            const auto found = files.find(static_cast<int>(value_number(args[index])));
            return found == files.end() ? nullptr : found->second.get();
        };
        if (name == "file_exists") {
            std::error_code error;
            const std::filesystem::path requested(text(0));
            if (requested.is_absolute())
                return std::filesystem::is_regular_file(requested, error);
            if (requested.has_parent_path()) {
                if (std::filesystem::is_regular_file(requested, error))
                    return true;
                error.clear();
                return std::filesystem::is_regular_file(game_dir / requested, error);
            }
            if (std::filesystem::is_regular_file(save_path(text(0)), error))
                return true;
            error.clear();
            return std::filesystem::is_regular_file(game_dir / text(0), error);
        }
        if (name == "file_delete") {
            std::error_code error;
            std::filesystem::remove(save_path(text(0)), error);
            return std::monostate{};
        }
        if (name == "file_copy") {
            const std::filesystem::path requested(text(0));
            std::filesystem::path source = requested;
            std::error_code error;
            if (!source.is_absolute()) {
                if (!source.has_parent_path()) {
                    const auto saved_source = save_path(text(0));
                    if (std::filesystem::is_regular_file(saved_source, error))
                        source = saved_source;
                    else
                        source = game_dir / requested;
                } else if (!std::filesystem::is_regular_file(source, error))
                    source = game_dir / requested;
            }
            const auto destination = save_path(text(1));
            if (!prepare_save_directory() || !std::filesystem::is_regular_file(source, error))
                return false;
            error.clear();
            std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing,
                                       error);
            return !error;
        }
        if (name == "file_rename") {
            const auto from = save_path(text(0)), to = save_path(text(1));
            std::error_code error;
            if (!prepare_save_directory())
                return false;
            std::filesystem::rename(from, to, error);
            return !error;
        }
        if (name == "steam_file_delete") {
            std::error_code error;
            std::filesystem::remove(save_path(text(0)), error);
            return !error;
        }
        if (name == "steam_file_write_file") {
            const auto path = save_path(text(0));
            if (!prepare_save_directory())
                return false;
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output)
                return false;
            if (args.size() > 1 && buffers.contains(static_cast<int>(value_number(args[1])))) {
                const auto &bytes = buffers.at(static_cast<int>(value_number(args[1]))).bytes;
                output.write(reinterpret_cast<const char *>(bytes.data()),
                             static_cast<std::streamsize>(bytes.size()));
            } else if (args.size() > 1) {
                const auto contents = value_text(args[1]);
                output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            }
            return static_cast<bool>(output);
        }
        if (name == "buffer_async_group_begin" || name == "buffer_async_group_end" ||
            name == "buffer_async_group_option")
            return std::monostate{};
        if (name == "buffer_load_async" || name == "buffer_save_async") {
            if (args.size() < 4)
                throw Error(ErrorKind::vm, std::string(name) + " requires buffer, offset, size and filename");
            auto &buffer = buffers.at(static_cast<int>(n(0)));
            const auto offset = static_cast<std::size_t>(std::clamp(n(1), 0.0, 64.0 * 1024 * 1024));
            const auto size = static_cast<std::size_t>(std::clamp(n(2), 0.0, 64.0 * 1024 * 1024));
            const auto path = save_path(text(3));
            if (name == "buffer_load_async") {
                std::ifstream input(path, std::ios::binary);
                if (!input)
                    input.open(game_dir / text(3), std::ios::binary);
                if (input) {
                    std::vector<std::uint8_t> bytes(size);
                    input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size));
                    bytes.resize(static_cast<std::size_t>(input.gcount()));
                    if (offset + bytes.size() > buffer.bytes.size())
                        buffer.bytes.resize(offset + bytes.size());
                    std::copy(bytes.begin(), bytes.end(),
                              buffer.bytes.begin() + static_cast<std::ptrdiff_t>(offset));
                }
            } else {
                if (!prepare_save_directory())
                    return std::int32_t{-1};
                std::ofstream output(path, std::ios::binary | std::ios::trunc);
                if (!output)
                    return std::int32_t{-1};
                if (offset < buffer.bytes.size()) {
                    const auto count = std::min(size, buffer.bytes.size() - offset);
                    output.write(reinterpret_cast<const char *>(buffer.bytes.data() + offset),
                                 static_cast<std::streamsize>(count));
                }
            }
            return std::int32_t{next_async_request++};
        }
        if (name == "file_text_open_write" || name == "file_text_open_read" ||
            name == "file_text_open_append") {
            const auto path = save_path(text(0));
            if (name != "file_text_open_read" && !prepare_save_directory())
                return std::int32_t{-1};
            const auto mode = name == "file_text_open_read"     ? std::ios::in
                              : name == "file_text_open_append" ? std::ios::out | std::ios::app
                                                                : std::ios::out | std::ios::trunc;
            auto file = std::make_unique<std::fstream>(path, mode);
            if (!*file)
                return std::int32_t{-1};
            const auto id = next_file++;
            files.emplace(id, std::move(file));
            return std::int32_t{id};
        }
        if (name == "file_text_close") {
            files.erase(static_cast<int>(n(0)));
            return std::monostate{};
        }
        if (name == "file_text_write_string" || name == "file_text_write_real") {
            if (auto *file = text_file(0))
                *file << text(1);
            return std::monostate{};
        }
        if (name == "file_text_read_real") {
            auto *file = text_file(0);
            if (!file)
                return 0.0;
            double value = 0;
            *file >> value;
            return value;
        }
        if (name == "file_text_read_string") {
            auto *file = text_file(0);
            if (!file)
                return std::string{};
            std::string value;
            while (file->peek() != std::char_traits<char>::eof() && file->peek() != '\r' &&
                   file->peek() != '\n')
                value.push_back(static_cast<char>(file->get()));
            return value;
        }
        if (name == "file_text_readln") {
            auto *file = text_file(0);
            if (!file)
                return std::monostate{};
            std::string line;
            std::getline(*file, line);
            return std::monostate{};
        }
        if (name == "file_text_eof") {
            auto *file = text_file(0);
            return !file || file->peek() == std::char_traits<char>::eof();
        }
        if (name == "file_text_eoln") {
            auto *file = text_file(0);
            if (!file)
                return true;
            const auto ch = file->peek();
            return ch == '\r' || ch == '\n' || ch == std::char_traits<char>::eof();
        }
        if (name == "file_text_writeln") {
            if (auto *file = text_file(0))
                *file << '\n';
            return std::monostate{};
        }
        if (name == "ini_open" || name == "ini_open_from_string") {
            if (ini_opened)
                call("ini_close", {}, self, other);
            ini_path = name == "ini_open" ? save_path(text(0)) : std::filesystem::path{};
            ini.clear();
            ini_dirty = false;
            std::string line, section;
            auto parse_line = [&](const std::string &raw) {
                line = raw;
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.size() > 1 && line.front() == '[' && line.back() == ']')
                    section = line.substr(1, line.size() - 2);
                else if (const auto eq = line.find('='); eq != std::string::npos)
                    ini[section][line.substr(0, eq)] = line.substr(eq + 1);
            };
            if (name == "ini_open") {
                std::ifstream file(ini_path);
                while (std::getline(file, line))
                    parse_line(line);
            } else {
                std::istringstream input(text(0));
                while (std::getline(input, line))
                    parse_line(line);
            }
            ini_opened = true;
            return std::monostate{};
        }
        if (name == "ini_read_real" || name == "ini_read_string") {
            if (!ini_opened)
                throw Error(ErrorKind::vm, "INI read without open");
            const auto section = ini.find(text(0));
            if (section == ini.end())
                return args[2];
            const auto entry = section->second.find(text(1));
            if (entry == section->second.end())
                return args[2];
            if (name == "ini_read_string")
                return entry->second;
            try {
                return std::stod(entry->second);
            } catch (const std::exception &) {
                return args[2];
            }
        }
        if (name == "ini_write_real" || name == "ini_write_string") {
            if (!ini_opened)
                throw Error(ErrorKind::vm, "INI write without open");
            ini[text(0)][text(1)] = text(2);
            ini_dirty = true;
            return std::monostate{};
        }
        if (name == "ini_section_exists")
            return ini.contains(text(0));
        if (name == "ini_key_exists") {
            const auto section = ini.find(text(0));
            return section != ini.end() && section->second.contains(text(1));
        }
        if (name == "ini_section_delete") {
            ini.erase(text(0));
            ini_dirty = true;
            return std::monostate{};
        }
        if (name == "ini_key_delete") {
            if (auto section = ini.find(text(0)); section != ini.end())
                section->second.erase(text(1));
            ini_dirty = true;
            return std::monostate{};
        }
        if (name == "ini_close") {
            if (ini_dirty && !ini_path.empty()) {
                if (!prepare_save_directory()) {
                    ini_path.clear();
                    ini.clear();
                    ini_dirty = false;
                    ini_opened = false;
                    return std::monostate{};
                }
                std::ofstream file(ini_path);
                if (file) {
                    for (const auto &[section, entries] : ini) {
                        file << '[' << section << "]\n";
                        for (const auto &[key, value] : entries)
                            file << key << '=' << value << '\n';
                    }
                }
            }
            ini_path.clear();
            ini.clear();
            ini_dirty = false;
            ini_opened = false;
            return std::monostate{};
        }
        if (name == "steam_initialised" || name == "steam_is_overlay_enabled" || name == "steam_file_exists")
            return false;
        if (name == "math_get_epsilon")
            return comparison_epsilon;
        if (name == "math_set_epsilon") {
            const double epsilon = n(0);
            if (!std::isfinite(epsilon) || epsilon < 0 || epsilon >= 1)
                throw Error(ErrorKind::vm, "comparison epsilon must be between 0 and 1 (exclusive)");
            comparison_epsilon = epsilon;
            return std::monostate{};
        }
        if (name == "randomize") {
            if (!fixed_seed)
                random.seed(
                    static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
            return std::monostate{};
        }
        if (name == "audio_set_master_gain") {
            const auto listener = static_cast<int>(n(0));
            const auto gain = n(1);
            if (listener == 0) {
                if (!std::isfinite(gain))
                    throw Error(ErrorKind::vm, "master audio gain must be finite");
                audio_master_gain.store(static_cast<float>(std::clamp(gain, 0.0, 1.0)),
                                        std::memory_order_relaxed);
            }
            return std::monostate{};
        }
        if (name == "audio_master_gain") {
            const auto gain = n(0);
            if (!std::isfinite(gain))
                throw Error(ErrorKind::vm, "master audio gain must be finite");
            audio_master_gain.store(static_cast<float>(std::clamp(gain, 0.0, 1.0)),
                                    std::memory_order_relaxed);
            return std::monostate{};
        }
        if (name == "audio_group_load")
            return audio.load_group(static_cast<int>(n(0)));
        if (name == "audio_group_is_loaded")
            return audio.group_is_loaded(static_cast<int>(n(0)));
        if (name == "audio_group_set_gain") {
            audio.group_gain(static_cast<int>(n(0)), n(1), args.size() > 2 ? n(2) : 0);
            return std::monostate{};
        }
        if (name == "audio_group_get_gain")
            return audio.group_gain(static_cast<int>(n(0)));
        if (name == "audio_channel_num") {
            audio.channel_limit(static_cast<int>(n(0)));
            return std::monostate{};
        }
        if (gmrt_project && name.starts_with("audio_")) {
            if (name == "audio_play_sound" || name == "audio_create_stream")
                return std::int32_t{-1};
            if (name == "audio_is_playing")
                return false;
            if (name == "audio_sound_get_gain" || name == "audio_sound_get_pitch" ||
                name == "audio_sound_length" || name == "audio_sound_get_track_position")
                return 0.0;
            return std::monostate{};
        }
        if (name == "audio_create_stream")
            return std::int32_t{audio.create_stream(text(0))};
        if (name == "audio_play_sound")
            return std::int32_t{audio.play(static_cast<int>(n(0)), n(2) > 0.5, n(1))};
        if (name == "audio_stop_sound") {
            audio.stop(static_cast<int>(n(0)));
            return std::monostate{};
        }
        if (name == "audio_stop_all") {
            audio.stop(-1);
            return std::monostate{};
        }
        if (name == "audio_pause_sound" || name == "audio_resume_sound") {
            audio.pause(static_cast<int>(n(0)), name == "audio_pause_sound");
            return std::monostate{};
        }
        if (name == "audio_pause_all" || name == "audio_resume_all") {
            audio.pause(-1, name == "audio_pause_all");
            return std::monostate{};
        }
        if (name == "audio_is_playing")
            return audio.playing(static_cast<int>(n(0)));
        if (name == "audio_sound_get_gain")
            return audio.gain(static_cast<int>(n(0)));
        if (name == "audio_sound_gain") {
            audio.gain(static_cast<int>(n(0)), n(1));
            return std::monostate{};
        }
        if (name == "audio_sound_pitch") {
            audio.pitch(static_cast<int>(n(0)), n(1));
            return std::monostate{};
        }
        if (name == "audio_sound_get_pitch")
            return audio.pitch(static_cast<int>(n(0)));
        if (name == "audio_sound_length")
            return audio.length(static_cast<int>(n(0)));
        if (name == "audio_sound_get_track_position")
            return audio.position(static_cast<int>(n(0)));
        if (name == "audio_sound_set_track_position") {
            audio.position(static_cast<int>(n(0)), n(1));
            return std::monostate{};
        }
        if (name == "sprite_prefetch") {
            assets.prefetch(static_cast<int>(n(0)));
            return std::int32_t{0};
        }
        if (name == "texture_prefetch") {
            const auto found = sprite_textures.find(static_cast<int>(n(0)));
            if (found != sprite_textures.end())
                assets.prefetch(found->second.first);
            return std::monostate{};
        }
        if (name == "texture_is_ready")
            return sprite_textures.contains(static_cast<int>(n(0)));
        if (name == "texturegroup_get_textures")
            return ArrayPtr(new ArrayValue);
        if (name == "sprite_get_texture")
            return std::int32_t{sprite_texture_handle(static_cast<int>(n(0)), n(1))};
        if (name == "layer_background_create") {
            auto *layer = find_layer(layer_id_argument(0));
            const int sprite_id = static_cast<int>(n(1));
            if (!layer || (sprite_id != -1 && !assets.sprite_exists(sprite_id)))
                return std::int32_t{-1};
            LayerBackground background;
            background.id = next_layer_element_id++;
            background.sprite = sprite_id;
            layer->backgrounds.push_back(background);
            return std::int32_t{background.id};
        }
        if (name == "layer_background_exists") {
            const auto element = find_layer_background(static_cast<int>(n(1)));
            return element.second && element.first && element.first->id == layer_id_argument(0);
        }
        if (name == "layer_background_destroy") {
            const int layer_id = layer_id_argument(0), element_id = static_cast<int>(n(1));
            if (auto *layer = find_layer(layer_id))
                std::erase_if(layer->backgrounds,
                              [element_id](const LayerBackground &background) { return background.id == element_id; });
            return std::monostate{};
        }
        if (name == "layer_background_get_sprite" || name == "layer_background_get_index" ||
            name == "layer_background_get_blend" || name == "layer_background_get_alpha" ||
            name == "layer_background_get_htiled" || name == "layer_background_get_vtiled" ||
            name == "layer_background_get_stretch" || name == "layer_background_get_xscale" ||
            name == "layer_background_get_yscale" || name == "layer_background_get_visible") {
            const auto background = find_layer_background(static_cast<int>(n(0)));
            if (!background.second)
                return name == "layer_background_get_sprite" ? Value{std::int32_t{-1}} : Value{0.0};
            const auto &value = *background.second;
            if (name == "layer_background_get_sprite")
                return std::int32_t{value.sprite};
            if (name == "layer_background_get_index")
                return value.frame;
            if (name == "layer_background_get_blend")
                return std::int32_t{static_cast<int>(value.color & 0xffffffU)};
            if (name == "layer_background_get_alpha")
                return static_cast<double>((value.color >> 24) & 0xff) / 255.0;
            if (name == "layer_background_get_htiled")
                return value.htiled;
            if (name == "layer_background_get_vtiled")
                return value.vtiled;
            if (name == "layer_background_get_stretch")
                return value.stretch;
            if (name == "layer_background_get_xscale")
                return value.sx;
            if (name == "layer_background_get_yscale")
                return value.sy;
            return value.visible;
        }
        if (name == "layer_background_change" || name == "layer_background_sprite" ||
            name == "layer_background_blend" || name == "layer_background_alpha" ||
            name == "layer_background_htiled" || name == "layer_background_vtiled" ||
            name == "layer_background_stretch" || name == "layer_background_xscale" ||
            name == "layer_background_yscale" || name == "layer_background_visible" ||
            name == "layer_background_index") {
            const auto background = find_layer_background(static_cast<int>(n(0)));
            if (background.second) {
                auto &value = *background.second;
                const double setting = n(1);
                if (name == "layer_background_change" || name == "layer_background_sprite") {
                    const int sprite_id = static_cast<int>(setting);
                    if (sprite_id == -1 || assets.sprite_exists(sprite_id)) {
                        value.sprite = sprite_id;
                        value.frame = 0;
                    }
                } else if (name == "layer_background_blend")
                    value.color = (value.color & 0xff000000U) | (static_cast<unsigned>(setting) & 0xffffffU);
                else if (name == "layer_background_alpha")
                    value.color = (value.color & 0x00ffffffU) |
                                  (static_cast<unsigned>(std::clamp(setting, 0.0, 1.0) * 255) << 24);
                else if (name == "layer_background_htiled")
                    value.htiled = setting != 0;
                else if (name == "layer_background_vtiled")
                    value.vtiled = setting != 0;
                else if (name == "layer_background_stretch")
                    value.stretch = setting != 0;
                else if (name == "layer_background_xscale")
                    value.sx = setting;
                else if (name == "layer_background_yscale")
                    value.sy = setting;
                else if (name == "layer_background_visible")
                    value.visible = setting != 0;
                else if (name == "layer_background_index")
                    value.frame = setting;
            }
            return std::monostate{};
        }
        if (name == "layer_get_all") {
            std::vector<int> ids;
            if (current_room >= 0 && static_cast<std::size_t>(current_room) < rooms.size()) {
                const auto &current = rooms[static_cast<std::size_t>(current_room)];
                ids.reserve(current.layers.size());
                for (const auto &layer : current.layers)
                    ids.push_back(layer.id);
            }
            return make_id_array(ids);
        }
        if (name == "layer_get_id") {
            if (current_room >= 0 && static_cast<std::size_t>(current_room) < rooms.size()) {
                const auto &current = rooms[static_cast<std::size_t>(current_room)];
                const auto found = std::find_if(current.layers.begin(), current.layers.end(),
                                                [&](const RoomLayer &layer) { return layer.name == text(0); });
                if (found != current.layers.end())
                    return std::int32_t{found->id};
            }
            return std::int32_t{-1};
        }
        if (name == "layer_exists")
            return find_layer(layer_id_argument(0)) != nullptr;
        if (name == "layer_set_visible") {
            if (auto *layer = find_layer(layer_id_argument(0)))
                layer->visible = n(1) != 0;
            return std::monostate{};
        }
        if (name == "layer_get_visible") {
            const auto *layer = find_layer(layer_id_argument(0));
            return layer && layer->visible;
        }
        if (name == "layer_get_name") {
            const auto *layer = find_layer(layer_id_argument(0));
            return layer ? Value{layer->name} : Value{std::string{}};
        }
        if (name == "layer_get_depth") {
            const auto *layer = find_layer(layer_id_argument(0));
            return std::int32_t{layer ? layer->depth : 0};
        }
        if (name == "layer_depth") {
            if (auto *layer = find_layer(layer_id_argument(0)))
                layer->depth = static_cast<int>(n(1));
            return std::monostate{};
        }
        if (name == "layer_x" || name == "layer_y") {
            if (auto *layer = find_layer(layer_id_argument(0))) {
                const auto x = name == "layer_x" ? n(1) : layer->x;
                const auto y = name == "layer_y" ? n(1) : layer->y;
                move_layer(*layer, x, y);
            }
            return std::monostate{};
        }
        if (name == "layer_hspeed" || name == "layer_vspeed") {
            if (auto *layer = find_layer(layer_id_argument(0))) {
                if (name == "layer_hspeed")
                    layer->hspeed = n(1);
                else
                    layer->vspeed = n(1);
            }
            return std::monostate{};
        }
        if (name == "layer_get_x" || name == "layer_get_y" || name == "layer_get_hspeed" ||
            name == "layer_get_vspeed") {
            const auto *layer = find_layer(layer_id_argument(0));
            if (!layer)
                return 0.0;
            if (name == "layer_get_x")
                return layer->x;
            if (name == "layer_get_y")
                return layer->y;
            if (name == "layer_get_hspeed")
                return layer->hspeed;
            return layer->vspeed;
        }
        if (name == "layer_force_draw_depth") {
            force_draw_depth = n(0) != 0;
            forced_depth = static_cast<int>(n(1));
            return std::monostate{};
        }
        if (name == "layer_is_draw_depth_forced")
            return force_draw_depth;
        if (name == "layer_get_forced_depth")
            return force_draw_depth ? forced_depth : 0;
        if (name == "layer_destroy") {
            if (current_room < 0 || static_cast<std::size_t>(current_room) >= rooms.size())
                return std::monostate{};
            auto &current = rooms[static_cast<std::size_t>(current_room)];
            const int layer_id = layer_id_argument(0);
            const auto found = current.layer_indices.find(layer_id);
            if (found == current.layer_indices.end())
                return std::monostate{};
            const auto instance_ids = current.layers[found->second].instances;
            for (const int instance_id : instance_ids) {
                const auto instance = std::find_if(instances.begin(), instances.end(),
                                                   [instance_id](const InstancePtr &item) {
                                                       return item->alive && item->id == instance_id;
                                                   });
                if (instance != instances.end()) {
                    event(*instance, 1, 0);
                    (*instance)->alive = false;
                }
            }
            std::erase_if(current.tiles, [layer_id](const Tile &tile) { return tile.layer_id == layer_id; });
            std::erase_if(current.layers, [layer_id](const RoomLayer &layer) { return layer.id == layer_id; });
            current.layer_indices.clear();
            for (std::size_t i = 0; i < current.layers.size(); ++i)
                current.layer_indices.try_emplace(current.layers[i].id, i);
            hidden_tile_layers.erase(layer_id);
            tile_layer_offsets.erase(layer_id);
            return std::monostate{};
        }
        if (name == "layer_shader") {
            if (auto *layer = find_layer(layer_id_argument(0)))
                layer->shader = static_cast<int>(n(1));
            return std::monostate{};
        }
        if (name == "layer_script_begin" || name == "layer_script_end") {
            if (auto *layer = find_layer(layer_id_argument(0))) {
                Value script = args.size() > 1 ? args[1] : Value{std::monostate{}};
                if (const auto *id = std::get_if<std::int32_t>(&script); id && *id == -1)
                    script = std::monostate{};
                if (name == "layer_script_begin")
                    layer->begin_script = std::move(script);
                else
                    layer->end_script = std::move(script);
            }
            return std::monostate{};
        }
        if (name == "layer_get_id_at_depth") {
            const int depth = static_cast<int>(n(0));
            std::vector<int> ids;
            if (current_room >= 0 && static_cast<std::size_t>(current_room) < rooms.size()) {
                const auto &current = rooms[static_cast<std::size_t>(current_room)];
                for (const auto &layer : current.layers)
                    if (layer.depth == depth)
                        ids.push_back(layer.id);
            }
            if (ids.empty())
                ids.push_back(-1);
            return make_id_array(ids);
        }
        if (name == "layer_create") {
            if (current_room < 0 || static_cast<std::size_t>(current_room) >= rooms.size())
                return std::int32_t{-1};
            RoomLayer layer;
            layer.id = next_dynamic_layer_id++;
            layer.depth = static_cast<int>(n(0));
            layer.type = 3;
            layer.name = args.size() > 1 ? text(1) : std::string{};
            auto &current = rooms[static_cast<std::size_t>(current_room)];
            current.layers.push_back(std::move(layer));
            current.layer_indices.try_emplace(current.layers.back().id, current.layers.size() - 1);
            return std::int32_t{next_dynamic_layer_id - 1};
        }
        if (name == "layer_get_all_elements") {
            const auto *layer = find_layer(layer_id_argument(0));
            std::vector<int> ids;
            if (layer) {
                ids.reserve(layer->instances.size() + layer->sprites.size() + layer->backgrounds.size() + 1);
                ids.insert(ids.end(), layer->instances.begin(), layer->instances.end());
                for (const auto &sprite : layer->sprites)
                    ids.push_back(sprite.id);
                for (const auto &background : layer->backgrounds)
                    ids.push_back(background.id);
                if (layer->tile_element_id >= 0)
                    ids.push_back(layer->tile_element_id);
            }
            return make_id_array(ids);
        }
        if (name == "layer_get_element_type") {
            const int id = static_cast<int>(n(0));
            const auto sprite = find_layer_sprite(id);
            if (sprite.second)
                return std::int32_t{4};
            if (find_layer_background(id).second)
                return std::int32_t{1};
            if (find_layer_tile(id).second)
                return std::int32_t{3};
            if (current_room >= 0 && static_cast<std::size_t>(current_room) < rooms.size()) {
                const auto &current = rooms[static_cast<std::size_t>(current_room)];
                for (const auto &layer : current.layers)
                    if (std::find(layer.instances.begin(), layer.instances.end(), id) != layer.instances.end())
                        return std::int32_t{2};
            }
            return std::int32_t{-1};
        }
        if (name == "layer_tile_get_x" || name == "layer_tile_get_y") {
            const auto tile = find_layer_tile(static_cast<int>(n(0)));
            if (!tile.second)
                return 0.0;
            const int layer_id = tile.second->layer_id < 0 ? tile.second->id : tile.second->layer_id;
            const auto offset = tile_layer_offsets.find(layer_id);
            const double legacy_x = offset == tile_layer_offsets.end() ? 0 : offset->second.first;
            const double legacy_y = offset == tile_layer_offsets.end() ? 0 : offset->second.second;
            const double layer_x = tile.first ? tile.first->x - tile.first->origin_x : 0;
            const double layer_y = tile.first ? tile.first->y - tile.first->origin_y : 0;
            return name == "layer_tile_get_x" ? tile.second->x + layer_x + legacy_x
                                              : tile.second->y + layer_y + legacy_y;
        }
        if (name == "layer_tile_x" || name == "layer_tile_y" || name == "layer_tile_alpha" ||
            name == "layer_tile_visible") {
            auto tile = find_layer_tile(static_cast<int>(n(0)));
            if (!tile.second)
                return std::monostate{};
            auto &current = rooms.at(static_cast<std::size_t>(current_room));
            const int element_id = tile.second->element_id;
            const double setting = n(1);
            if (name == "layer_tile_x" || name == "layer_tile_y") {
                const int layer_id = tile.second->layer_id < 0 ? tile.second->id : tile.second->layer_id;
                const auto offset = tile_layer_offsets.find(layer_id);
                const double legacy_offset = offset == tile_layer_offsets.end()
                                                 ? 0
                                                 : (name == "layer_tile_x" ? offset->second.first
                                                                           : offset->second.second);
                const double layer_offset = tile.first
                                                ? (name == "layer_tile_x" ? tile.first->x - tile.first->origin_x
                                                                          : tile.first->y - tile.first->origin_y)
                                                : 0;
                const double old_value = (name == "layer_tile_x" ? tile.second->x : tile.second->y) +
                                         layer_offset + legacy_offset;
                const double delta = setting - old_value;
                for (auto &entry : current.tiles)
                    if (entry.element_id == element_id) {
                        if (name == "layer_tile_x")
                            entry.x += delta;
                        else
                            entry.y += delta;
                    }
            } else if (name == "layer_tile_alpha") {
                const auto alpha = static_cast<unsigned>(std::clamp(setting, 0.0, 1.0) * 255);
                for (auto &entry : current.tiles)
                    if (entry.element_id == element_id)
                        entry.color = (entry.color & 0x00ffffffU) | (alpha << 24);
            } else {
                for (auto &entry : current.tiles)
                    if (entry.element_id == element_id)
                        entry.visible = setting != 0;
            }
            return std::monostate{};
        }
        if (name == "layer_sprite_destroy") {
            const int id = static_cast<int>(n(0));
            const auto sprite = find_layer_sprite(id);
            if (sprite.first)
                std::erase_if(sprite.first->sprites,
                              [id](const LayerSprite &item) { return item.id == id; });
            return std::monostate{};
        }
        if (name == "layer_sprite_get_x" || name == "layer_sprite_get_y") {
            const auto sprite = find_layer_sprite(static_cast<int>(n(0)));
            if (!sprite.second)
                return 0.0;
            return name == "layer_sprite_get_x" ? sprite.second->x : sprite.second->y;
        }
        if (name == "layer_sprite_get_xscale" || name == "layer_sprite_get_yscale" ||
            name == "layer_sprite_get_angle" || name == "layer_sprite_get_speed" ||
            name == "layer_sprite_get_index" || name == "layer_sprite_get_alpha" ||
            name == "layer_sprite_get_blend") {
            const auto sprite = find_layer_sprite(static_cast<int>(n(0)));
            if (!sprite.second)
                return 0.0;
            if (name == "layer_sprite_get_xscale")
                return sprite.second->sx;
            if (name == "layer_sprite_get_yscale")
                return sprite.second->sy;
            if (name == "layer_sprite_get_angle")
                return sprite.second->rotation;
            if (name == "layer_sprite_get_speed")
                return sprite.second->speed;
            if (name == "layer_sprite_get_index")
                return sprite.second->frame;
            if (name == "layer_sprite_get_alpha")
                return static_cast<double>((sprite.second->color >> 24) & 0xff) / 255.0;
            return static_cast<std::int32_t>(sprite.second->color & 0xffffffU);
        }
        if (name == "layer_sprite_get_sprite") {
            const auto sprite = find_layer_sprite(static_cast<int>(n(0)));
            return std::int32_t{sprite.second ? sprite.second->sprite : -1};
        }
        if (name == "layer_sprite_index" || name == "layer_sprite_speed" || name == "layer_sprite_x" ||
            name == "layer_sprite_y" || name == "layer_sprite_xscale" || name == "layer_sprite_yscale" ||
            name == "layer_sprite_angle" || name == "layer_sprite_alpha" || name == "layer_sprite_blend") {
            const auto sprite = find_layer_sprite(static_cast<int>(n(0)));
            if (sprite.second) {
                if (name == "layer_sprite_index")
                    sprite.second->frame = n(1);
                else if (name == "layer_sprite_speed")
                    sprite.second->speed = n(1);
                else if (name == "layer_sprite_x")
                    sprite.second->x = n(1);
                else if (name == "layer_sprite_y")
                    sprite.second->y = n(1);
                else if (name == "layer_sprite_xscale")
                    sprite.second->sx = n(1);
                else if (name == "layer_sprite_yscale")
                    sprite.second->sy = n(1);
                else if (name == "layer_sprite_angle")
                    sprite.second->rotation = n(1);
                else if (name == "layer_sprite_alpha")
                    sprite.second->color = (sprite.second->color & 0x00ffffffU) |
                                            (static_cast<unsigned>(std::clamp(n(1), 0.0, 1.0) * 255) << 24);
                else
                    sprite.second->color = (sprite.second->color & 0xff000000U) |
                                            (static_cast<unsigned>(n(1)) & 0xffffffU);
            }
            return std::monostate{};
        }
        if (name == "layer_sprite_create") {
            auto *layer = find_layer(layer_id_argument(0));
            const int sprite_id = static_cast<int>(n(3));
            if (!layer || !assets.sprite_exists(sprite_id))
                return std::int32_t{-1};
            LayerSprite sprite;
            sprite.id = next_layer_element_id++;
            sprite.sprite = sprite_id;
            sprite.x = n(1);
            sprite.y = n(2);
            layer->sprites.push_back(sprite);
            return std::int32_t{sprite.id};
        }
        if (name == "layer_element_move") {
            auto *target = find_layer(layer_id_argument(0));
            const int element_id = static_cast<int>(n(1));
            const auto source = find_layer_sprite(element_id);
            if (!target || !source.first || !source.second || source.first == target)
                return std::monostate{};
            const auto element = *source.second;
            std::erase_if(source.first->sprites,
                          [element_id](const LayerSprite &sprite) { return sprite.id == element_id; });
            target->sprites.push_back(element);
            return std::monostate{};
        }
        if (name == "sprite_get_number")
            return static_cast<double>(assets.sprite(static_cast<int>(n(0))).frames.size());
        if (name == "sprite_get_speed_type")
            return std::int32_t{0};
        if (name == "sprite_exists")
            return assets.sprite_exists(static_cast<int>(n(0)));
        if (name == "sprite_get_name")
            return std::string(assets.sprite_name(static_cast<int>(n(0))));
        if (name == "sprite_set_offset") {
            assets.set_sprite_offset(static_cast<int>(n(0)), static_cast<int>(n(1)), static_cast<int>(n(2)));
            return std::monostate{};
        }
        if (name == "sprite_create_from_surface") {
            const int surface_id = static_cast<int>(n(0));
            const Canvas *source = nullptr;
            if (surface_id == 0)
                source = &this->canvas;
            else if (const auto found = surfaces.find(surface_id); found != surfaces.end())
                source = &found->second;
            if (!source)
                return std::int32_t{-1};
            const int x = static_cast<int>(n(1));
            const int y = static_cast<int>(n(2));
            const int width = static_cast<int>(n(3));
            const int height = static_cast<int>(n(4));
            const bool remove_background = n(5) != 0;
            const bool smooth = n(6) != 0;
            const int origin_x = static_cast<int>(n(7));
            const int origin_y = static_cast<int>(n(8));
            return std::int32_t{assets.create_sprite_from_surface(*source,
                                                                  x,
                                                                  y,
                                                                  width,
                                                                  height,
                                                                  remove_background,
                                                                  smooth,
                                                                  origin_x,
                                                                  origin_y)};
        }
        if (name == "sprite_delete")
            return false;
        if (name == "sprite_replace")
            return false;
        if (name == "sprite_collision_mask")
            return std::monostate{};
        if (name == "background_get_width" || name == "background_get_height") {
            const auto [width, height] = assets.background_size(static_cast<int>(n(0)));
            return std::int32_t{name == "background_get_width" ? width : height};
        }
        if (name == "sprite_get_width")
            return std::int32_t{assets.sprite(static_cast<int>(n(0))).width};
        if (name == "sprite_get_height")
            return std::int32_t{assets.sprite(static_cast<int>(n(0))).height};
        if (name == "sprite_get_xoffset")
            return std::int32_t{assets.sprite(static_cast<int>(n(0))).ox};
        if (name == "sprite_get_yoffset")
            return std::int32_t{assets.sprite(static_cast<int>(n(0))).oy};
        if (name == "sprite_get_bbox_left" || name == "sprite_get_bbox_right" ||
            name == "sprite_get_bbox_top" || name == "sprite_get_bbox_bottom") {
            const int id = static_cast<int>(n(0));
            if (!assets.sprite_exists(id))
                return std::int32_t{-1};
            const auto &sprite = assets.sprite(id);
            if (name == "sprite_get_bbox_left")
                return std::int32_t{sprite.left};
            if (name == "sprite_get_bbox_right")
                return std::int32_t{sprite.right};
            if (name == "sprite_get_bbox_top")
                return std::int32_t{sprite.top};
            return std::int32_t{sprite.bottom};
        }
        if (name == "draw_set_color" || name == "draw_set_colour") {
            draw_color = static_cast<unsigned>(n(0));
            canvas.color(draw_color);
            return std::monostate{};
        }
        if (name == "draw_get_color")
            return static_cast<double>(draw_color);
        if (name == "draw_set_alpha") {
            draw_alpha = n(0);
            canvas.alpha(draw_alpha);
            return std::monostate{};
        }
        if (name == "draw_get_alpha")
            return draw_alpha;
        if (name == "font_add_sprite_ext")
            return static_cast<std::int32_t>(assets.add_sprite_font(static_cast<int>(n(0)),
                                                                    text(1),
                                                                    n(2) != 0,
                                                                    static_cast<int>(n(3))));
        if (name == "draw_set_font") {
            draw_font = static_cast<int>(n(0));
            return std::monostate{};
        }
        if (name == "draw_set_halign") {
            halign = static_cast<int>(n(0));
            return std::monostate{};
        }
        if (name == "draw_get_halign")
            return std::int32_t{halign};
        if (name == "camera_create") {
            const int id = next_camera_id++;
            Camera camera;
            if (args.size() > 0)
                camera.x = n(0);
            if (args.size() > 1)
                camera.y = n(1);
            if (args.size() > 2)
                camera.width = n(2);
            if (args.size() > 3)
                camera.height = n(3);
            if (args.size() > 4)
                camera.angle = n(4);
            if (args.size() > 5)
                camera.target = number(args[5]);
            if (args.size() > 6)
                camera.speed_x = n(6);
            if (args.size() > 7)
                camera.speed_y = n(7);
            if (args.size() > 8)
                camera.border_x = n(8);
            if (args.size() > 9)
                camera.border_y = n(9);
            cameras.emplace(id, camera);
            return std::int32_t{id};
        }
        if (name == "view_get_camera")
            return std::int32_t{camera_for_view(view_index())};
        if (name == "view_set_camera") {
            const int view = view_index();
            const int id = number(n(1));
            engine["view_camera" + view_suffix(view)] = std::int32_t{id};
            const auto &camera = camera_for_id(id);
            engine["view_xview" + view_suffix(view)] = camera.x;
            engine["view_yview" + view_suffix(view)] = camera.y;
            engine["view_wview" + view_suffix(view)] = camera.width;
            engine["view_hview" + view_suffix(view)] = camera.height;
            engine["view_hborder" + view_suffix(view)] = camera.border_x;
            engine["view_vborder" + view_suffix(view)] = camera.border_y;
            engine["view_hspeed" + view_suffix(view)] = camera.speed_x;
            engine["view_vspeed" + view_suffix(view)] = camera.speed_y;
            engine["view_object" + view_suffix(view)] = std::int32_t{camera.target};
            return std::monostate{};
        }
        if (name == "camera_get_view_x" || name == "camera_get_view_y" ||
            name == "camera_get_view_width" || name == "camera_get_view_height" ||
            name == "camera_get_view_angle" || name == "camera_get_view_target" ||
            name == "camera_get_view_border_x" || name == "camera_get_view_border_y" ||
            name == "camera_get_view_speed_x" || name == "camera_get_view_speed_y") {
            const auto &camera = camera_for_id(static_cast<int>(n(0)));
            if (name == "camera_get_view_x")
                return camera.x;
            if (name == "camera_get_view_y")
                return camera.y;
            if (name == "camera_get_view_width")
                return camera.width;
            if (name == "camera_get_view_height")
                return camera.height;
            if (name == "camera_get_view_angle")
                return camera.angle;
            if (name == "camera_get_view_target")
                return std::int32_t{camera.target};
            if (name == "camera_get_view_border_x")
                return camera.border_x;
            if (name == "camera_get_view_border_y")
                return camera.border_y;
            if (name == "camera_get_view_speed_x")
                return camera.speed_x;
            return camera.speed_y;
        }
        if (name == "camera_set_view_pos" || name == "camera_set_view_size" ||
            name == "camera_set_view_angle" || name == "camera_set_view_target" ||
            name == "camera_set_view_border" || name == "camera_set_view_speed") {
            auto &camera = camera_for_id(static_cast<int>(n(0)));
            if (name == "camera_set_view_pos") {
                camera.x = n(1);
                camera.y = n(2);
            } else if (name == "camera_set_view_size") {
                camera.width = n(1);
                camera.height = n(2);
            } else if (name == "camera_set_view_angle")
                camera.angle = n(1);
            else if (name == "camera_set_view_target")
                camera.target = number(n(1));
            else if (name == "camera_set_view_border") {
                camera.border_x = n(1);
                camera.border_y = n(2);
            } else {
                camera.speed_x = n(1);
                camera.speed_y = n(2);
            }
            sync_camera_to_views(static_cast<int>(n(0)));
            return std::monostate{};
        }
        if (name == "view_get_visible" || name == "view_get_xport" || name == "view_get_yport" ||
            name == "view_get_wport" || name == "view_get_hport" || name == "view_get_surface_id") {
            const int view = view_index();
            const std::string_view field =
                name == "view_get_visible"     ? "view_visible"
                : name == "view_get_xport"     ? "view_xport"
                : name == "view_get_yport"     ? "view_yport"
                : name == "view_get_wport"     ? "view_wport"
                : name == "view_get_hport"     ? "view_hport"
                                              : "view_surface_id";
            return view_value(field, view, std::int32_t{field == "view_surface_id" ? -1 : 0});
        }
        if (name == "view_set_visible" || name == "view_set_xport" || name == "view_set_yport" ||
            name == "view_set_wport" || name == "view_set_hport" || name == "view_set_surface_id") {
            const int view = view_index();
            const std::string_view field =
                name == "view_set_visible"     ? "view_visible"
                : name == "view_set_xport"     ? "view_xport"
                : name == "view_set_yport"     ? "view_yport"
                : name == "view_set_wport"     ? "view_wport"
                : name == "view_set_hport"     ? "view_hport"
                                              : "view_surface_id";
            engine[std::string(field) + view_suffix(view)] = n(1);
            return std::monostate{};
        }
        if (name == "draw_set_valign") {
            valign = static_cast<int>(n(0));
            return std::monostate{};
        }
        if (name == "draw_get_valign")
            return std::int32_t{valign};
        if (name == "draw_text") {
            std::string storage;
            assets.draw_text(canvas, draw_font, n(0), n(1), text_view(2, storage), draw_color, draw_alpha,
                             halign, valign);
            return std::monostate{};
        }
        if (name == "draw_text_color" || name == "draw_text_colour") {
            const unsigned color = static_cast<unsigned>((n(3) + n(4) + n(5) + n(6)) / 4);
            std::string storage;
            assets.draw_text(canvas, draw_font, n(0), n(1), text_view(2, storage), color, n(7), halign, valign);
            return std::monostate{};
        }
        if (name == "draw_text_transformed") {
            std::string storage;
            assets.draw_text(canvas, draw_font, n(0), n(1), text_view(2, storage), draw_color, draw_alpha,
                             halign, valign, n(3), n(4), n(5));
            return std::monostate{};
        }
        if (name == "draw_text_transformed_color") {
            std::string storage;
            assets.draw_text(canvas, draw_font, n(0), n(1), text_view(2, storage), static_cast<unsigned>(n(6)),
                             draw_alpha, halign, valign, n(3), n(4), n(5));
            return std::monostate{};
        }
        if (name == "draw_text_ext" || name == "draw_text_ext_transformed") {
            std::string storage;
            const auto value = text_view(2, storage);
            const bool transformed = name == "draw_text_ext_transformed";
            const double line_height = n(3) < 0 ? assets.text_height(draw_font) : n(3);
            const double max_width = n(4);
            const double sx = transformed ? n(5) : 1;
            const double sy = transformed ? n(6) : 1;
            const double angle = transformed ? n(7) : 0;
            const double radians = angle * 3.14159265358979323846 / 180.0;
            const double cs = angle == 0 ? 1 : std::cos(radians);
            const double sn = angle == 0 ? 0 : std::sin(radians);
            std::size_t line_start = 0;
            double line_width = 0;
            std::size_t line_number = 0;
            auto draw_line = [&](std::size_t line_end) {
                const double line_offset = line_height * static_cast<double>(line_number);
                const double x = n(0) + (transformed ? line_offset * sy * sn : 0);
                const double y = n(1) + (transformed ? line_offset * sy * cs : line_offset);
                assets.draw_text(canvas, draw_font, x, y, value.substr(line_start, line_end - line_start),
                                 draw_color, draw_alpha, halign, valign, sx, sy, angle);
                line_start = line_end;
                line_width = 0;
                ++line_number;
            };
            for (std::size_t index = 0; index < value.size(); ++index) {
                const char ch = value[index];
                const double char_width = ch == '\n' || ch == '#'
                                              ? 0
                                              : assets.text_character_width(
                                                    draw_font, static_cast<unsigned char>(ch));
                if ((ch == ' ' && max_width > 0 && line_width + char_width > max_width) || ch == '\n' ||
                    ch == '#') {
                    draw_line(index);
                    if (ch == '\n' || ch == '#') {
                        line_start = index + 1;
                        continue;
                    }
                }
                line_width += char_width;
            }
            draw_line(value.size());
            return std::monostate{};
        }
        if (name == "surface_create") {
            const int width = static_cast<int>(std::clamp(n(0), 1.0, 8192.0));
            const int height = static_cast<int>(std::clamp(n(1), 1.0, 8192.0));
            if (static_cast<std::uint64_t>(width) * height > 64ULL * 1024 * 1024)
                return std::int32_t{-1};
            const int id = next_surface++;
            surfaces.emplace(std::piecewise_construct, std::forward_as_tuple(id),
                             std::forward_as_tuple(width, height));
            surfaces.at(id).clear_transparent();
            surfaces.at(id).color_write_enable(color_write_enable[0], color_write_enable[1],
                                               color_write_enable[2], color_write_enable[3]);
            surfaces.at(id).blend_enable(blend_enabled);
            surfaces.at(id).alpha_test_enable(alpha_test_enabled);
            surfaces.at(id).alpha_test_ref(alpha_test_reference);
            return std::int32_t{id};
        }
        if (name == "surface_resize") {
            const int id = static_cast<int>(n(0));
            const int width = static_cast<int>(std::clamp(n(1), 1.0, 4096.0));
            const int height = static_cast<int>(std::clamp(n(2), 1.0, 4096.0));
            if (static_cast<std::uint64_t>(width) * height > 64ULL * 1024 * 1024)
                return std::monostate{};
            if (id == 0) {
                this->canvas = Canvas(width, height, true);
                this->canvas.color_write_enable(color_write_enable[0], color_write_enable[1],
                                                color_write_enable[2], color_write_enable[3]);
                this->canvas.blend_enable(blend_enabled);
                this->canvas.alpha_test_enable(alpha_test_enabled);
                this->canvas.alpha_test_ref(alpha_test_reference);
            } else if (surfaces.contains(id)) {
                surfaces.at(id) = Canvas(width, height);
                surfaces.at(id).clear_transparent();
                surfaces.at(id).color_write_enable(color_write_enable[0], color_write_enable[1],
                                                   color_write_enable[2], color_write_enable[3]);
                surfaces.at(id).blend_enable(blend_enabled);
                surfaces.at(id).alpha_test_enable(alpha_test_enabled);
                surfaces.at(id).alpha_test_ref(alpha_test_reference);
            }
            return std::monostate{};
        }
        if (name == "surface_exists") {
            const int id = static_cast<int>(n(0));
            return id == 0 || surfaces.contains(id);
        }
        if (name == "surface_get_width" || name == "surface_get_height") {
            const int id = static_cast<int>(n(0));
            if (id == 0)
                return std::int32_t{name == "surface_get_width" ? this->canvas.logical_width()
                                                                : this->canvas.logical_height()};
            if (!surfaces.contains(id))
                return std::int32_t{-1};
            const auto &surface = surfaces.at(id);
            return std::int32_t{name == "surface_get_width" ? surface.width() : surface.height()};
        }
        if (name == "surface_set_target") {
            const int id = static_cast<int>(n(0));
            if (!surfaces.contains(id))
                return false;
            surface_target = id;
            return true;
        }
        if (name == "surface_reset_target") {
            surface_target = 0;
            return std::monostate{};
        }
        if (name == "surface_free") {
            const int id = static_cast<int>(n(0));
            if (surface_target == id)
                surface_target = 0;
            surfaces.erase(id);
            return std::monostate{};
        }
        if (name == "draw_surface" || name == "draw_surface_ext") {
            const int id = static_cast<int>(n(0));
            if (!surfaces.contains(id))
                return std::monostate{};
            const Canvas source = surfaces.at(id);
            const bool extended = name == "draw_surface_ext";
            canvas.draw_surface(source, n(1), n(2), extended ? n(3) : 1, extended ? n(4) : 1,
                                extended ? n(5) : 0, extended ? static_cast<unsigned>(n(6)) : 0xffffff,
                                extended ? n(7) : 1);
            return std::monostate{};
        }
        if (name == "draw_surface_tiled_ext") {
            const int id = static_cast<int>(n(0));
            if (!surfaces.contains(id))
                return std::monostate{};
            const Canvas source = surfaces.at(id);
            const double sx = n(3), sy = n(4);
            const double tile_width = source.width() * std::abs(sx);
            const double tile_height = source.height() * std::abs(sy);
            if (!std::isfinite(tile_width + tile_height) || tile_width <= 0 || tile_height <= 0)
                return std::monostate{};
            const double x = n(1), y = n(2);
            const double start_x = x + std::floor(-x / tile_width) * tile_width;
            const double start_y = y + std::floor(-y / tile_height) * tile_height;
            const double columns = std::ceil((canvas.logical_width() - start_x) / tile_width);
            const double rows = std::ceil((canvas.logical_height() - start_y) / tile_height);
            if (columns > 0 && rows > 0 && columns * rows > 1'000'000)
                throw Error(ErrorKind::vm, "tiled surface draw limit exceeded");
            for (double tile_y = start_y; tile_y < canvas.logical_height(); tile_y += tile_height)
                for (double tile_x = start_x; tile_x < canvas.logical_width(); tile_x += tile_width)
                    canvas.draw_surface(source, tile_x, tile_y, sx, sy, 0,
                                        static_cast<unsigned>(n(5)), n(6));
            return std::monostate{};
        }
        if (name == "string_width")
            return assets.text_width(draw_font, text(0));
        if (name == "string_height")
            return assets.text_height(draw_font);
        auto average_color_arguments = [&](std::size_t first, std::size_t count) {
            std::array<unsigned, 3> channels{};
            for (std::size_t i = 0; i < count; ++i) {
                const unsigned color = static_cast<unsigned>(n(first + i));
                channels[0] += (color >> 16) & 255;
                channels[1] += (color >> 8) & 255;
                channels[2] += color & 255;
            }
            return ((channels[0] / count) << 16) | ((channels[1] / count) << 8) | (channels[2] / count);
        };
        if (name == "draw_rectangle") {
            assets.rectangle(canvas, n(0), n(1), n(2), n(3), n(4) != 0);
            return std::monostate{};
        }
        if (name == "ossafe_fill_rectangle") {
            assets.rectangle(canvas, n(0), n(1), n(2), n(3), false);
            return std::monostate{};
        }
        if (name == "draw_circle" || name == "draw_circle_color" || name == "draw_circle_colour") {
            const bool colored = name != "draw_circle";
            if (colored) {
                draw_color = average_color_arguments(3, 2);
                canvas.color(draw_color);
            }
            canvas.circle(n(0), n(1), n(2), n(colored ? 5 : 3) != 0);
            return std::monostate{};
        }
        if (name == "draw_line" || name == "draw_line_width" || name == "draw_line_color" ||
            name == "draw_line_width_color" || name == "draw_line_colour" ||
            name == "draw_line_width_colour") {
            const bool thick = name == "draw_line_width" || name == "draw_line_width_color" ||
                              name == "draw_line_width_colour";
            const bool colored = name == "draw_line_color" || name == "draw_line_width_color" ||
                                 name == "draw_line_colour" || name == "draw_line_width_colour";
            if (colored) {
                draw_color = average_color_arguments(thick ? 5 : 4, 2);
                canvas.color(draw_color);
            }
            canvas.line(n(0), n(1), n(2), n(3), thick ? n(4) : 1);
            return std::monostate{};
        }
        if (name == "draw_triangle" || name == "draw_triangle_color" || name == "draw_triangle_colour") {
            const bool colored = name != "draw_triangle";
            const unsigned outline_index = colored ? 9U : 6U;
            if (colored) {
                draw_color = average_color_arguments(6, 3);
                canvas.color(draw_color);
            }
            canvas.triangle(n(0), n(1), n(2), n(3), n(4), n(5), n(outline_index) != 0);
            return std::monostate{};
        }
        if (name == "draw_ellipse" || name == "draw_ellipse_color" || name == "draw_ellipse_colour") {
            const bool colored = name != "draw_ellipse";
            if (colored) {
                draw_color = average_color_arguments(4, 2);
                canvas.color(draw_color);
            }
            canvas.ellipse(n(0), n(1), n(2), n(3), n(colored ? 6 : 4) != 0);
            return std::monostate{};
        }
        if (name == "draw_roundrect" || name == "draw_roundrect_color" || name == "draw_roundrect_colour" ||
            name == "draw_roundrect_ext" || name == "draw_roundrect_color_ext" ||
            name == "draw_roundrect_colour_ext") {
            const bool colored = name == "draw_roundrect_color" || name == "draw_roundrect_colour" ||
                                 name == "draw_roundrect_color_ext" || name == "draw_roundrect_colour_ext";
            const bool extended = name == "draw_roundrect_ext" || name == "draw_roundrect_color_ext" ||
                                  name == "draw_roundrect_colour_ext";
            const bool outline = n(extended ? (colored ? 8 : 6) : (colored ? 6 : 4)) != 0;
            if (colored) {
                draw_color = average_color_arguments(extended ? 6 : 4, 2);
                canvas.color(draw_color);
            }
            const double width = std::abs(n(2) - n(0)), height = std::abs(n(3) - n(1));
            const double radius_x = extended ? n(4) : std::min(10.0, width / 4);
            const double radius_y = extended ? n(5) : std::min(10.0, height / 4);
            canvas.round_rectangle(n(0), n(1), n(2), n(3), radius_x, radius_y, outline);
            return std::monostate{};
        }
        if (name == "draw_rectangle_color" || name == "draw_rectangle_colour") {
            draw_color = average_color_arguments(4, 4);
            canvas.color(draw_color);
            assets.rectangle(canvas, n(0), n(1), n(2), n(3), n(8) != 0);
            return std::monostate{};
        }
        if (name == "draw_point" || name == "draw_point_color" || name == "draw_point_colour") {
            if (name != "draw_point") {
                draw_color = average_color_arguments(2, 2);
                canvas.color(draw_color);
            }
            canvas.line(n(0), n(1), n(0), n(1));
            return std::monostate{};
        }
        if (name == "draw_arrow") {
            const double x1 = n(0), y1 = n(1), x2 = n(2), y2 = n(3);
            const double size = std::max(0.0, n(4));
            canvas.line(x1, y1, x2, y2);
            const double angle = std::atan2(y2 - y1, x2 - x1);
            const double wing = size * 0.55;
            constexpr double spread = 0.55;
            canvas.line(x2, y2, x2 - size * std::cos(angle - spread), y2 - size * std::sin(angle - spread), wing);
            canvas.line(x2, y2, x2 - size * std::cos(angle + spread), y2 - size * std::sin(angle + spread), wing);
            return std::monostate{};
        }
        if (name == "draw_path") {
            const int path_id = static_cast<int>(n(0));
            if (path_id < 0 || static_cast<std::size_t>(path_id) >= paths.size())
                throw Error(ErrorKind::vm, "path ID out of range");
            const auto &path = paths[static_cast<std::size_t>(path_id)];
            if (path.points.size() < 2 || path.length <= 0)
                return std::monostate{};
            const double x = n(1), y = n(2);
            const double end = std::clamp(n(3), 0.0, 1.0);
            const bool reverse = n(4) != 0;
            double remaining = path.length * end;
            const std::size_t segment_count = path.points.size() - 1;
            for (std::size_t step = 0; step < segment_count && remaining > 0; ++step) {
                const std::size_t first = reverse ? segment_count - step : step;
                const std::size_t second = reverse ? first - 1 : first + 1;
                const auto &a = path.points[first];
                const auto &b = path.points[second];
                const double length = std::hypot(b.x - a.x, b.y - a.y);
                if (length <= 0)
                    continue;
                const double fraction = std::min(1.0, remaining / length);
                canvas.line(x + a.x, y + a.y, x + a.x + (b.x - a.x) * fraction,
                            y + a.y + (b.y - a.y) * fraction);
                remaining -= length;
            }
            return std::monostate{};
        }
        if (name == "gpu_set_colorwriteenable" || name == "gpu_set_colourwriteenable") {
            auto enabled = std::array<bool, 4>{true, true, true, true};
            if (!args.empty()) {
                if (const auto *array = std::get_if<ArrayPtr>(&args[0]); array && *array) {
                    for (std::size_t i = 0; i < enabled.size(); ++i) {
                        const auto *value = (*array)->elements.find(static_cast<std::int32_t>(i));
                        if (value)
                            enabled[i] = value_number(*value) != 0;
                    }
                } else {
                    for (std::size_t i = 0; i < std::min(enabled.size(), args.size()); ++i)
                        enabled[i] = value_number(args[i]) != 0;
                }
            }
            color_write_enable = enabled;
            this->canvas.color_write_enable(enabled[0], enabled[1], enabled[2], enabled[3]);
            for (auto &[id, surface] : surfaces) {
                (void)id;
                surface.color_write_enable(enabled[0], enabled[1], enabled[2], enabled[3]);
            }
            return std::monostate{};
        }
        if (name == "gpu_get_colorwriteenable" || name == "gpu_get_colourwriteenable") {
            ArrayPtr result(new ArrayValue);
            result->row_lengths[0] = 4;
            for (std::size_t i = 0; i < color_write_enable.size(); ++i)
                result->elements[static_cast<std::int32_t>(i)] = color_write_enable[i];
            return result;
        }
        if (name == "gpu_set_blendenable" || name == "draw_enable_alphablend") {
            blend_enabled = !args.empty() && value_number(args[0]) != 0;
            this->canvas.blend_enable(blend_enabled);
            for (auto &[id, surface] : surfaces) {
                (void)id;
                surface.blend_enable(blend_enabled);
            }
            return std::monostate{};
        }
        if (name == "gpu_get_blendenable")
            return blend_enabled;
        if (name == "gpu_set_alphatestenable") {
            alpha_test_enabled = !args.empty() && value_number(args[0]) != 0;
            this->canvas.alpha_test_enable(alpha_test_enabled);
            for (auto &[id, surface] : surfaces) {
                (void)id;
                surface.alpha_test_enable(alpha_test_enabled);
            }
            return std::monostate{};
        }
        if (name == "gpu_get_alphatestenable")
            return alpha_test_enabled;
        if (name == "gpu_set_alphatestref") {
            const double reference = args.empty() ? 0 : value_number(args[0]);
            alpha_test_reference = static_cast<unsigned>(std::clamp(reference, 0.0, 255.0));
            this->canvas.alpha_test_ref(alpha_test_reference);
            for (auto &[id, surface] : surfaces) {
                (void)id;
                surface.alpha_test_ref(alpha_test_reference);
            }
            return std::monostate{};
        }
        if (name == "gpu_get_alphatestref")
            return static_cast<double>(alpha_test_reference);
        if (name == "gpu_set_fog" || name == "gpu_set_texfilter" || name == "draw_set_circle_precision" ||
            name == "gpu_set_blendmode" || name == "gpu_set_blendmode_ext" ||
            name == "gpu_set_blendmode_ext_sepalpha")
            return std::monostate{};
        if (name == "draw_clear_alpha") {
            canvas.clear(static_cast<unsigned>(n(0)));
            return std::monostate{};
        }
        if (name == "draw_clear") {
            canvas.clear(static_cast<unsigned>(n(0)));
            return std::monostate{};
        }
        if (name == "draw_background") {
            assets.draw_background(canvas, static_cast<int>(n(0)), n(1), n(2));
            return std::monostate{};
        }
        if (name == "draw_background_stretched") {
            assets.draw_background_stretched(canvas, static_cast<int>(n(0)), n(1), n(2), n(3), n(4));
            return std::monostate{};
        }
        if (name == "draw_background_part_ext") {
            assets.draw_background_part_ext(canvas, static_cast<int>(n(0)), n(1), n(2), n(3), n(4), n(5),
                                            n(6), n(7), n(8), static_cast<unsigned>(n(9)), n(10));
            return std::monostate{};
        }
        if (name == "draw_sprite_tiled_ext") {
            const int sprite = static_cast<int>(n(0));
            const auto &info = assets.sprite(sprite);
            const double frame_index = n(1), x = n(2), y = n(3), sx = n(4), sy = n(5);
            const double tile_width = info.width * std::abs(sx);
            const double tile_height = info.height * std::abs(sy);
            if (!std::isfinite(tile_width + tile_height + x + y) || tile_width <= 0 || tile_height <= 0)
                return std::monostate{};
            const double width = canvas.logical_width(), height = canvas.logical_height();
            const double start_x = x + std::floor(-x / tile_width) * tile_width;
            const double start_y = y + std::floor(-y / tile_height) * tile_height;
            const double columns = std::ceil((width - start_x) / tile_width);
            const double rows = std::ceil((height - start_y) / tile_height);
            if (columns > 0 && rows > 0 && columns * rows > 1'000'000)
                throw Error(ErrorKind::vm, "tiled sprite draw limit exceeded");
            for (double tile_y = start_y; tile_y < height; tile_y += tile_height)
                for (double tile_x = start_x; tile_x < width; tile_x += tile_width)
                    assets.draw_sprite(canvas, sprite, frame_index, tile_x, tile_y, sx, sy, 0,
                                       static_cast<unsigned>(n(6)), n(7));
            return std::monostate{};
        }
        if (name == "draw_sprite") {
            const auto f = n(1) < 0 ? value_number(self->vars.at("image_index")) : n(1);
            assets.draw_sprite(canvas, static_cast<int>(n(0)), f, n(2), n(3), 1, 1, 0, 0xffffff, draw_alpha);
            return std::monostate{};
        }
        if (name == "draw_sprite_ext") {
            assets.draw_sprite(canvas, static_cast<int>(n(0)),
                               n(1) < 0 ? value_number(self->vars.at("image_index")) : n(1), n(2), n(3), n(4),
                               n(5), n(6), static_cast<unsigned>(n(7)), n(8));
            return std::monostate{};
        }
        if (name == "draw_sprite_stretched") {
            const auto sprite = static_cast<int>(n(0));
            const auto &info = assets.sprite(sprite);
            const double sx = info.width ? n(4) / info.width : 1, sy = info.height ? n(5) / info.height : 1;
            assets.draw_sprite(canvas, sprite, n(1) < 0 ? value_number(self->vars.at("image_index")) : n(1),
                               n(2), n(3), sx, sy, 0, 0xffffff, draw_alpha);
            return std::monostate{};
        }
        if (name == "draw_self") {
            const int sprite = static_cast<int>(value_number(self->vars.at("sprite_index")));
            if (sprite >= 0)
                assets.draw_sprite(canvas, sprite, value_number(self->vars.at("image_index")),
                                   value_number(self->vars.at("x")), value_number(self->vars.at("y")),
                                   value_number(self->vars.at("image_xscale")),
                                   value_number(self->vars.at("image_yscale")),
                                   value_number(self->vars.at("image_angle")),
                                   static_cast<unsigned>(value_number(self->vars.at("image_blend"))),
                                   value_number(self->vars.at("image_alpha")));
            return std::monostate{};
        }
        if (name == "draw_getpixel") {
            const int x = static_cast<int>(n(0)), y = static_cast<int>(n(1));
            if (x < 0 || y < 0 || x >= canvas.logical_width() || y >= canvas.logical_height())
                return std::int32_t{-1};
            const auto pixel = canvas.pixel(static_cast<int>(x * canvas.render_scale()),
                                            static_cast<int>(y * canvas.render_scale()));
            return std::int32_t{
                static_cast<int>(((pixel >> 16) & 0xff) | (pixel & 0xff00) | ((pixel & 0xff) << 16))};
        }
        if (name == "draw_sprite_part") {
            assets.draw_sprite_part(canvas, static_cast<int>(n(0)),
                                    n(1) < 0 ? value_number(self->vars.at("image_index")) : n(1), n(2), n(3),
                                    n(4), n(5), n(6), n(7), 1, 1, 0xffffff, draw_alpha);
            return std::monostate{};
        }
        if (name == "draw_sprite_part_ext") {
            assets.draw_sprite_part(canvas, static_cast<int>(n(0)),
                                    n(1) < 0 ? value_number(self->vars.at("image_index")) : n(1), n(2), n(3),
                                    n(4), n(5), n(6), n(7), n(8), n(9), static_cast<unsigned>(n(10)), n(11));
            return std::monostate{};
        }
        if (name == "collision_line") {
            const double x = n(0), y = n(1), dx = n(2) - x, dy = n(3) - y;
            const bool exclude = n(6) != 0;
            for (const auto &i : targets(static_cast<int>(n(4)), self, other)) {
                if (exclude && i == self)
                    continue;
                const auto b = bounds(*i);
                if (!b.valid)
                    continue;
                double lo = 0, hi = 1;
                auto clip = [&](double origin, double delta, double low, double high) {
                    if (delta == 0)
                        return origin >= low && origin < high;
                    double a = (low - origin) / delta, z = (high - origin) / delta;
                    if (a > z)
                        std::swap(a, z);
                    lo = std::max(lo, a);
                    hi = std::min(hi, z);
                    return lo <= hi;
                };
                if (clip(x, dx, b.left, b.right) && clip(y, dy, b.top, b.bottom))
                    return std::int32_t{i->id};
            }
            return std::int32_t{-4};
        }
        if (name == "collision_point" || name == "collision_rectangle") {
            const bool point = name == "collision_point";
            const double x1 = n(0), y1 = n(1), x2 = point ? x1 : n(2), y2 = point ? y1 : n(3);
            const auto object = static_cast<int>(n(point ? 2 : 4));
            const bool exclude = n(point ? 4 : 6) != 0;
            for (const auto &i : targets(object, self, other)) {
                if (exclude && i == self)
                    continue;
                const auto b = bounds(*i);
                if (!b.valid)
                    continue;
                if (point ? (x1 >= b.left && x1 < b.right && y1 >= b.top && y1 < b.bottom)
                          : (std::max(x1, x2) >= b.left && std::min(x1, x2) < b.right &&
                             std::max(y1, y2) >= b.top && std::min(y1, y2) < b.bottom))
                    return std::int32_t{i->id};
            }
            return std::int32_t{-4};
        }
        if (name == "place_meeting" || name == "instance_place") {
            for (const auto &i : targets(static_cast<int>(n(2)), self, other))
                if (i != self) {
                    if (collides(*self, n(0), n(1), *i))
                        return name == "place_meeting" ? Value{true} : Value{std::int32_t{i->id}};
                }
            return name == "place_meeting" ? Value{false} : Value{std::int32_t{-4}};
        }
        if (name == "keyboard_clear") {
            const auto key = static_cast<int>(n(0));
            if (key >= 0 && key < 256) {
                const auto k = static_cast<std::size_t>(key);
                held[k] = pressed[k] = released[k] = false;
            }
            return 0.0;
        }
        if (name == "action_move") {
            const auto directions = text(0);
            if (directions.size() != 9)
                throw Error(ErrorKind::vm, "action_move requires nine directions");
            std::vector<int> choices;
            for (int k = 0; k < 9; ++k)
                if (directions[static_cast<std::size_t>(k)] == '1')
                    choices.push_back(k);
            if (choices.empty())
                return 0.0;
            const int chosen =
                choices[std::uniform_int_distribution<std::size_t>(0, choices.size() - 1)(random)];
            const double angles[] = {225, 270, 315, 180, 0, 0, 135, 90, 45};
            const double speed = chosen == 4 ? 0 : n(1), angle = angles[chosen],
                         rad = angle * 3.141592653589793 / 180;
            self->vars["speed"] = speed;
            self->vars["direction"] = angle;
            self->vars["hspeed"] = speed * std::cos(rad);
            self->vars["vspeed"] = -speed * std::sin(rad);
            return 0.0;
        }
        if (name == "action_move_point") {
            double target_x = n(0), target_y = n(1);
            if (auto it = globals.find("__argument_relative");
                it != globals.end() && value_number(it->second) != 0) {
                target_x += value_number(self->vars.at("x"));
                target_y += value_number(self->vars.at("y"));
            }
            const double dx = target_x - value_number(self->vars.at("x"));
            const double dy = target_y - value_number(self->vars.at("y"));
            const double length = std::hypot(dx, dy), speed = n(2);
            self->vars["speed"] = speed;
            if (length == 0) {
                self->vars["hspeed"] = 0.0;
                self->vars["vspeed"] = 0.0;
            } else {
                self->vars["direction"] = std::fmod(std::atan2(-dy, dx) * 180 / 3.141592653589793 + 360, 360);
                self->vars["hspeed"] = dx * speed / length;
                self->vars["vspeed"] = dy * speed / length;
            }
            return std::monostate{};
        }
        if (name == "action_set_motion") {
            double direction = n(0), speed = n(1);
            if (auto it = globals.find("__argument_relative");
                it != globals.end() && value_number(it->second) != 0) {
                direction += value_number(self->vars.at("direction"));
                speed += value_number(self->vars.at("speed"));
            }
            direction = std::fmod(std::fmod(direction, 360.0) + 360.0, 360.0);
            const double radians = direction * 3.141592653589793 / 180;
            self->vars["direction"] = direction;
            self->vars["speed"] = speed;
            self->vars["hspeed"] = speed * std::cos(radians);
            self->vars["vspeed"] = -speed * std::sin(radians);
            return std::monostate{};
        }
        if (name == "action_set_alarm") {
            const int alarm = static_cast<int>(n(1));
            if (alarm < 0 || alarm >= 12)
                throw Error(ErrorKind::vm, "alarm index out of range");
            self->vars["alarm[" + std::to_string(alarm) + "]"] = std::int32_t{static_cast<int>(n(0))};
            return 0.0;
        }
        if (name == "action_set_friction") {
            double value = n(0);
            if (auto it = globals.find("__argument_relative");
                it != globals.end() && value_number(it->second) != 0)
                value += value_number(self->vars.at("friction"));
            self->vars["friction"] = value;
            return 0.0;
        }
        if (name == "action_set_gravity") {
            double direction = n(0), gravity = n(1);
            if (auto it = globals.find("__argument_relative");
                it != globals.end() && value_number(it->second) != 0) {
                direction += value_number(self->vars.at("gravity_direction"));
                gravity += value_number(self->vars.at("gravity"));
            }
            self->vars["gravity_direction"] = direction;
            self->vars["gravity"] = gravity;
            return std::monostate{};
        }
        if (name == "action_set_hspeed" || name == "action_set_vspeed") {
            const auto component = name == "action_set_hspeed" ? "hspeed" : "vspeed";
            double value = n(0);
            if (auto it = globals.find("__argument_relative");
                it != globals.end() && value_number(it->second) != 0)
                value += value_number(self->vars.at(component));
            set_velocity(component, value);
            return std::monostate{};
        }
        if (name == "action_set_relative") {
            globals["__argument_relative"] = n(0) != 0;
            return std::monostate{};
        }
        if (name == "path_end") {
            self->vars["path_index"] = std::int32_t{-1};
            return std::monostate{};
        }
        if (name == "path_start") {
            const int id = number(args[0]);
            if (id < 0 || static_cast<std::size_t>(id) >= paths.size())
                throw Error(ErrorKind::vm, "invalid path ID");
            const auto &path = paths[static_cast<std::size_t>(id)];
            if (path.points.empty())
                return std::monostate{};
            const auto &first = path.points.front();
            self->vars["path_index"] = std::int32_t{id};
            self->vars["path_speed"] = n(1);
            self->vars["path_position"] = n(1) < 0 ? 1.0 : 0.0;
            self->vars["path_endaction"] = args[2];
            self->vars["path_scale"] = 1.0;
            self->vars["path_orientation"] = 0.0;
            self->vars["path_xstart"] = n(3) != 0 ? 0.0 : value_number(self->vars.at("x")) - first.x;
            self->vars["path_ystart"] = n(3) != 0 ? 0.0 : value_number(self->vars.at("y")) - first.y;
            return std::monostate{};
        }
        if (name == "move_towards_point") {
            const double dx = n(0) - value_number(self->vars.at("x")),
                         dy = n(1) - value_number(self->vars.at("y")), length = std::hypot(dx, dy),
                         speed = n(2);
            self->vars["speed"] = speed;
            self->vars["direction"] = std::fmod(std::atan2(-dy, dx) * 180 / 3.141592653589793 + 360, 360);
            self->vars["hspeed"] = length != 0 ? dx * speed / length : 0;
            self->vars["vspeed"] = length != 0 ? dy * speed / length : 0;
            return 0.0;
        }
        if (name == "distance_to_point") {
            const auto b = bounds(*self);
            return std::hypot(std::max({b.left - n(0), 0.0, n(0) - b.right}),
                              std::max({b.top - n(1), 0.0, n(1) - b.bottom}));
        }
        if (name == "point_distance")
            return std::hypot(n(2) - n(0), n(3) - n(1));
        if (name == "point_direction")
            return std::fmod(std::atan2(n(1) - n(3), n(2) - n(0)) * 180 / 3.141592653589793 + 360, 360);
        if (name == "lengthdir_x")
            return n(0) * std::cos(n(1) * 3.141592653589793 / 180);
        if (name == "lengthdir_y")
            return -n(0) * std::sin(n(1) * 3.141592653589793 / 180);
        if (name == "degtorad")
            return n(0) * 3.141592653589793 / 180;
        if (name == "darctan2")
            return std::atan2(n(0), n(1)) * 180 / 3.141592653589793;
        if (name == "make_color_rgb") {
            const auto channel = [&](std::size_t i) {
                return static_cast<unsigned>(std::clamp(n(i), 0.0, 255.0));
            };
            return std::int32_t{static_cast<int>((channel(2) << 16) | (channel(1) << 8) | channel(0))};
        }
        if (name == "make_color_hsv") {
            const double hue = std::fmod(std::fmod(n(0), 256.0) + 256.0, 256.0) * 6 / 256;
            const double saturation = std::clamp(n(1), 0.0, 255.0) / 255;
            const double value = std::clamp(n(2), 0.0, 255.0) / 255;
            const double chroma = value * saturation, x = chroma * (1 - std::abs(std::fmod(hue, 2.0) - 1));
            double r = 0, g = 0, b = 0;
            if (hue < 1) {
                r = chroma;
                g = x;
            } else if (hue < 2) {
                r = x;
                g = chroma;
            } else if (hue < 3) {
                g = chroma;
                b = x;
            } else if (hue < 4) {
                g = x;
                b = chroma;
            } else if (hue < 5) {
                r = x;
                b = chroma;
            } else {
                r = chroma;
                b = x;
            }
            const auto channel = [&](double c) {
                return static_cast<unsigned>(std::lround((c + value - chroma) * 255));
            };
            return std::int32_t{static_cast<int>((channel(b) << 16) | (channel(g) << 8) | channel(r))};
        }
        if (name == "merge_color") {
            const auto first = static_cast<unsigned>(n(0)), second = static_cast<unsigned>(n(1));
            const double t = std::clamp(n(2), 0.0, 1.0);
            unsigned result = 0;
            for (unsigned shift : {0U, 8U, 16U}) {
                const double a = static_cast<double>((first >> shift) & 255), b = (second >> shift) & 255;
                result |= static_cast<unsigned>(std::lround(a + (b - a) * t)) << shift;
            }
            return std::int32_t{static_cast<int>(result)};
        }
        if (name == "instance_position") {
            const double x = n(0), y = n(1);
            for (const auto &i : targets(static_cast<int>(n(2)), self, other)) {
                if (i == self)
                    continue;
                const auto b = bounds(*i);
                if (b.valid && x >= b.left && x < b.right && y >= b.top && y < b.bottom)
                    return std::int32_t{i->id};
            }
            return std::int32_t{-4};
        }
        if (name == "distance_to_object") {
            const auto source = bounds(*self);
            double nearest = std::numeric_limits<double>::infinity();
            for (const auto &i : targets(static_cast<int>(n(0)), self, other)) {
                if (i == self)
                    continue;
                const auto target = bounds(*i);
                if (source.valid && target.valid)
                    nearest = std::min(
                        nearest,
                        std::hypot(std::max({target.left - source.right, source.left - target.right, 0.0}),
                                   std::max({target.top - source.bottom, source.top - target.bottom, 0.0})));
            }
            return std::isfinite(nearest) ? nearest : -1.0;
        }
        if (name == "collision_circle") {
            const double x = n(0), y = n(1), radius = std::max(0.0, n(2));
            const bool not_me = args.size() > 5 && n(5) != 0;
            for (const auto &i : targets(static_cast<int>(n(3)), self, other)) {
                if ((not_me && i == self) || i->id == -4)
                    continue;
                const auto b = bounds(*i);
                if (!b.valid)
                    continue;
                const double cx = std::clamp(x, b.left, b.right), cy = std::clamp(y, b.top, b.bottom);
                if ((cx - x) * (cx - x) + (cy - y) * (cy - y) <= radius * radius)
                    return std::int32_t{i->id};
            }
            return std::int32_t{-4};
        }
        if (name == "move_snap") {
            const double sx = n(0), sy = n(1);
            if (sx > 0)
                self->vars["x"] = std::round(value_number(self->vars.at("x")) / sx) * sx;
            if (sy > 0)
                self->vars["y"] = std::round(value_number(self->vars.at("y")) / sy) * sy;
            return std::monostate{};
        }
        if (name == "instance_exists")
            return first_target(static_cast<int>(n(0)), self, other) != nullptr;
        if (name == "event_inherited") {
            if (event_stack.empty())
                throw Error(ErrorKind::vm, "event_inherited outside an event");
            const auto f = event_stack.back();
            const auto parent = objects.at(static_cast<std::size_t>(f.object)).parent;
            if (parent >= 0)
                event(self, f.type, f.subtype, parent, other);
            return std::monostate{};
        }
        if (name == "event_perform") {
            const int type = static_cast<int>(n(0)), subtype = static_cast<int>(n(1));
            if (type < 0 || type > 12)
                throw Error(ErrorKind::vm, "event_perform event type out of range");
            event(self, type, subtype);
            return std::monostate{};
        }
        if (name == "event_user") {
            event(self, 7, 10 + static_cast<int>(n(0)), -1, other);
            return std::monostate{};
        }
        if (name == "action_move_to") {
            self->vars["x"] = n(0);
            self->vars["y"] = n(1);
            return std::monostate{};
        }
        if (name == "instance_number") {
            const int target = static_cast<int>(n(0));
            if (target == -1 || target == -2)
                return first_target(target, self, other) ? 1.0 : 0.0;
            std::size_t count = 0;
            for (const auto &i : instances)
                if (i->alive && !i->deactivated && matches(*i, target))
                    ++count;
            return static_cast<double>(count);
        }
        if (name == "instance_find") {
            const int target = static_cast<int>(n(0));
            auto index = static_cast<std::size_t>(n(1));
            if (target == -1 || target == -2) {
                const auto *found = index == 0 ? first_target(target, self, other) : nullptr;
                return std::int32_t{found ? found->id : -4};
            }
            for (const auto &i : instances)
                if (i->alive && !i->deactivated && matches(*i, target)) {
                    if (index == 0)
                        return std::int32_t{i->id};
                    --index;
                }
            return std::int32_t{-4};
        }
        if (name == "instance_create") {
            Placement p;
            p.x = n(0);
            p.y = n(1);
            p.object = static_cast<int>(n(2));
            p.id = next_id++;
            const auto i = make(p);
            event(i, 14, 0);
            event(i, 0, 0);
            return std::int32_t{i->id};
        }
        if (name == "instance_create_depth") {
            Placement p;
            p.x = n(0);
            p.y = n(1);
            const auto depth = n(2);
            p.object = static_cast<int>(n(3));
            p.id = next_id++;
            const auto i = make(p);
            i->vars["depth"] = depth;
            if (args.size() > 4) {
                if (const auto *initial = std::get_if<StructPtr>(&args[4]); initial && *initial)
                    for (const auto &[name, value] : (*initial)->variables)
                        i->vars[std::string(name)] = value;
                else if (!std::holds_alternative<std::monostate>(args[4]))
                    throw Error(ErrorKind::vm, "instance_create_depth initial values must be a struct");
            }
            event(i, 14, 0);
            event(i, 0, 0);
            return std::int32_t{i->id};
        }
        if (name == "instance_change") {
            const int object = static_cast<int>(n(0));
            const bool perform = n(1) != 0;
            if (object < 0 || static_cast<std::size_t>(object) >= objects.size())
                throw Error(ErrorKind::vm, "instance_change object ID out of range");
            if (perform) {
                event(self, 1, 0);
                if (self->alive)
                    event(self, 12, 0);
                if (!self->alive)
                    return std::monostate{};
            }
            self->object = object;
            self->vars["object_index"] = std::int32_t{object};
            if (perform) {
                const auto &replacement = objects[static_cast<std::size_t>(object)];
                self->vars["sprite_index"] = std::int32_t{replacement.sprite};
                self->vars["mask_index"] = std::int32_t{replacement.mask};
                self->vars["visible"] = std::int32_t{replacement.visible};
                self->vars["depth"] = std::int32_t{replacement.depth};
                self->vars["solid"] = std::int32_t{replacement.solid};
                self->vars["image_index"] = 0.0;
                self->vars["image_speed"] = 1.0;
                event(self, 14, 0);
                event(self, 0, 0);
            }
            return std::monostate{};
        }
        if (name == "instance_destroy" || name == "action_kill_object") {
            event(self, 1, 0);
            self->alive = false;
            return std::monostate{};
        }
        if (name == "instance_deactivate_all" || name == "instance_activate_all") {
            const bool deactivate = name == "instance_deactivate_all";
            const bool keep_self = deactivate && args.size() > 0 && n(0) != 0;
            for (const auto &i : instances)
                if (i->alive && (!keep_self || i != self))
                    i->deactivated = deactivate;
            return std::monostate{};
        }
        if (name == "instance_activate_object") {
            const int object = static_cast<int>(n(0));
            for (const auto &i : instances)
                if (i->alive && matches(*i, object))
                    i->deactivated = false;
            return std::monostate{};
        }
        if (name == "instance_deactivate_object") {
            const int object = static_cast<int>(n(0));
            for (const auto &i : instances)
                if (i->alive && matches(*i, object))
                    i->deactivated = true;
            return std::monostate{};
        }
        if (name == "room_set_persistent") {
            const int id = static_cast<int>(n(0));
            if (id < 0 || static_cast<std::size_t>(id) >= rooms.size())
                throw Error(ErrorKind::vm, "room ID out of range");
            const bool persistent = n(1) != 0;
            rooms[static_cast<std::size_t>(id)].persistent = persistent;
            if (id == current_room)
                engine["room_persistent"] = std::int32_t{persistent};
            if (auto saved = saved_rooms.find(id); saved != saved_rooms.end())
                saved->second.engine["room_persistent"] = std::int32_t{persistent};
            return std::monostate{};
        }
        if (name == "room_get_name") {
            const int id = number(args[0]);
            if (id < 0 || static_cast<std::size_t>(id) >= rooms.size())
                throw Error(ErrorKind::vm, "room ID out of range");
            return std::string(rooms[static_cast<std::size_t>(id)].name);
        }
        if (name == "room_goto") {
            pending_room = static_cast<int>(n(0));
            return std::monostate{};
        }
        if (name == "room_restart") {
            pending_room = current_room;
            return std::monostate{};
        }
        if (name == "room_goto_previous" || name == "action_previous_room") {
            const auto it = std::find(order.begin(), order.end(), current_room);
            if (it == order.end() || it == order.begin())
                throw Error(ErrorKind::vm, "no previous room");
            pending_room = *(it - 1);
            return std::monostate{};
        }
        if (name == "room_goto_next") {
            const auto it = std::find(order.begin(), order.end(), current_room);
            if (it == order.end() || it + 1 == order.end())
                throw Error(ErrorKind::vm, "no next room");
            pending_room = *(it + 1);
            return std::monostate{};
        }
        if (name == "room_previous" || name == "room_next") {
            const int id = args.empty() ? current_room : static_cast<int>(n(0));
            const auto it = std::find(order.begin(), order.end(), id);
            if (it == order.end())
                return std::int32_t{-1};
            if (name == "room_previous")
                return std::int32_t{it == order.begin() ? -1 : *(it - 1)};
            return std::int32_t{it + 1 == order.end() ? -1 : *(it + 1)};
        }
        if (name == "game_restart") {
            pending_room = order.front();
            return std::monostate{};
        }
        if (name == "game_end") {
            engine["game_end"] = std::int32_t{1};
            return std::monostate{};
        }
        if (name == "action_create_object") {
            Placement p;
            p.object = static_cast<int>(n(0));
            p.x = n(1);
            p.y = n(2);
            if (auto it = globals.find("__argument_relative");
                it != globals.end() && value_number(it->second) != 0) {
                p.x += value_number(self->vars.at("x"));
                p.y += value_number(self->vars.at("y"));
            }
            p.id = next_id++;
            auto instance = make(p);
            event(instance, 14, 0);
            event(instance, 0, 0);
            return std::int32_t{instance->id};
        }
        if (name == "tile_layer_hide" || name == "tile_layer_show") {
            const int id = static_cast<int>(n(0));
            if (name == "tile_layer_hide")
                hidden_tile_layers.insert(id);
            else
                hidden_tile_layers.erase(id);
            return std::monostate{};
        }
        if (name == "tile_layer_shift") {
            const int id = static_cast<int>(n(0));
            auto &offset = tile_layer_offsets[id];
            offset.first += n(1);
            offset.second += n(2);
            return std::monostate{};
        }
        if (name == "keyboard_check" || name == "keyboard_check_direct" || name == "keyboard_check_pressed" ||
            name == "keyboard_check_released") {
            const auto key = static_cast<int>(n(0));
            if (key < 0 || key >= 256)
                return false;
            return name == "keyboard_check_pressed"    ? pressed[static_cast<std::size_t>(key)]
                   : name == "keyboard_check_released" ? released[static_cast<std::size_t>(key)]
                                                       : held[static_cast<std::size_t>(key)];
        }
        if (name == "mouse_check_button" || name == "mouse_check_button_pressed" ||
            name == "mouse_check_button_released" || name == "device_mouse_check_button" ||
            name == "device_mouse_check_button_pressed" || name == "device_mouse_check_button_released")
            return false;
        if (name == "keyboard_key_press" || name == "keyboard_key_release") {
            const auto key = static_cast<int>(n(0));
            if (key >= 0 && key < 256) {
                const auto k = static_cast<std::size_t>(key);
                held[k] = name == "keyboard_key_press";
            }
            return std::monostate{};
        }
        if (name == "gamepad_get_device_count")
            return std::int32_t{0};
        if (name == "gamepad_get_description")
            return std::string{};
        if (name == "gamepad_get_guid")
            return std::string{"none"};
        if (name == "gamepad_test_mapping")
            return false;
        if (name == "gamepad_is_connected")
            return false;
        if (name == "joystick_exists")
            return false;
        if (name == "gamepad_button_check" || name == "gamepad_button_check_pressed" ||
            name == "gamepad_button_check_released" || name == "joystick_check_button" ||
            name == "joystick_buttons" || name == "joystick_has_pov")
            return false;
        if (name == "joystick_pov" || name == "joystick_direction" || name == "gamepad_axis_value" ||
            name == "joystick_xpos" || name == "joystick_ypos")
            return 0.0;
        if (name == "vertex_format_begin") {
            vertex_format_building = true;
            vertex_format_attributes.clear();
            return std::monostate{};
        }
        if (name == "vertex_format_add_position") {
            if (vertex_format_building)
                vertex_format_attributes.push_back(1);
            return std::monostate{};
        }
        if (name == "vertex_format_add_position_3d") {
            if (vertex_format_building)
                vertex_format_attributes.push_back(2);
            return std::monostate{};
        }
        if (name == "vertex_format_add_normal") {
            if (vertex_format_building)
                vertex_format_attributes.push_back(3);
            return std::monostate{};
        }
        if (name == "vertex_format_add_colour" || name == "vertex_format_add_color") {
            if (vertex_format_building)
                vertex_format_attributes.push_back(4);
            return std::monostate{};
        }
        if (name == "vertex_format_add_textcoord") {
            if (vertex_format_building)
                vertex_format_attributes.push_back(5);
            return std::monostate{};
        }
        if (name == "vertex_format_end") {
            vertex_format_building = false;
            const int format = next_vertex_format++;
            vertex_formats.insert(format);
            vertex_format_attributes.clear();
            return std::int32_t{format};
        }
        if (name == "vertex_create_buffer") {
            const int buffer = next_vertex_buffer++;
            vertex_buffers.insert(buffer);
            return std::int32_t{buffer};
        }
        if (name == "switch_accounts_is_user_open")
            return false;
        if (name == "switch_controller_support_get_selected_id")
            return std::int32_t{-1};
        if (name == "switch_language_get_desired_language")
            return std::int32_t{0};
        if (name == "switch_save_data_mount")
            return true;
        if (name == "switch_accounts_open_user" || name == "switch_accounts_select_account" ||
            name == "switch_controller_support_show" || name == "switch_controller_set_supported_styles" ||
            name == "switch_controller_support_set_defaults" ||
            name == "switch_controller_support_set_singleplayer_only" ||
            name == "switch_save_data_commit" || name == "switch_show_store" ||
            name == "psn_init_np_libs" || name == "psn_init_trophy" || name == "psn_load_modules" ||
            name == "psn_np_commerce_dialog_tick" || name == "psn_post_uds_event" || name == "psn_tick" ||
            name == "psn_unlock_trophy")
            return std::monostate{};
        if (name == "psn_get_trophy_unlock_state")
            return std::int32_t{0};
        if (name == "sqr")
            return n(0) * n(0);
        if (name == "power")
            return std::pow(n(0), n(1));
        if (name == "abs")
            return std::abs(n(0));
        if (name == "sign") {
            const double value = n(0);
            return std::int32_t{value > 0 ? 1 : value < 0 ? -1 : 0};
        }
        if (name == "floor")
            return std::floor(n(0));
        if (name == "ceil")
            return std::ceil(n(0));
        if (name == "round")
            return std::nearbyint(n(0));
        if (name == "sin")
            return std::sin(n(0));
        if (name == "dsin")
            return std::sin(n(0) * 3.141592653589793 / 180.0);
        if (name == "cos")
            return std::cos(n(0));
        if (name == "dcos")
            return std::cos(n(0) * 3.141592653589793 / 180.0);
        if (name == "sqrt")
            return std::sqrt(n(0));
        if (name == "min" || name == "max") {
            double v = n(0);
            for (std::size_t i = 1; i < args.size(); ++i)
                v = name == "min" ? std::min(v, n(i)) : std::max(v, n(i));
            return v;
        }
        if (name == "clamp")
            return std::clamp(n(0), n(1), n(2));
        if (name == "lerp")
            return n(0) + (n(1) - n(0)) * n(2);
        if (name == "angle_difference") {
            double difference = std::fmod(n(0) - n(1), 360.0);
            if (difference < -180)
                difference += 360;
            else if (difference > 180)
                difference -= 360;
            return difference;
        }
        if (name == "random")
            return std::generate_canonical<double, 53>(random) * n(0);
        if (name == "irandom")
            return std::floor(std::generate_canonical<double, 53>(random) * (n(0) + 1));
        if (name == "choose") {
            if (args.empty())
                throw Error(ErrorKind::vm, "choose requires at least one argument");
            return args[std::uniform_int_distribution<std::size_t>(0, args.size() - 1)(random)];
        }
        if (name == "window_set_caption") {
            engine["window_caption"] = text(0);
            return std::monostate{};
        }
        if (name == "window_set_size") {
            engine["window_width"] = std::max(1.0, n(0));
            engine["window_height"] = std::max(1.0, n(1));
            return std::monostate{};
        }
        if (name == "window_set_cursor")
            return std::monostate{};
        if (name == "game_get_speed") {
            const double fps = value_number(engine.contains("room_speed") ? engine.at("room_speed")
                                                                          : Value{60.0});
            return args.empty() || n(0) == 0 ? Value{fps} : Value{fps > 0 ? 1000000.0 / fps : 0.0};
        }
        if (name == "display_set_gui_size") {
            engine["gui_width"] = n(0);
            engine["gui_height"] = n(1);
            return std::monostate{};
        }
        if (name == "display_get_width")
            return static_cast<double>(canvas.logical_width());
        if (name == "display_get_height")
            return static_cast<double>(canvas.logical_height());
        if (name == "display_get_gui_width")
            return engine.contains("gui_width") ? engine.at("gui_width")
                                                 : Value{static_cast<double>(canvas.logical_width())};
        if (name == "display_get_gui_height")
            return engine.contains("gui_height") ? engine.at("gui_height")
                                                  : Value{static_cast<double>(canvas.logical_height())};
        if (name == "window_get_width")
            return engine.contains("window_width") ? engine.at("window_width")
                                                    : Value{static_cast<double>(canvas.logical_width())};
        if (name == "window_get_height")
            return engine.contains("window_height") ? engine.at("window_height")
                                                     : Value{static_cast<double>(canvas.logical_height())};
        if (name == "window_get_x")
            return engine.contains("window_x") ? engine.at("window_x") : Value{0.0};
        if (name == "window_get_y")
            return engine.contains("window_y") ? engine.at("window_y") : Value{0.0};
        if (name == "window_get_fullscreen")
            return engine.contains("window_fullscreen") && value_number(engine.at("window_fullscreen")) != 0;
        if (name == "window_enable_borderless_fullscreen") {
            engine["window_borderless_fullscreen"] = n(0) != 0;
            return std::monostate{};
        }
        if (name == "window_get_borderless_fullscreen")
            return engine.contains("window_borderless_fullscreen") &&
                   value_number(engine.at("window_borderless_fullscreen")) != 0;
        if (name == "window_set_position") {
            engine["window_x"] = n(0);
            engine["window_y"] = n(1);
            return std::monostate{};
        }
        if (name == "window_center") {
            engine["window_x"] = 0.0;
            engine["window_y"] = 0.0;
            return std::monostate{};
        }
        if (name == "window_set_fullscreen") {
            engine["window_fullscreen"] = n(0) != 0;
            return std::monostate{};
        }
        if (name == "os_is_paused")
            return false;
        if (name == "get_string_async")
            return std::int32_t{next_async_request++};
        if (name == "extension_stubfunc_real")
            return 0.0;
        if (name == "shader_replace_simple_init_raw")
            return shoujo_shader >= 0 && !this->canvas.gpu_target();
        if (name == "shader_replace_simple_raw")
            return args.empty() ? Value{std::int32_t{-1}} : args[0];
        if (name == "shader_replace_simple_sync") {
            active_shader = args.empty() ? -1 : number(args[0]);
            configure_shader(canvas);
            return std::monostate{};
        }
        if (name == "shaders_are_supported")
            return shoujo_shader >= 0 && !this->canvas.gpu_target();
        if (name == "shader_is_compiled")
            return shader_is_supported(static_cast<int>(n(0)));
        if (name == "shader_get_uniform" || name == "shader_get_sampler_index")
            return std::int32_t{shader_uniform_handle(static_cast<int>(n(0)), text(1),
                                                     name == "shader_get_sampler_index")};
        if (name == "shader_set") {
            active_shader = static_cast<int>(n(0));
            shoujo_texture_failed.fill(false);
            configure_shader(canvas);
            return std::monostate{};
        }
        if (name == "shader_reset") {
            active_shader = -1;
            configure_shader(canvas);
            return std::monostate{};
        }
        if (name.starts_with("shader_set_uniform_")) {
            set_shader_uniform_values(args);
            return std::monostate{};
        }
        if (name == "texture_set_stage") {
            if (args.size() >= 2) {
                const int sampler = static_cast<int>(n(0)), texture = static_cast<int>(n(1));
                const auto uniform = shader_uniforms.find(sampler);
                if (uniform != shader_uniforms.end() && uniform->second.sampler && texture >= 0 &&
                    sprite_textures.contains(texture))
                    shader_texture_stages[sampler] = texture;
                else
                    shader_texture_stages.erase(sampler);
            }
            return std::monostate{};
        }
        if (name == "texture_set_interpolation" || name == "application_surface_enable" ||
            name == "application_surface_draw_enable")
            return std::monostate{};
        if (name == "show_debug_message") {
            std::cout << "Guest: " << text(0) << '\n';
            return std::monostate{};
        }
        throw Error(ErrorKind::unsupported, "engine built-in pending: " + std::string(name));
    }
    void tick(const KeyboardState &input, const KeyboardState &edges) {
        using ProfileClock = std::chrono::steady_clock;
        const auto profile_start = profile_tick_timing ? ProfileClock::now() : ProfileClock::time_point{};
        auto draw_start = ProfileClock::time_point{};
        last_logic_ms = last_draw_ms = 0;
        last_draw_event_ms = 0;
        last_draw_builtin_ms = 0;
        last_draw_builtin_categories.fill(0);
        max_draw_builtin_call_ms = 0;
        slowest_draw_builtin = {};
        draw_event_depth = 0;
        const char *allocation_stage = "audio service";
        try {
            audio.service();
            static constexpr std::array<std::string_view, 12> alarm_names = {
                "alarm[0]", "alarm[1]", "alarm[2]", "alarm[3]", "alarm[4]",  "alarm[5]",
                "alarm[6]", "alarm[7]", "alarm[8]", "alarm[9]", "alarm[10]", "alarm[11]"};
            held = input;
            pressed = edges;
            for (std::size_t k = 0; k < 256; ++k) {
                pressed[k] = pressed[k] || (held[k] && !previous_held[k]);
                released[k] = !held[k] && previous_held[k];
            }
            previous_held = held;
            event_budget = 0;
            allocation_stage = "room entry";
            for (int transitions = 0; pending_room >= 0 && transitions < 16; ++transitions)
                enter(pending_room);
#if REGML_PS2
            if (graphics_prefetch_pending) {
                allocation_stage = "room graphics prefetch";
                prefetch_room_graphics();
                graphics_prefetch_pending = false;
            }
#endif
            frame_instances.assign(instances.begin(), instances.end());
            struct SnapshotGuard {
                std::vector<InstancePtr> &instances;
                ~SnapshotGuard() { instances.clear(); }
            } snapshot_guard{frame_instances};
            const auto &snapshot = frame_instances;
            allocation_stage = "step and alarm events";
            for (auto &i : snapshot) {
                if (!i->alive || i->deactivated)
                    continue;
                event(i, 3, 1);
                if (i->alive) {
                    i->vars["xprevious"] = i->vars["x"];
                    i->vars["yprevious"] = i->vars["y"];
                }
            }
            for (auto &i : snapshot)
                if (i->alive && !i->deactivated)
                    for (int a = 0; a < 12; ++a) {
                        auto &v = i->vars.at(alarm_names[static_cast<std::size_t>(a)]);
                        const auto timer = number(v);
                        if (timer > 0) {
                            v = std::int32_t{timer - 1};
                            if (timer == 1)
                                event(i, 2, a);
                        }
                    }
            for (auto &i : snapshot)
                if (!i->deactivated)
                    event(i, 3, 0);
            for (auto &i : snapshot)
                if (i->alive && !i->deactivated) {
                    double h = value_number(i->vars.at("hspeed")), v = value_number(i->vars.at("vspeed"));
                    const double friction = value_number(i->vars.at("friction"));
                    if (friction != 0 && (h != 0 || v != 0)) {
                        const double speed = std::hypot(h, v);
                        const double next = std::max(0.0, speed - friction);
                        h *= next / speed;
                        v *= next / speed;
                    }
                    const double gravity = value_number(i->vars.at("gravity"));
                    if (gravity != 0) {
                        const double direction = value_number(i->vars.at("gravity_direction"));
                        if (direction == 0 || direction == 360)
                            h += gravity;
                        else if (direction == 90)
                            v -= gravity;
                        else if (direction == 180)
                            h -= gravity;
                        else if (direction == 270)
                            v += gravity;
                        else {
                            const double angle = direction * 3.141592653589793 / 180;
                            h += gravity * std::cos(angle);
                            v -= gravity * std::sin(angle);
                        }
                    }
                    i->vars["hspeed"] = h;
                    i->vars["vspeed"] = v;
                    i->vars["speed"] = h == 0 && v == 0 ? 0 : std::hypot(h, v);
                    if (h != 0 || v != 0)
                        i->vars["direction"] =
                            std::fmod(std::atan2(-v, h) * 180 / 3.141592653589793 + 360, 360);
                    const int path_id = number(i->vars.at("path_index"));
                    if (path_id >= 0) {
                        const auto &path = paths.at(static_cast<std::size_t>(path_id));
                        if (path.length > 0) {
                            const double scale = value_number(i->vars.at("path_scale"));
                            double position = value_number(i->vars.at("path_position")),
                                   distance = position * path.length;
                            auto sample = [&](double d) {
                                auto next = std::lower_bound(path.points.begin() + 1, path.points.end(), d,
                                                             [](const PathPoint &point, double target) {
                                                                 return point.distance < target;
                                                             });
                                if (next == path.points.end())
                                    return path.points.back();
                                const auto &before = *(next - 1);
                                const double length = next->distance - before.distance,
                                             t = length > 0 ? (d - before.distance) / length : 0;
                                return PathPoint{before.x + (next->x - before.x) * t,
                                                 before.y + (next->y - before.y) * t,
                                                 before.speed + (next->speed - before.speed) * t, d};
                            };
                            const auto before = sample(distance);
                            distance += value_number(i->vars.at("path_speed")) * before.speed / 100 /
                                        std::max(std::abs(scale), 0.000001);
                            const bool ended = distance < 0 || distance >= path.length;
                            const int action = number(i->vars.at("path_endaction"));
                            double continuation_x = 0, continuation_y = 0;
                            if (ended) {
                                if (action == 1 || action == 2) {
                                    if (action == 2) {
                                        // continue starts the next pass from the endpoint's offset
                                        const double cycles = std::floor(distance / path.length);
                                        continuation_x =
                                            (path.points.back().x - path.points.front().x) * scale * cycles;
                                        continuation_y =
                                            (path.points.back().y - path.points.front().y) * scale * cycles;
                                        i->vars["path_xstart"] =
                                            value_number(i->vars.at("path_xstart")) + continuation_x;
                                        i->vars["path_ystart"] =
                                            value_number(i->vars.at("path_ystart")) + continuation_y;
                                    }
                                    distance = std::fmod(std::fmod(distance, path.length) + path.length,
                                                         path.length);
                                } else if (action == 3) {
                                    distance = std::clamp(distance, 0.0, path.length);
                                    i->vars["path_speed"] = -value_number(i->vars.at("path_speed"));
                                } else {
                                    distance = std::clamp(distance, 0.0, path.length);
                                    i->vars["path_index"] = std::int32_t{-1};
                                }
                            }
                            const auto point = sample(distance);
                            const double dx = (point.x - before.x) * scale + continuation_x,
                                         dy = (point.y - before.y) * scale + continuation_y;
                            if (dx != 0 || dy != 0)
                                i->vars["direction"] =
                                    std::fmod(std::atan2(-dy, dx) * 180 / 3.141592653589793 + 360, 360);
                            i->vars["path_positionprevious"] = position;
                            i->vars["path_position"] = distance / path.length;
                            i->vars["x"] = value_number(i->vars.at("path_xstart")) + point.x * scale;
                            i->vars["y"] = value_number(i->vars.at("path_ystart")) + point.y * scale;
                            h = 0;
                            v = 0;
                            if (ended)
                                event(i, 7, 8);
                        }
                    }
                    i->vars["x"] = value_number(i->vars["x"]) + h;
                    i->vars["y"] = value_number(i->vars["y"]) + v;
                    if (objects[static_cast<std::size_t>(i->object)].has_keyboard_events) {
                        for (std::size_t k = 0; k < 256; ++k) {
                            if (held[k])
                                event(i, 5, static_cast<int>(k));
                            if (pressed[k])
                                event(i, 9, static_cast<int>(k));
                        }
                    }
                }
            // fire once after the whole sprite leaves the room
            const double room_width = value_number(engine.at("room_width")),
                         room_height = value_number(engine.at("room_height"));
            allocation_stage = "outside-room checks";
            for (const auto &i : snapshot)
                if (i->alive) {
                    const auto b =
                        bounds(*i, value_number(i->vars.at("x")), value_number(i->vars.at("y")), true);
                    const bool outside =
                        b.valid
                            ? (b.right <= 0 || b.left >= room_width || b.bottom <= 0 || b.top >= room_height)
                            : (b.left < 0 || b.left > room_width || b.top < 0 || b.top > room_height);
                    const bool entered = outside && !i->outside_room;
                    i->outside_room = outside;
                    if (entered)
                        event(i, 7, 0);
                }
            // skip the geometry work when the pair has no collision handler
            allocation_stage = "collision checks";
            if (tick_collision_routes.size() != objects.size()) {
                tick_collision_routes.resize(objects.size());
                tick_collision_route_stamps.assign(objects.size(), 0);
                tick_collision_route_generation = 0;
            }
            for (const auto &i : snapshot)
                if (i->alive && objects[static_cast<std::size_t>(i->object)].has_collision_events) {
                    if (++tick_collision_route_generation == 0) {
                        std::fill(tick_collision_route_stamps.begin(), tick_collision_route_stamps.end(), 0);
                        ++tick_collision_route_generation;
                    }
                    for (const auto &j : snapshot)
                        if (j->alive && i != j) {
                            const auto target_object = static_cast<std::size_t>(j->object);
                            if (tick_collision_route_stamps[target_object] !=
                                tick_collision_route_generation) {
                                tick_collision_routes[target_object] = collision_event(i->object, j->object);
                                tick_collision_route_stamps[target_object] = tick_collision_route_generation;
                            }
                            const auto route = tick_collision_routes[target_object];
                            if (route.source >= 0 && collides(*i, value_number(i->vars.at("x")),
                                                              value_number(i->vars.at("y")), *j)) {
                                const double other_x = value_number(j->vars.at("x"));
                                const double other_y = value_number(j->vars.at("y"));
                                const double other_step_x = other_x - value_number(j->vars.at("xprevious"));
                                const double other_step_y = other_y - value_number(j->vars.at("yprevious"));
                                event(i, 4, route.target, route.source, j);
                                if (j->alive) {
                                    // collision code can move `other` by the same amount it already moved
                                    // this step. the collision runs after Step, so don't count that
                                    // movement twice
                                    const double pushed_x = value_number(j->vars.at("x")) - other_x;
                                    const double pushed_y = value_number(j->vars.at("y")) - other_y;
                                    if (other_step_x != 0 && std::abs(pushed_x - other_step_x) < 1e-9)
                                        j->vars["x"] = value_number(j->vars.at("x")) - other_step_x;
                                    if (other_step_y != 0 && std::abs(pushed_y - other_step_y) < 1e-9)
                                        j->vars["y"] = value_number(j->vars.at("y")) - other_step_y;
                                }
                            }
                        }
                }
            allocation_stage = "end-step events";
            for (const auto &i : snapshot)
                event(i, 3, 2);
            if (number(engine.at("view_enabled")) != 0 && number(engine.at("view_visible[0]")) != 0) {
                const int target = number(engine.at("view_object[0]"));
                const double vw = value_number(engine.at("view_wview[0]")),
                             vh = value_number(engine.at("view_hview[0]"));
                if (target >= 0 && vw > 0 && vh > 0) {
                    const auto tracked = std::find_if(instances.begin(), instances.end(), [&](const auto &i) {
                        return i->alive && i->object == target;
                    });
                    if (tracked != instances.end()) {
                        const auto &room = rooms.at(static_cast<std::size_t>(current_room));
                        auto follow = [&](const char *view_key, const char *speed_key, const char *coordinate,
                                          double extent, double border, double room_extent) {
                            double origin = value_number(engine.at(view_key));
                            const double pos = value_number((*tracked)->vars.at(coordinate));
                            double desired = origin;
                            if (pos < origin + border)
                                desired = pos - border;
                            else if (pos > origin + extent - border)
                                desired = pos - extent + border;
                            const double rate = value_number(engine.at(speed_key));
                            if (rate < 0)
                                origin = desired;
                            else
                                origin += std::clamp(desired - origin, -rate, rate);
                            const double maximum = std::max(0.0, room_extent - extent);
                            origin = std::clamp(origin, 0.0, maximum);
                            engine[view_key] = origin;
                        };
                        follow("view_xview[0]", "view_hspeed[0]", "x", vw,
                               value_number(engine.at("view_hborder[0]")), room.width);
                        follow("view_yview[0]", "view_vspeed[0]", "y", vh,
                               value_number(engine.at("view_vborder[0]")), room.height);
                    }
                }
            }
            allocation_stage = "room view setup";
            if (profile_tick_timing) {
                draw_start = ProfileClock::now();
                last_logic_ms = std::chrono::duration<double, std::milli>(draw_start - profile_start).count();
            }
            canvas.clear(rooms.at(static_cast<std::size_t>(current_room)).color);
            if (number(engine.at("view_enabled")) != 0 && number(engine.at("view_visible[0]")) != 0) {
                const auto w = value_number(engine.at("view_wview[0]")),
                           h = value_number(engine.at("view_hview[0]"));
                if (w <= 0 || h <= 0)
                    throw Error(ErrorKind::vm, "invalid view dimensions");
                const double port_x = value_number(engine.at("view_xport[0]"));
                const double port_y = value_number(engine.at("view_yport[0]"));
                const double port_width = value_number(engine.at("view_wport[0]"));
                double port_height = value_number(engine.at("view_hport[0]"));
                const double screen_width = canvas.logical_width();
                const double screen_height = canvas.logical_height();
                if (port_x == 0 && port_y == 0 && std::abs(port_width - screen_width) < 0.5 &&
                    port_height < screen_height &&
                    std::abs(w / h - screen_width / screen_height) < 0.001) {
                    port_height = screen_height;
                    engine["view_hport[0]"] = port_height;
                }
                assets.view(value_number(engine.at("view_xview[0]")),
                            value_number(engine.at("view_yview[0]")), port_width / w, port_height / h,
                            port_x, port_y);
            } else
                assets.view(0, 0, 1, 1, 0, 0);
            auto &room = rooms.at(static_cast<std::size_t>(current_room));
            for (auto &layer : room.layers) {
                layer.x += layer.hspeed;
                layer.y += layer.vspeed;
            }
            allocation_stage = "background and tile drawing";
            for (const auto &b : room.backgrounds)
                if (!b.foreground)
                    assets.draw_background(canvas, b.id, b.x, b.y);
            auto draw_tile = [&](const Tile &t) {
                if (!t.visible)
                    return;
                const auto layer_id = t.layer_id < 0 ? t.id : t.layer_id;
                if (hidden_tile_layers.contains(layer_id))
                    return;
                double layer_x = 0, layer_y = 0;
                if (const auto found = room.layer_indices.find(t.layer_id); found != room.layer_indices.end()) {
                    const auto &layer = room.layers[found->second];
                    if (!layer.visible)
                        return;
                    layer_x = layer.x - layer.origin_x;
                    layer_y = layer.y - layer.origin_y;
                }
                const auto offset = tile_layer_offsets.find(layer_id);
                const double ox = offset == tile_layer_offsets.end() ? 0 : offset->second.first;
                const double oy = offset == tile_layer_offsets.end() ? 0 : offset->second.second;
                int source_x = t.source_x, source_y = t.source_y;
                if (t.modern_tileset) {
                    const auto elapsed_us = static_cast<std::uint64_t>(
                        std::max(0.0, value_number(engine["current_time"])) * 1000.0);
                    const auto source = assets.background_tile_source(t.id, t.tileset_index, elapsed_us);
                    if (!source)
                        return;
                    source_x = source->x;
                    source_y = source->y;
                }
                assets.draw_tile(canvas, t.id, source_x, source_y, t.width, t.height,
                                 t.x + ox + layer_x, t.y + oy + layer_y, t.sx, t.sy, t.color, t.transform);
            };
            struct LayerVisualEntry {
                double depth = 0;
                int layer = -1, element = -1;
                bool background = false;
            };
            std::vector<LayerVisualEntry> layer_visuals;
            for (const auto &layer : room.layers) {
                if (!layer.visible)
                    continue;
                for (const auto &sprite : layer.sprites)
                    layer_visuals.push_back({static_cast<double>(layer.depth), layer.id, sprite.id, false});
                for (const auto &background : layer.backgrounds)
                    layer_visuals.push_back({background.foreground ? std::numeric_limits<double>::lowest()
                                                                  : static_cast<double>(layer.depth),
                                             layer.id, background.id, true});
            }
            std::stable_sort(layer_visuals.begin(), layer_visuals.end(),
                             [](const auto &left, const auto &right) { return left.depth > right.depth; });
            auto draw_layer_visual = [&](const LayerVisualEntry &entry) {
                const auto layer_index = room.layer_indices.find(entry.layer);
                if (layer_index == room.layer_indices.end())
                    return;
                auto &layer = room.layers[layer_index->second];
                if (!layer.visible)
                    return;
                const double ox = layer.x - layer.origin_x, oy = layer.y - layer.origin_y;
                if (entry.background) {
                    const auto found = std::find_if(layer.backgrounds.begin(), layer.backgrounds.end(),
                                                    [&](const LayerBackground &item) {
                                                        return item.id == entry.element;
                                                    });
                    if (found == layer.backgrounds.end() || !found->visible || !assets.sprite_exists(found->sprite))
                        return;
                    auto &background = *found;
                    const auto &sprite = assets.sprite(background.sprite);
                    if (sprite.width <= 0 || sprite.height <= 0)
                        return;
                    double sx = background.sx, sy = background.sy;
                    if (background.stretch) {
                        sx *= static_cast<double>(room.width) / sprite.width;
                        sy *= static_cast<double>(room.height) / sprite.height;
                    }
                    const double step_x = sprite.width * sx, step_y = sprite.height * sy;
                    if (step_x == 0 || step_y == 0)
                        return;
                    double x = ox, y = oy;
                    if (background.htiled)
                        while (x > 0)
                            x -= step_x;
                    if (background.vtiled)
                        while (y > 0)
                            y -= step_y;
                    const int columns = background.htiled
                                            ? std::clamp(static_cast<int>(std::ceil((room.width - x) /
                                                                                     std::abs(step_x))),
                                                         1, 256)
                                            : 1;
                    const int rows = background.vtiled
                                         ? std::clamp(static_cast<int>(std::ceil((room.height - y) /
                                                                                std::abs(step_y))),
                                                      1, 256)
                                         : 1;
                    const auto tint = background.color & 0xffffffU;
                    const double alpha = static_cast<double>(background.color >> 24) / 255.0;
                    for (int row = 0; row < rows && row * columns < 4096; ++row)
                        for (int column = 0; column < columns && row * columns + column < 4096; ++column)
                            assets.draw_sprite(canvas, background.sprite, background.frame,
                                               x + column * step_x, y + row * step_y, sx, sy, 0, tint, alpha);
                    const double frame_count = static_cast<double>(sprite.frames.size());
                    if (frame_count > 0) {
                        const double fps = value_number(engine.at("room_speed"));
                        const double advance = background.speed_type == 0 ? background.speed / fps
                                                                          : background.speed;
                        background.frame = std::fmod(std::fmod(background.frame + advance, frame_count) +
                                                         frame_count,
                                                     frame_count);
                    }
                    return;
                }
                const auto found = std::find_if(layer.sprites.begin(), layer.sprites.end(),
                                                [&](const LayerSprite &item) { return item.id == entry.element; });
                if (found == layer.sprites.end() || !assets.sprite_exists(found->sprite))
                    return;
                auto &sprite = *found;
                assets.draw_sprite(canvas, sprite.sprite, sprite.frame, sprite.x + ox, sprite.y + oy, sprite.sx,
                                   sprite.sy, sprite.rotation, sprite.color & 0xffffffU,
                                   static_cast<double>(sprite.color >> 24) / 255.0);
                const double frame_count = static_cast<double>(assets.sprite(sprite.sprite).frames.size());
                if (frame_count > 0 && sprite.speed != 0) {
                    const double fps = value_number(engine.at("room_speed"));
                    const double advance = sprite.speed_type == 0 ? sprite.speed / fps : sprite.speed;
                    sprite.frame = std::fmod(std::fmod(sprite.frame + advance, frame_count) + frame_count,
                                             frame_count);
                }
            };
            auto run_layer_script = [&](const Value &script, const InstancePtr &context) {
                if (std::holds_alternative<std::monostate>(script))
                    return;
                const auto old_event_type = engine.find("event_type");
                const auto old_event_number = engine.find("event_number");
                const std::optional<Value> saved_event_type =
                    old_event_type == engine.end() ? std::nullopt : std::optional<Value>{old_event_type->second};
                const std::optional<Value> saved_event_number = old_event_number == engine.end()
                                                                   ? std::nullopt
                                                                   : std::optional<Value>{old_event_number->second};
                engine["event_type"] = std::int32_t{8};
                engine["event_number"] = std::int32_t{0};
                if (const auto *callable = std::get_if<CallablePtr>(&script); callable && *callable &&
                    (*callable)->invoke)
                    (*callable)->invoke(std::span<const Value>{});
                else if (const auto *function = std::get_if<FunctionReference>(&script))
                    invoke_function(function->symbol_id, std::span<const Value>{}, context, {});
                if (saved_event_type)
                    engine["event_type"] = *saved_event_type;
                else
                    engine.erase("event_type");
                if (saved_event_number)
                    engine["event_number"] = *saved_event_number;
                else
                    engine.erase("event_number");
            };
            std::size_t tile = 0;
            std::size_t layer_visual = 0;
            allocation_stage = "draw-list allocation";
            bool rebuild_draw_order = draw_entries.size() != snapshot.size();
            bool resort_draw_order = false;
            if (!rebuild_draw_order) {
                for (auto &entry : draw_entries) {
                    if (entry.order >= snapshot.size() || entry.instance != snapshot[entry.order].get()) {
                        rebuild_draw_order = true;
                        break;
                    }
                    const double depth = value_number(snapshot[entry.order]->vars.at("depth"));
                    resort_draw_order |= entry.depth != depth;
                    entry.depth = depth;
                }
            }
            if (rebuild_draw_order) {
                draw_entries.clear();
                if (draw_entries.capacity() < snapshot.size())
                    draw_entries.reserve(snapshot.size());
                for (std::size_t index = 0; index < snapshot.size(); ++index) {
                    const auto &instance = snapshot[index];
                    draw_entries.push_back({value_number(instance->vars.at("depth")), index, instance.get()});
                }
                resort_draw_order = true;
            }
            if (resort_draw_order)
                std::sort(draw_entries.begin(), draw_entries.end(), [](const auto &a, const auto &b) {
                    return a.depth != b.depth ? a.depth > b.depth : a.order < b.order;
                });
            allocation_stage = "instance draw calls";
            auto draw_world_before = [&](double depth) {
                while ((tile < room.tiles.size() && room.tiles[tile].depth >= depth) ||
                       (layer_visual < layer_visuals.size() && layer_visuals[layer_visual].depth >= depth)) {
                    if (tile < room.tiles.size() && room.tiles[tile].depth >= depth &&
                        (layer_visual >= layer_visuals.size() ||
                         room.tiles[tile].depth >= layer_visuals[layer_visual].depth))
                        draw_tile(room.tiles[tile++]);
                    else
                        draw_layer_visual(layer_visuals[layer_visual++]);
                }
            };
            for (auto &entry : draw_entries) {
                const auto &i = snapshot[entry.order];
                if (i->alive && !i->deactivated && number(i->vars.at("visible"))) {
                    const int previous_shader = active_shader;
                    const auto layer_id = static_cast<int>(number(i->vars.at("layer")));
                    const auto layer = room.layer_indices.find(layer_id);
                    const RoomLayer *draw_layer =
                        layer == room.layer_indices.end() ? nullptr : &room.layers[layer->second];
                    const bool apply_layer_shader = draw_layer && draw_layer->shader >= 0;
                    if (apply_layer_shader) {
                        active_shader = draw_layer->shader;
                        configure_shader(canvas);
                    }
                    if (draw_layer)
                        run_layer_script(draw_layer->begin_script, i);
                    draw_world_before(value_number(i->vars.at("depth")));
                    if (objects[static_cast<std::size_t>(i->object)].draw_owner >= 0)
                        event(i, 8, 0);
                    else {
                        const auto sprite = number(i->vars.at("sprite_index"));
                        if (sprite >= 0)
                            assets.draw_sprite(canvas, sprite, value_number(i->vars.at("image_index")),
                                               value_number(i->vars.at("x")) +
                                                   (draw_layer ? draw_layer->x - draw_layer->origin_x : 0),
                                               value_number(i->vars.at("y")) +
                                                   (draw_layer ? draw_layer->y - draw_layer->origin_y : 0),
                                               value_number(i->vars.at("image_xscale")),
                                               value_number(i->vars.at("image_yscale")),
                                               value_number(i->vars.at("image_angle")),
                                               static_cast<unsigned>(number(i->vars.at("image_blend"))),
                                               value_number(i->vars.at("image_alpha")));
                    }
                    const int animated_object = i->object,
                              animated_sprite = number(i->vars.at("sprite_index"));
                    const double old_index = value_number(i->vars.at("image_index")),
                                 image_speed = value_number(i->vars.at("image_speed"));
                    if (animated_sprite >= 0 && image_speed != 0) {
                        const auto frame_count = assets.sprite(animated_sprite).frames.size();
                        if (frame_count) {
                            const double count = static_cast<double>(frame_count),
                                         next_index = old_index + image_speed;
                            const bool ended =
                                (image_speed > 0 && old_index < count && next_index >= count) ||
                                (image_speed < 0 && old_index >= 0 && next_index < 0);
                            i->vars["image_index"] = next_index;
                            if (ended)
                                event(i, 7, 7);
                            if (i->alive && i->object == animated_object &&
                                number(i->vars.at("sprite_index")) == animated_sprite &&
                                value_number(i->vars.at("image_index")) == next_index)
                                i->vars["image_index"] =
                                    std::fmod(std::fmod(next_index, count) + count, count);
                        }
                    }
                    if (draw_layer)
                        run_layer_script(draw_layer->end_script, i);
                    if (apply_layer_shader) {
                        active_shader = previous_shader;
                        configure_shader(canvas);
                    }
                }
            }
            draw_world_before(std::numeric_limits<double>::lowest());
            while (layer_visual < layer_visuals.size())
                draw_layer_visual(layer_visuals[layer_visual++]);
            for (const auto &b : room.backgrounds)
                if (b.foreground)
                    assets.draw_background(canvas, b.id, b.x, b.y);
            assets.view(0, 0, 1, 1, 0, 0);
            allocation_stage = "GUI draw events";
            for (const auto &i : snapshot)
                if (i->alive && !i->deactivated && number(i->vars.at("visible")))
                    event(i, 8, 64);
            if (profile_tick_timing)
                last_draw_ms = std::chrono::duration<double, std::milli>(ProfileClock::now() - draw_start).count();
            std::erase_if(instances, [](const auto &i) { return !i->alive; });
            ++frame;
            engine["current_time"] = static_cast<double>(frame) * 1000.0 / value_number(engine["room_speed"]);
#if REGML_PS2
            if (frame % 60 == 0) {
                std::size_t map_entries = 0;
                for (const auto &[id, map] : maps) {
                    (void)id;
                    map_entries += map.size();
                }
                ps2_memory_trace("frame", frame, map_entries);
            }
#endif
        } catch (const GameChangeRequested &) {
            return;
        } catch (const std::bad_alloc &) {
#if REGML_PS2
            std::size_t map_entries = 0;
            for (const auto &[id, map] : maps) {
                (void)id;
                map_entries += map.size();
            }
            ps2_oom_trace(allocation_stage, frame, map_entries);
            throw Error(ErrorKind::io, "out of memory");
#else
            throw Error(ErrorKind::io, "out of memory during " + std::string(allocation_stage));
#endif
        } catch (const std::exception &error) {
            throw Error(ErrorKind::vm,
                        "host error during " + std::string(allocation_stage) + ": " + error.what());
        }
    }
};
GameSession::GameSession(const std::filesystem::path &path,
                         std::optional<std::uint32_t> seed,
                         const std::filesystem::path &save_directory)
    : impl_(std::make_unique<Impl>(path, seed, save_directory)) {}
GameSession::~GameSession() = default;
void GameSession::tick(const KeyboardState &h, const KeyboardState &p) {
    impl_->tick(h, p);
    if (impl_->pending_game_change) {
        const auto path = std::move(*impl_->pending_game_change);
        const auto seed = impl_->fixed_seed;
        const auto save_directory = impl_->save_dir;
        const auto profile = impl_->profile_tick_timing;
        auto next = std::make_unique<Impl>(path, seed, save_directory);
        next->profile_tick_timing = profile;
        next->canvas.profile_text(profile);
#if !REGML_PS2
        std::lock_guard lock(audio_mutex_);
#endif
        impl_ = std::move(next);
    }
}
void GameSession::profile_tick(bool enabled) noexcept {
    impl_->profile_tick_timing = enabled;
    impl_->canvas.profile_text(enabled);
#if REGML_PS2
    ps2_gpu_profile(enabled);
#endif
}
GameSession::TickProfile GameSession::take_tick_profile() noexcept {
    TickProfile profile{impl_->last_logic_ms, impl_->last_draw_ms};
    profile.draw_event_ms = impl_->last_draw_event_ms;
    profile.draw_builtin_ms = impl_->last_draw_builtin_ms;
    profile.draw_text_builtin_ms = impl_->last_draw_builtin_categories[0];
    profile.draw_image_builtin_ms = impl_->last_draw_builtin_categories[1];
    profile.draw_primitive_builtin_ms = impl_->last_draw_builtin_categories[2];
    profile.draw_other_builtin_ms = impl_->last_draw_builtin_categories[3];
    profile.max_draw_builtin_call_ms = impl_->max_draw_builtin_call_ms;
    profile.slowest_draw_builtin = impl_->slowest_draw_builtin;
    profile.text_ms = impl_->canvas.take_text_profile_ms();
#if REGML_PS2
    const auto gpu_profile = ps2_gpu_take_profile();
    profile.image_ms = gpu_profile.image_ms;
    profile.image_calls = gpu_profile.image_calls;
    profile.image_quads = gpu_profile.image_quads;
    profile.glyph_quads = gpu_profile.glyph_quads;
    profile.texture_uploads = gpu_profile.texture_uploads;
#endif
    return profile;
}
void GameSession::text_input(std::string_view text) {
    auto *typed = std::get_if<std::string>(&impl_->engine.at("keyboard_string"));
    if (!typed)
        return;
    for (const unsigned char character : text) {
        if (character == '\b') {
            if (!typed->empty()) {
                typed->pop_back();
                while (!typed->empty() && (static_cast<unsigned char>(typed->back()) & 0xc0) == 0x80)
                    typed->pop_back();
            }
        } else if (character == '\r') {
            typed->push_back('\n');
        } else if (character >= 32) {
            typed->push_back(static_cast<char>(character));
        }
        impl_->engine["keyboard_lastkey"] = static_cast<std::int32_t>(character);
    }
}
const Canvas &GameSession::canvas() const {
    return impl_->canvas;
}
Canvas &GameSession::canvas() {
    return impl_->canvas;
}
std::size_t GameSession::frames() const {
    return impl_->frame;
}
std::size_t GameSession::instructions() const {
    return impl_->instruction_count;
}
std::string GameSession::room_name() const {
    return std::string(impl_->rooms.at(static_cast<std::size_t>(impl_->current_room)).name);
}
std::string_view GameSession::vm_code_name() const {
    if (impl_->current_code < 0)
        return {};
    return impl_->package.code_name(static_cast<std::size_t>(impl_->current_code));
}
std::size_t GameSession::vm_address() const {
    return impl_->current_vm_address;
}
void GameSession::mix_audio(std::span<float> output, double playback_rate) noexcept {
#if !REGML_PS2
    std::lock_guard lock(audio_mutex_);
#endif
    impl_->audio.mix(output, playback_rate);
    const auto master_gain = static_cast<float>(impl_->audio_master_gain.load(std::memory_order_relaxed));
    if (master_gain != 1.0f)
        for (auto &sample : output)
            sample *= master_gain;
}
double GameSession::speed() const {
    return value_number(impl_->engine.at("room_speed"));
}
std::vector<GameSession::InstanceState> GameSession::instance_states() const {
    std::vector<InstanceState> result;
    for (const auto &i : impl_->instances)
        if (i->alive)
            result.push_back(
                {i->id, std::string(impl_->objects.at(static_cast<std::size_t>(i->object)).name), i->vars});
    return result;
}
} // namespace regml
