#include "game.hpp"
#include "audio/sdl/audio.hpp"
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
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
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
#endif
namespace regml {
namespace {
#if REGML_PS2
void ps2_memory_trace(std::string_view stage, std::size_t position = 0) {
    static const bool enabled = [] {
        auto *marker = std::fopen("host:reGML/regml-trace.enabled", "r");
        if (!marker)
            return false;
        std::fclose(marker);
        return true;
    }();
    if (!enabled)
        return;
    const auto memory = mallinfo();
    if (auto *log = std::fopen("host:reGML/regml-load.log", "a")) {
        std::fprintf(log, "%.*s at %u heap=%u free=%u arena=%u\n", static_cast<int>(stage.size()),
                     stage.data(), static_cast<unsigned>(position), static_cast<unsigned>(memory.uordblks),
                     static_cast<unsigned>(memory.fordblks), static_cast<unsigned>(memory.arena));
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
Package load_game_package(const std::filesystem::path &path) {
    auto package = Package::load(path);
    if (package.general_info() && package.general_info()->bytecode_version == 17)
        throw Error(
            ErrorKind::unsupported,
            "bytecode 17 can be decoded and linked, but GameMaker 2 runtime support is not implemented");
    return package;
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
    double x = 0, y = 0, sx = 1, sy = 1;
};
struct Background {
    int id = -1, x = 0, y = 0;
    bool foreground = false;
};
struct Tile {
    int id = -1, x = 0, y = 0, source_x = 0, source_y = 0, width = 0, height = 0, depth = 0;
    double sx = 1, sy = 1;
    unsigned color = 0xffffffff;
};
struct Room {
    std::size_t source_offset = 0;
    bool payload_loaded = false;
    std::string_view name;
    int width = 640, height = 480, speed = 30, create = -1;
    bool persistent = false;
    unsigned color = 0;
    std::vector<Placement> instances;
    VariableStore views;
    std::vector<Background> backgrounds;
    std::vector<Tile> tiles;
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
    LinkedPackage linked;
    GameAssets assets;
    GameAudio audio;
    Canvas canvas{640, 480, true};
    std::vector<Path> paths;
    std::vector<Object> objects;
    struct CollisionRoute {
        int source = -1, target = -1;
    };
    std::unordered_map<std::uint64_t, CollisionRoute> collision_routes;
    std::vector<Room> rooms;
    std::vector<int> order, scripts;
    std::unordered_map<std::string, int, VariableStoreHash, std::equal_to<>> script_names;
    std::vector<InstancePtr> instances;
    std::vector<InstancePtr> tick_instances;
    std::vector<InstancePtr> frame_instances;
    std::vector<InstancePtr> placed_instances;
    struct DrawEntry {
        double depth;
        std::size_t order;
    };
    std::vector<DrawEntry> draw_entries;
    struct SavedRoom {
        std::vector<InstancePtr> instances;
        VariableStore engine;
    };
    std::unordered_map<int, SavedRoom> saved_rooms;
    VariableStore globals, engine;
    KeyboardState held{}, pressed{}, previous_held{}, released{};
    // ds maps can have any game data as keys, so they need their own storage
    // instead of sharing the bytecode name pool
    using DynamicMap = std::unordered_map<std::string, Value, VariableStoreHash, std::equal_to<>>;
    std::unordered_map<int, DynamicMap> maps;
    std::unordered_map<int, std::vector<Value>> lists;
    std::unordered_map<int, GameBuffer> buffers;
    std::unordered_map<int, Canvas> surfaces;
    int next_surface = 1, surface_target = 0;
    std::unordered_set<int> hidden_tile_layers;
    std::unordered_map<int, std::pair<double, double>> tile_layer_offsets;
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
    double comparison_epsilon = 0.00001;
    int draw_font = -1, halign = 0, valign = 0;
    std::size_t frame = 0, instruction_count = 0, depth = 0, event_budget = 0;
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
        std::vector<Value> stack, arguments;
        struct Environment {
            InstancePtr saved, saved_other;
            std::vector<InstancePtr> choices;
            std::size_t index = 0;
        };
        std::vector<Environment> environments;
        std::size_t environment_count = 0;
    };
    std::array<VmScratch, 128> vm_scratch;
    std::filesystem::path game_dir;
    std::optional<std::uint32_t> fixed_seed;
    mutable std::unordered_map<std::string, std::vector<std::uint8_t>> load_chunks;
    explicit Impl(const std::filesystem::path &path,
                  std::optional<std::uint32_t> seed,
                  const std::filesystem::path &save_directory)
        : package(load_stage("package metadata", [&] { return load_game_package(path); })),
          linked(load_stage("bytecode",
                            [&] {
                                return link_legacy_package(package, {}, false, build_instruction_index,
                                                           lazy_programs);
                            })),
          assets(load_stage("graphics metadata", [&] { return GameAssets(package); })),
          audio(load_stage("audio metadata", [&] { return GameAudio(package, path.parent_path()); })),
          game_dir(path.parent_path()), fixed_seed(seed) {
        if (seed)
            random.seed(*seed);
        if (!save_directory.empty())
            save_dir = save_directory;
        else
            save_dir /= save_profile(package, path);
        load_stage("world metadata", [&] {
            load_world();
            std::unordered_map<std::string, std::vector<std::uint8_t>>{}.swap(load_chunks);
            return true;
        });
        // instructions already have the IDs they need, so we can drop these lookup maps now
        std::unordered_map<std::size_t, std::size_t>{}.swap(linked.variable_at);
        std::unordered_map<std::size_t, std::size_t>{}.swap(linked.function_at);
        engine = {{"os_type", std::int32_t{0}},
                  {"os_version", std::int32_t{0}},
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
    void code_valid(int id) const {
        if (id < -1 || (id >= 0 && static_cast<std::size_t>(id) >= linked.programs.size()))
            throw Error(ErrorKind::malformed, "invalid resource code ID");
    }
    void load_world() {
        const auto *sc = package.find_chunk("SCPT");
        const auto *ob = package.find_chunk("OBJT");
        const auto *ro = package.find_chunk("ROOM");
        const auto *ge = package.find_chunk("GEN8");
        if (!sc || !ob || !ro || !ge)
            throw Error(ErrorKind::unsupported, "game requires SCPT/OBJT/ROOM/GEN8 resources");
        for (auto p : list(sc->data_offset, *sc)) {
            const auto r = record(p, *sc, 8);
            const auto name = std::string(package.string_pointer(r.u32(0)));
            const auto id = r.i32(4);
            code_valid(id);
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
            auto r = record(p, *ob, 80);
            Object o;
            o.name = package.string_pointer(r.u32(0));
            o.sprite = r.i32(4);
            o.visible = r.u32(8) != 0;
            o.depth = r.i32(16);
            o.persistent = r.u32(20) != 0;
            o.parent = r.i32(24);
            o.mask = r.i32(28);
            o.solid = r.u32(12) != 0;
            const auto vertices = r.u32(64);
            if (vertices > 4096)
                throw Error(ErrorKind::unsupported, "physics vertices exceed limit", p + 64);
            const auto groups = list(p + 80 + static_cast<std::size_t>(vertices) * 8, *ob);
            for (std::size_t type = 0; type < groups.size(); ++type)
                for (auto e : list(groups[type], *ob)) {
                    const auto er = record(e, *ob, 8);
                    const auto subtype = er.i32(0);
                    for (auto a : list(e + 4, *ob)) {
                        const auto ar = record(a, *ob, 56);
                        const auto code = ar.i32(32);
                        code_valid(code);
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
            room.color = r.u32(24);
            room.create = r.i32(32);
            code_valid(room.create);
            room.persistent = r.u32(20) != 0;
            room.source_offset = p;
            if (room.speed < 1 || room.speed > 1000 || room.width < 1 || room.height < 1)
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
        const auto r = record(p, *ro, 88);
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
            const auto ir = record(q, *ro, 40);
            Placement placement;
            placement.x = ir.i32(0);
            placement.y = ir.i32(4);
            placement.object = ir.i32(8);
            placement.id = ir.i32(12);
            placement.create = ir.i32(16);
            placement.precreate = ir.i32(36);
            placement.sx = std::bit_cast<float>(ir.u32(20));
            placement.sy = std::bit_cast<float>(ir.u32(24));
            if (placement.object < 0 || static_cast<std::size_t>(placement.object) >= objects.size())
                throw Error(ErrorKind::malformed, "invalid placed object", q + 8);
            code_valid(placement.create);
            code_valid(placement.precreate);
            room.instances.push_back(placement);
        }
        for (auto q : list(r.u32(40), *ro)) {
            const auto b = record(q, *ro, 40);
            if (b.i32(0) && b.i32(8) >= 0)
                room.backgrounds.push_back({b.i32(8), b.i32(12), b.i32(16), b.i32(4) != 0});
        }
        for (auto q : list(r.u32(52), *ro)) {
            const auto t = record(q, *ro, 48);
            room.tiles.push_back({t.i32(8), t.i32(0), t.i32(4), t.i32(12), t.i32(16), t.i32(20), t.i32(24),
                                  t.i32(28), std::bit_cast<float>(t.u32(36)), std::bit_cast<float>(t.u32(40)),
                                  t.u32(44)});
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
                      {"sprite_index", std::int32_t{obj.sprite}},
                      {"mask_index", std::int32_t{obj.mask}},
                      {"visible", std::int32_t{obj.visible}},
                      {"persistent", std::int32_t{obj.persistent}},
                      {"solid", std::int32_t{obj.solid}},
                      {"depth", std::int32_t{obj.depth}},
                      {"image_index", 0.0},
                      {"image_speed", 1.0},
                      {"image_xscale", p.sx},
                      {"image_yscale", p.sy},
                      {"image_angle", 0.0},
                      {"image_alpha", 1.0},
                      {"image_blend", std::int32_t{0xffffff}},
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
    std::vector<InstancePtr> targets(int target, const InstancePtr &self, const InstancePtr &other) {
        if (target == -1)
            return self ? std::vector{self} : std::vector<InstancePtr>{};
        if (target == -2)
            return other && other->alive ? std::vector{other} : std::vector<InstancePtr>{};
        std::vector<InstancePtr> out;
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
        engine["room_speed"] = std::int32_t{room.speed};
        engine["room_persistent"] = std::int32_t{room.persistent};
        for (const auto &[key, value] : room.views)
            engine[key] = value;
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
        std::cout << "Entered room " << id << ": " << room.name << " (" << instances.size() << " instances)"
                  << std::endl;
    }
    Value execute(int code, InstancePtr self, std::span<const Value> arguments, InstancePtr other = {}) {
        if (code < 0)
            return std::monostate{};
#if REGML_WII
        if (frame == 0 && depth < 4)
            report_wii_memory(package.code_name(static_cast<std::size_t>(code)));
#endif
        if (depth >= 128)
            throw Error(ErrorKind::vm, "script call depth exceeded");
        ++depth;
        struct Guard {
            std::size_t &d;
            ~Guard() { --d; }
        } guard{depth};
        const auto &program = linked.program(package, static_cast<std::size_t>(code));
        VariableStore locals;
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            locals["argument" + std::to_string(i)] = arguments[i];
            locals["argument[" + std::to_string(i) + "]"] = arguments[i];
        }
        auto &scratch = vm_scratch[depth - 1];
        auto &stack = scratch.stack;
        struct ScratchGuard {
            VmScratch &scratch;
            ~ScratchGuard() {
                scratch.stack.clear();
                scratch.arguments.clear();
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
        std::size_t pc = 0;
        std::size_t instruction_index = 0;
        auto &environments = scratch.environments;
        auto &environment_count = scratch.environment_count;
        auto pop = [&]() {
            if (stack.empty())
                throw Error(ErrorKind::vm, "native VM stack underflow", program.origin + pc);
            auto v = std::move(stack.back());
            stack.pop_back();
            return v;
        };
        auto push = [&](Value v) {
            if (stack.size() >= 4096)
                throw Error(ErrorKind::vm, "native VM stack limit", program.origin + pc);
            stack.push_back(std::move(v));
        };
        try {
            while (pc < program.byte_length) {
                if (++event_budget > 2'000'000)
                    throw Error(ErrorKind::vm, "event instruction budget exhausted", program.origin + pc);
                ++instruction_count;
#if REGML_PS2
                if (frame == 0 && instruction_count % 10000 == 0)
                    ps2_memory_trace(package.code_name(static_cast<std::size_t>(code)), pc);
#endif
                const auto &ins = program.instructions[instruction_index];
                current_code = code;
                current_vm_address = program.origin + pc;
                std::size_t next = pc + ins.size;
                auto variable = [&](bool writing, Value value = std::monostate{}) -> Value {
                    const auto id = static_cast<std::size_t>(ins.symbol_id);
                    const auto &symbol = linked.variables[id];
                    std::string indexed_name;
                    std::string_view name = symbol.name;
                    const auto kind = (ins.reference_value() >> 24) & 0xf8;
                    int scope = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(ins.word));
                    int array_index = -1;
                    if (kind == 0) {
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
                    if (scope == -9)
                        scope = number(pop());
                    if (!writing && name.starts_with("bbox_")) {
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
                    if (!writing &&
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
                    if (scope == -5)
                        store = &globals;
                    else if (scope == -7 || name.starts_with("argument"))
                        store = &locals;
                    else if (engine.contains(name) || (kind == 0xa0 && ins.opcode() == Opcode::push_builtin &&
                                                       !self->vars.contains(name)))
                        store = &engine;
                    else if (scope == -1 && self)
                        store = &self->vars;
                    else if (scope == -2 && other && other->alive)
                        store = &other->vars;
                    else {
                        multi_target = writing;
                        if (!writing) {
                            auto *found = first_target(scope, self, other);
                            if (!found)
                                throw Error(ErrorKind::vm,
                                            "variable instance not found: " + std::string(name));
                            store = &found->vars;
                        }
                    }
                    auto access = [&](VariableStore *store) -> Value {
                        if (!writing && store == &locals && name == "argument_count" &&
                            !locals.contains(name))
                            return static_cast<std::int32_t>(arguments.size());
                        if (array_index >= 0 && !store->contains(name)) {
                            auto base = store->find(symbol.name);
                            if (base == store->end()) {
                                if (!writing)
                                    throw Error(ErrorKind::vm,
                                                "array variable is uninitialized: " + symbol.name);
                                base = store->emplace(symbol.name, ArrayPtr(new ArrayValue)).first;
                            }
                            auto *array = std::get_if<ArrayPtr>(&base->second);
                            if (writing && (!array || !*array)) {
                                base->second = ArrayPtr(new ArrayValue);
                                array = std::get_if<ArrayPtr>(&base->second);
                            }
                            if (!array || !*array)
                                throw Error(ErrorKind::vm,
                                            "indexed variable is not an array: " + symbol.name);
                            const int row = array_index / 32000, col = array_index % 32000;
                            if (writing) {
                                if (array->use_count() > 1)
                                    *array = ArrayPtr(new ArrayValue(**array));
                                if ((*array)->elements.size() >= 1'000'000)
                                    throw Error(ErrorKind::vm, "array element limit reached");
                                (*array)->elements[array_index] = value;
                                auto &length = (*array)->row_lengths[row];
                                length = std::max<std::int64_t>(length, static_cast<std::int64_t>(col) + 1);
                                return value;
                            }
                            const auto length = (*array)->row_lengths.find(row);
                            if (length == (*array)->row_lengths.end() || col >= length->second)
                                throw Error(ErrorKind::vm,
                                            "array read exceeds allocated row: " + symbol.name);
                            const auto element = (*array)->elements.find(array_index);
                            return element == (*array)->elements.end() ? Value{std::int32_t{0}}
                                                                       : element->second;
                        }
                        if (writing) {
                            (*store)[name] = value;
                            if (store == &engine && name == "room")
                                pending_room = number(value);
                            if (store != &engine && store != &globals && store != &locals) {
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
                        const auto it = store->find(name);
                        if (it == store->end())
                            throw Error(ErrorKind::vm, "uninitialized variable: " + std::string(name));
                        return it->second;
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
                            throw Error(ErrorKind::vm, "variable instance not found: " + std::string(name));
                        return value;
                    }
                    return access(store);
                };
                switch (ins.opcode()) {
                case Opcode::push_i:
                case Opcode::push:
                case Opcode::push_local:
                case Opcode::push_global:
                case Opcode::push_builtin:
                    if (ins.type1 == DataType::variable)
                        push(variable(false));
                    else if (ins.type1 == DataType::string)
                        push(PackageString{package.string_at(ins.reference_value())});
                    else if (ins.immediate_kind == Instruction::ImmediateKind::int32)
                        push(ins.immediate_i32());
                    else if (ins.immediate_kind == Instruction::ImmediateKind::real64)
                        push(ins.immediate_real());
                    else if (ins.immediate_kind == Instruction::ImmediateKind::real32)
                        push(ins.immediate_real());
                    else if (ins.immediate_kind == Instruction::ImmediateKind::int64)
                        push(static_cast<double>(ins.immediate_i64()));
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
                        std::array<Value, 2> address;
                        std::size_t address_count = 0;
                        if (kind == 0) {
                            address[1] = pop();
                            address[0] = pop();
                            address_count = 2;
                        } else if (kind == 0x80) {
                            address[0] = pop();
                            address_count = 1;
                        }
                        auto v = pop();
                        for (std::size_t i = 0; i < address_count; ++i)
                            push(std::move(address[i]));
                        variable(true, v);
                    }
                    break;
                }
                case Opcode::conv: {
                    auto v = pop();
                    if (ins.type2() == DataType::int32)
                        push(static_cast<std::int32_t>(number(v)));
                    else if (ins.type2() == DataType::real)
                        push(value_number(v));
                    else if (ins.type2() == DataType::boolean)
                        push(value_number(v) > 0.5);
                    else
                        push(v);
                    break;
                }
                case Opcode::dup: {
                    const auto count = static_cast<std::size_t>(ins.extra()) + 1;
                    if (count > stack.size())
                        throw Error(ErrorKind::vm, "DUP stack underflow");
                    const auto first = stack.size() - count;
                    for (std::size_t i = 0; i < count; ++i)
                        push(stack[first + i]);
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
                    if (value_is_string(left) && value_is_string(right)) {
                        const auto a = value_string_view(left);
                        const auto b = value_string_view(right);
                        if (ins.opcode() == Opcode::add) {
                            if (a.size() + b.size() > 1'048'576)
                                throw Error(ErrorKind::vm, "string limit");
                            std::string result;
                            result.reserve(a.size() + b.size());
                            result.append(a).append(b);
                            push(std::move(result));
                        } else if (ins.opcode() == Opcode::cmp) {
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
                    const double a = value_number(left), b = value_number(right);
                    switch (ins.opcode()) {
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
                    auto result = call(name, args, self, other);
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
                    if (condition == (ins.opcode() == Opcode::branch_true))
                        next = ins.branch_target_value();
                    break;
                }
                case Opcode::push_env: {
                    const auto target = number(pop());
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
                    if (sub != -1)
                        throw Error(ErrorKind::unsupported,
                                    "native extended opcode pending: " + std::to_string(sub));
                    if (stack.empty() || number(stack.back()) < 0 || number(stack.back()) >= 32000)
                        throw Error(ErrorKind::vm, "legacy two-dimensional array index out of range");
                    break;
                }
                default:
                    throw Error(ErrorKind::unsupported,
                                "native opcode pending: " + std::string(opcode_name(ins.opcode())));
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
        } catch (const Error &e) {
            throw Error(e.kind(),
                        std::string(e.what()) + " in " +
                            std::string(package.code_name(static_cast<std::size_t>(code))),
                        e.offset().value_or(program.origin + pc));
        } catch (const std::bad_alloc &) {
            throw Error(ErrorKind::io,
                        "out of memory in " + std::string(package.code_name(static_cast<std::size_t>(code))),
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
    Value call(std::string_view name, std::span<const Value> args, InstancePtr self, InstancePtr other) {
        auto n = [&](std::size_t i) {
            if (i >= args.size())
                throw Error(ErrorKind::vm, "missing built-in argument");
            return value_number(args[i]);
        };
        auto text = [&](std::size_t i) {
            if (i >= args.size())
                throw Error(ErrorKind::vm, "missing built-in argument");
            return value_text(args[i]);
        };
        auto set_velocity = [&](std::string_view component, double value) {
            self->vars[std::string(component)] = value;
            const double h = value_number(self->vars.at("hspeed")), v = value_number(self->vars.at("vspeed"));
            self->vars["speed"] = std::hypot(h, v);
            if (h != 0 || v != 0)
                self->vars["direction"] = std::fmod(std::atan2(-v, h) * 180 / 3.141592653589793 + 360, 360);
        };
        Canvas &canvas = surface_target == 0 ? this->canvas : surfaces.at(surface_target);
        if (const auto it = script_names.find(name); it != script_names.end())
            return execute(it->second, self, args, other);
        if (name == "script_execute") {
            const auto id = static_cast<std::size_t>(n(0));
            if (id >= scripts.size())
                throw Error(ErrorKind::vm, "script ID out of range");
            return execute(scripts[id], self, args.subspan(1), other);
        }
        if (name == "os_get_language")
            return std::string("en");
        if (name == "date_current_datetime") {
            const auto now = std::chrono::system_clock::now().time_since_epoch();
            return std::chrono::duration<double>(now).count() / 86400.0 + 25569.0;
        }
        if (name == "os_get_region")
            return std::string("US");
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
        if (name == "is_undefined") {
            if (args.empty())
                throw Error(ErrorKind::vm, "is_undefined argument missing");
            return std::holds_alternative<std::monostate>(args[0]);
        }
        if (name == "is_string")
            return value_is_string(args[0]);
        if (name == "is_array")
            return std::holds_alternative<ArrayPtr>(args[0]);
        if (name == "is_real")
            return !value_is_string(args[0]) && !std::holds_alternative<std::monostate>(args[0]);
        if (name == "string_upper" || name == "string_lower") {
            auto str = text(0);
            for (auto &c : str)
                c = static_cast<char>(name == "string_upper" ? std::toupper(static_cast<unsigned char>(c))
                                                             : std::tolower(static_cast<unsigned char>(c)));
            return str;
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
            const auto found = targets(static_cast<int>(n(0)), self, other);
            return !found.empty() && found[0]->vars.contains(text(1));
        }
        if (name == "variable_global_get")
            return globals.at(text(0));
        if (name == "variable_global_set") {
            globals[text(0)] = args[1];
            return std::monostate{};
        }
        if (name == "ds_map_create") {
            const int id = next_ds++;
            maps[id] = {};
            return std::int32_t{id};
        }
        if (name == "ds_map_set" || name == "ds_map_replace" || name == "ds_map_add" ||
            name == "ds_map_set_post") {
            auto &map = maps.at(static_cast<int>(n(0)));
            const auto key = (value_is_string(args[1]) ? "s:" : "n:") + text(1);
            if (name == "ds_map_add" && map.contains(key))
                return false;
            map[key] = args[2];
            return true;
        }
        if (name == "ds_map_exists" || name == "ds_map_find_value") {
            const auto &map = maps.at(static_cast<int>(n(0)));
            const auto key = (value_is_string(args[1]) ? "s:" : "n:") + text(1);
            if (name == "ds_map_exists")
                return map.contains(key);
            const auto it = map.find(key);
            return it == map.end() ? Value{std::monostate{}} : it->second;
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
                if (value_is_string(value))
                    json += quote(value_string_view(value));
                else if (const auto *boolean = std::get_if<bool>(&value))
                    json += *boolean ? "true" : "false";
                else if (std::holds_alternative<std::monostate>(value))
                    json += "null";
                else
                    json += value_text(value);
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
                map["s:" + key] = std::move(value);
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
        if (name == "ds_list_size")
            return static_cast<double>(lists.at(static_cast<int>(n(0))).size());
        if (name == "ds_list_find_value")
            return lists.at(static_cast<int>(n(0))).at(static_cast<std::size_t>(n(1)));
        if (name == "ds_list_destroy") {
            lists.erase(static_cast<int>(n(0)));
            return std::monostate{};
        }
        if (name == "buffer_create") {
            const auto size = static_cast<std::size_t>(std::clamp(n(0), 0.0, 64.0 * 1024 * 1024));
            const int id = next_buffer++;
            buffers.emplace(id, GameBuffer{std::vector<std::uint8_t>(size), 0});
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
        if (name == "audio_channel_num") {
            audio.channel_limit(static_cast<int>(n(0)));
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
        if (name == "sprite_get_number")
            return static_cast<double>(assets.sprite(static_cast<int>(n(0))).frames.size());
        if (name == "sprite_exists")
            return assets.sprite_exists(static_cast<int>(n(0)));
        if (name == "sprite_get_name")
            return std::string(assets.sprite_name(static_cast<int>(n(0))));
        if (name == "sprite_create_from_surface")
            return std::int32_t{-1};
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
        if (name == "draw_set_font") {
            draw_font = static_cast<int>(n(0));
            return std::monostate{};
        }
        if (name == "draw_set_halign") {
            halign = static_cast<int>(n(0));
            return std::monostate{};
        }
        if (name == "draw_set_valign") {
            valign = static_cast<int>(n(0));
            return std::monostate{};
        }
        if (name == "draw_text") {
            assets.draw_text(canvas, draw_font, n(0), n(1), text(2), draw_color, draw_alpha, halign, valign);
            return std::monostate{};
        }
        if (name == "draw_text_transformed") {
            assets.draw_text(canvas, draw_font, n(0), n(1), text(2), draw_color, draw_alpha, halign, valign,
                             n(3), n(4), n(5));
            return std::monostate{};
        }
        if (name == "draw_text_transformed_color") {
            assets.draw_text(canvas, draw_font, n(0), n(1), text(2), static_cast<unsigned>(n(6)), draw_alpha,
                             halign, valign, n(3), n(4), n(5));
            return std::monostate{};
        }
        if (name == "draw_text_ext") {
            const auto value = text(2);
            const double line_height = n(3) < 0 ? assets.text_height(draw_font) : n(3);
            const double max_width = n(4);
            std::string line;
            double line_width = 0;
            double y = n(1);
            for (const char ch : value) {
                const double char_width =
                    ch == '\n' || ch == '#' ? 0 : assets.text_width(draw_font, {&ch, 1});
                if ((ch == ' ' && max_width > 0 && line_width + char_width > max_width) || ch == '\n' ||
                    ch == '#') {
                    assets.draw_text(canvas, draw_font, n(0), y, line, draw_color, draw_alpha, halign,
                                     valign);
                    line.clear();
                    line_width = 0;
                    y += line_height;
                    if (ch == '\n' || ch == '#')
                        continue;
                }
                line += ch;
                line_width += char_width;
            }
            assets.draw_text(canvas, draw_font, n(0), y, line, draw_color, draw_alpha, halign, valign);
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
            return std::int32_t{id};
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
        if (name == "string_width")
            return assets.text_width(draw_font, text(0));
        if (name == "string_height")
            return assets.text_height(draw_font);
        if (name == "draw_rectangle") {
            assets.rectangle(canvas, n(0), n(1), n(2), n(3), n(4) != 0);
            return std::monostate{};
        }
        if (name == "ossafe_fill_rectangle") {
            assets.rectangle(canvas, n(0), n(1), n(2), n(3), false);
            return std::monostate{};
        }
        if (name == "draw_circle") {
            canvas.circle(n(0), n(1), n(2), n(3) != 0);
            return std::monostate{};
        }
        if (name == "draw_line" || name == "draw_line_width" || name == "draw_line_color" ||
            name == "draw_line_width_color") {
            const bool thick = name == "draw_line_width" || name == "draw_line_width_color";
            const bool colored = name == "draw_line_color" || name == "draw_line_width_color";
            if (colored) {
                draw_color = static_cast<unsigned>(n(thick ? 5 : 4));
                canvas.color(draw_color);
            }
            canvas.line(n(0), n(1), n(2), n(3), thick ? n(4) : 1);
            return std::monostate{};
        }
        if (name == "draw_triangle" || name == "draw_triangle_color") {
            const bool colored = name == "draw_triangle_color";
            const unsigned outline_index = colored ? 9U : 6U;
            if (colored) {
                const auto c0 = static_cast<unsigned>(n(6)), c1 = static_cast<unsigned>(n(7)),
                           c2 = static_cast<unsigned>(n(8));
                draw_color = ((c0 & 0x00fefefeU) + (c1 & 0x00fefefeU) + (c2 & 0x00fefefeU)) / 3;
                canvas.color(draw_color);
            }
            canvas.triangle(n(0), n(1), n(2), n(3), n(4), n(5), n(outline_index) != 0);
            return std::monostate{};
        }
        if (name == "draw_ellipse_color") {
            draw_color = static_cast<unsigned>(n(4));
            canvas.color(draw_color);
            canvas.circle((n(0) + n(2)) / 2, (n(1) + n(3)) / 2, std::max(n(2) - n(0), n(3) - n(1)) / 2,
                          n(6) != 0);
            return std::monostate{};
        }
        if (name == "draw_roundrect") {
            assets.rectangle(canvas, n(0), n(1), n(2), n(3), n(4) != 0);
            return std::monostate{};
        }
        if (name == "draw_rectangle_color") {
            draw_color = static_cast<unsigned>(n(4));
            canvas.color(draw_color);
            assets.rectangle(canvas, n(0), n(1), n(2), n(3), n(8) != 0);
            return std::monostate{};
        }
        if (name == "draw_point_color") {
            draw_color = static_cast<unsigned>(n(2));
            canvas.color(draw_color);
            canvas.line(n(0), n(1), n(0), n(1));
            return std::monostate{};
        }
        if (name == "draw_enable_alphablend" || name == "draw_set_circle_precision")
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
        if (name == "gamepad_is_connected")
            return false;
        if (name == "joystick_exists")
            return false;
        if (name == "gamepad_button_check" || name == "joystick_check_button" || name == "joystick_buttons" ||
            name == "joystick_has_pov")
            return false;
        if (name == "joystick_pov" || name == "joystick_direction" || name == "gamepad_axis_value" ||
            name == "joystick_xpos" || name == "joystick_ypos")
            return 0.0;
        if (name == "sqr")
            return n(0) * n(0);
        if (name == "power")
            return std::pow(n(0), n(1));
        if (name == "abs")
            return std::abs(n(0));
        if (name == "floor")
            return std::floor(n(0));
        if (name == "ceil")
            return std::ceil(n(0));
        if (name == "round")
            return std::nearbyint(n(0));
        if (name == "sin")
            return std::sin(n(0));
        if (name == "cos")
            return std::cos(n(0));
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
        if (name == "window_get_width")
            return static_cast<double>(canvas.logical_width());
        if (name == "window_get_height")
            return static_cast<double>(canvas.logical_height());
        if (name == "window_get_x")
            return engine.contains("window_x") ? engine.at("window_x") : Value{0.0};
        if (name == "window_get_y")
            return engine.contains("window_y") ? engine.at("window_y") : Value{0.0};
        if (name == "window_get_fullscreen")
            return engine.contains("window_fullscreen") && value_number(engine.at("window_fullscreen")) != 0;
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
        const char *allocation_stage = "audio service";
        try {
            audio.service();
            static const std::array<std::string, 12> alarm_names = {
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
                            distance += value_number(i->vars.at("path_speed")) * sample(distance).speed /
                                        100 / std::max(std::abs(scale), 0.000001);
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
                            const auto before = sample(position * path.length);
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
            allocation_stage = "collision and end-step events";
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
            for (const auto &i : snapshot)
                if (i->alive && objects[static_cast<std::size_t>(i->object)].has_collision_events)
                    for (const auto &j : snapshot)
                        if (j->alive && i != j) {
                            const auto route = collision_event(i->object, j->object);
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
            canvas.clear(rooms.at(static_cast<std::size_t>(current_room)).color);
            if (number(engine.at("view_enabled")) != 0 && number(engine.at("view_visible[0]")) != 0) {
                const auto w = value_number(engine.at("view_wview[0]")),
                           h = value_number(engine.at("view_hview[0]"));
                if (w <= 0 || h <= 0)
                    throw Error(ErrorKind::vm, "invalid view dimensions");
                assets.view(
                    value_number(engine.at("view_xview[0]")), value_number(engine.at("view_yview[0]")),
                    value_number(engine.at("view_wport[0]")) / w,
                    value_number(engine.at("view_hport[0]")) / h, value_number(engine.at("view_xport[0]")),
                    value_number(engine.at("view_yport[0]")));
            } else
                assets.view(0, 0, 1, 1, 0, 0);
            const auto &room = rooms.at(static_cast<std::size_t>(current_room));
            allocation_stage = "background and tile drawing";
            for (const auto &b : room.backgrounds)
                if (!b.foreground)
                    assets.draw_background(canvas, b.id, b.x, b.y);
            auto draw_tile = [&](const Tile &t) {
                if (hidden_tile_layers.contains(t.id))
                    return;
                const auto offset = tile_layer_offsets.find(t.id);
                const double ox = offset == tile_layer_offsets.end() ? 0 : offset->second.first;
                const double oy = offset == tile_layer_offsets.end() ? 0 : offset->second.second;
                assets.draw_tile(canvas, t.id, t.source_x, t.source_y, t.width, t.height, t.x + ox, t.y + oy,
                                 t.sx, t.sy, t.color);
            };
            std::size_t tile = 0;
            allocation_stage = "draw-list allocation";
            draw_entries.clear();
            if (draw_entries.capacity() < snapshot.size())
                draw_entries.reserve(snapshot.size());
            for (std::size_t index = 0; index < snapshot.size(); ++index) {
                const auto &instance = snapshot[index];
                draw_entries.push_back({value_number(instance->vars.at("depth")), index});
            }
            std::sort(draw_entries.begin(), draw_entries.end(), [](const auto &a, const auto &b) {
                return a.depth != b.depth ? a.depth > b.depth : a.order < b.order;
            });
            allocation_stage = "instance draw calls";
            for (auto &entry : draw_entries) {
                const auto &i = snapshot[entry.order];
                if (i->alive && !i->deactivated && number(i->vars.at("visible"))) {
                    while (tile < room.tiles.size() &&
                           room.tiles[tile].depth >= value_number(i->vars.at("depth")))
                        draw_tile(room.tiles[tile++]);
                    if (objects[static_cast<std::size_t>(i->object)].draw_owner >= 0)
                        event(i, 8, 0);
                    else {
                        const auto sprite = number(i->vars.at("sprite_index"));
                        if (sprite >= 0)
                            assets.draw_sprite(canvas, sprite, value_number(i->vars.at("image_index")),
                                               value_number(i->vars.at("x")), value_number(i->vars.at("y")),
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
                }
            }
            while (tile < room.tiles.size())
                draw_tile(room.tiles[tile++]);
            for (const auto &b : room.backgrounds)
                if (b.foreground)
                    assets.draw_background(canvas, b.id, b.x, b.y);
            std::erase_if(instances, [](const auto &i) { return !i->alive; });
            ++frame;
            engine["current_time"] = static_cast<double>(frame) * 1000.0 / value_number(engine["room_speed"]);
        } catch (const std::bad_alloc &) {
            throw Error(ErrorKind::io, "out of memory during " + std::string(allocation_stage));
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
    impl_->audio.mix(output, playback_rate);
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
