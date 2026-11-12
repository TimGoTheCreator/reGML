#include "game.hpp"
#include "audio/sdl/audio.hpp"
#include "bit_cast.hpp"
#include "gmrt_ir.hpp"
#include "gmrt_project.hpp"
#include "gmrt_vm.hpp"
#include "gmrt_wad.hpp"
#include "hash.hpp"
#include "ini.hpp"
#include "instance_ptr.hpp"
#include "interpreter.hpp"
#include "json.hpp"
#include "linker.hpp"
#include "lookup.hpp"
#include "rendersoftware/assets.hpp"
#include "video/video_player.hpp"
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
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <queue>
#include <random>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#if defined(__GNUC__) || defined(__clang__)
#define REGML_NOINLINE_LAMBDA __attribute__((noinline))
#else
#define REGML_NOINLINE_LAMBDA
#endif
#if REGML_WITH_SDL2
#include <SDL2/SDL.h>
#endif
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
template <class Map, class Key> auto heterogeneous_find(Map &map, const Key &key) {
    const auto bucket_count = map.bucket_count();
    if (bucket_count == 0)
        return map.end();
    const auto bucket = map.hash_function()(key) % bucket_count;
    for (auto local = map.begin(bucket); local != map.end(bucket); ++local) {
        if (map.key_eq()(local->first, key))
            return map.find(local->first);
    }
    return map.end();
}

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
#if REGML_WIIU
void wiiu_load_trace(std::string_view stage) {
    static const bool enabled = std::filesystem::exists("/vol/external01/reGML/regml-trace.enabled");
    if (!enabled)
        return;
    static std::ofstream log("/vol/external01/reGML/regml-session-trace.log", std::ios::trunc);
    if (log) {
        log << stage << '\n';
        log.flush();
    }
}
#else
void wiiu_load_trace(std::string_view) {}
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
#if REGML_WIIU
    wiiu_load_trace(std::string("begin:") + std::string(name));
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
        throw Error(ErrorKind::vm,
                    "integer value out of range: " + value_text(v) + " (expected " +
                        std::to_string(INT32_MIN) + " through " + std::to_string(INT32_MAX) + ")");
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
constexpr int event_draw = 8;
constexpr int event_draw_normal = 0;
constexpr int event_draw_gui = 64;
constexpr int event_draw_begin = 72;
constexpr int event_draw_end = 73;
constexpr int event_draw_gui_begin = 74;
constexpr int event_draw_gui_end = 75;
constexpr int event_draw_pre = 76;
constexpr int event_draw_post = 77;
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
    std::string_view name;
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
    int tile_set = -1, tile_width = 0, tile_height = 0;
    int tilemap_width = 0, tilemap_height = 0;
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
    int cell_x = -1, cell_y = -1;
    double x = 0, y = 0;
    int source_x = 0, source_y = 0, width = 0, height = 0, depth = 0;
    double sx = 1, sy = 1;
    unsigned color = 0xffffffff;
    int layer_id = -1;
    std::uint32_t transform = 0;
    std::uint32_t tile_data = 0;
    std::uint32_t tileset_index = 0;
    bool modern_tileset = false, sprite_tile = false, visible = true;
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
using Matrix4 = std::array<double, 16>;
constexpr Matrix4 identity_matrix() {
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}
struct Camera {
    double x = 0, y = 0, width = 0, height = 0, angle = 0;
    double border_x = 0, border_y = 0, speed_x = -1, speed_y = -1;
    int target = -1;
    Matrix4 view_matrix = identity_matrix(), projection_matrix = identity_matrix();
};
struct PathPoint {
    double x = 0, y = 0, speed = 100, distance = 0;
};
struct Path {
    bool smooth = false, closed = false, valid = true;
    unsigned precision = 4;
    double length = 0;
    std::vector<PathPoint> control_points;
    std::vector<PathPoint> points;
};
struct MotionPlanningGrid {
    double x = 0, y = 0, cell_width = 1, cell_height = 1;
    int columns = 0, rows = 0;
    std::vector<std::uint8_t> blocked;
};
struct VertexPoint {
    double x = 0, y = 0, z = 0, u = 0, v = 0, alpha = 1;
    double nx = 0, ny = 0, nz = 0;
    unsigned color = 0xffffff;
};
struct VertexBufferData {
    std::vector<VertexPoint> vertices;
    std::vector<int> attributes;
    VertexPoint current;
    std::size_t next_attribute = 0;
    int format = 0;
    bool building = false;
};
struct DataGrid {
    int width = 0, height = 0;
    std::vector<Value> cells;
};
void rebuild_path(Path &path) {
    path.points.clear();
    path.length = 0;
    if (!path.valid || path.control_points.empty())
        return;

    std::vector<PathPoint> controls = path.control_points;
    if (path.closed && controls.size() > 1)
        controls.push_back(controls.front());

    if (path.smooth && controls.size() > 1) {
        const int subdivisions = static_cast<int>(std::clamp(path.precision, 1U, 8U) * 8U);
        path.points.reserve((controls.size() - 1) * static_cast<std::size_t>(subdivisions + 1) + 1);
        for (std::size_t segment = 0; segment + 1 < controls.size(); ++segment) {
            const auto &p1 = controls[segment];
            const auto &p2 = controls[segment + 1];
            const auto &p0 =
                segment ? controls[segment - 1] : (path.closed ? controls[controls.size() - 2] : p1);
            const auto &p3 =
                segment + 2 < controls.size() ? controls[segment + 2] : (path.closed ? controls[1] : p2);
            for (int sample = 0; sample < subdivisions; ++sample) {
                const double t = static_cast<double>(sample) / subdivisions, t2 = t * t, t3 = t2 * t;
                const double x =
                    0.5 * ((2 * p1.x) + (-p0.x + p2.x) * t + (2 * p0.x - 5 * p1.x + 4 * p2.x - p3.x) * t2 +
                           (-p0.x + 3 * p1.x - 3 * p2.x + p3.x) * t3);
                const double y =
                    0.5 * ((2 * p1.y) + (-p0.y + p2.y) * t + (2 * p0.y - 5 * p1.y + 4 * p2.y - p3.y) * t2 +
                           (-p0.y + 3 * p1.y - 3 * p2.y + p3.y) * t3);
                const double speed = p1.speed + (p2.speed - p1.speed) * t;
                path.points.push_back({x, y, speed, 0});
            }
        }
        path.points.push_back(controls.back());
    } else {
        path.points = std::move(controls);
    }

    for (std::size_t i = 0; i < path.points.size(); ++i) {
        if (i)
            path.length +=
                std::hypot(path.points[i].x - path.points[i - 1].x, path.points[i].y - path.points[i - 1].y);
        path.points[i].distance = path.length;
    }
}
struct SpatialCellMembership {
    std::uint64_t cell = 0;
    std::size_t slot = 0;
};
struct Instance {
    int id = 0, object = 0;
    std::size_t references = 0;
    std::size_t spatial_order = 0;
    std::uint32_t spatial_query_generation = 0;
    bool alive = true, outside_room = false, deactivated = false, manual_depth = false, destroying = false;
    bool spatial_large = false;
    VariableStore vars;
    std::vector<SpatialCellMembership> spatial_cells;
};
using InstancePtr = regml::IntrusivePtr<Instance>;
struct GameBuffer {
    std::vector<std::uint8_t> bytes;
    std::size_t position = 0;
    int type = 0;
};
struct ParticleTypeState {
    int sprite = -1, life_min = 60, life_max = 60, death_type = -1, death_count = 0;
    double direction_min = 0, direction_max = 360, speed_min = 0, speed_max = 0;
    double size_min = 1, size_max = 1, scale_x = 1, scale_y = 1, gravity = 0, gravity_direction = 270;
    std::array<double, 3> alpha{1, 1, 1};
    bool animated = false, stretch = false, random_start = false, additive = false;
};
struct ParticleState {
    int type = -1, life = 1, age = 0;
    double x = 0, y = 0, direction = 0, speed = 0, size = 1, image_index = 0;
};
struct ParticleSystemState {
    int depth = 0;
    bool automatic_draw = true, automatic_update = true;
    std::vector<ParticleState> particles;
};
struct ParticleEmitterState {
    int system = -1, shape = 0, distribution = 0, stream_type = -1, stream_count = 0;
    double x_min = 0, x_max = 0, y_min = 0, y_max = 0;
    bool relative = false;
};
struct AudioEmitterState {
    double x = 0, y = 0, z = 0, vx = 0, vy = 0, vz = 0, gain = 1, pitch = 1;
    double falloff_ref = 100, falloff_max = 100, falloff_factor = 1;
    std::unordered_map<int, std::array<double, 2>> voices;
};
#if REGML_WITH_SDL2 && !REGML_WEB
struct AudioRecorderInput {
    SDL_AudioDeviceID device = 0;
    SDL_AudioSpec format{};
    std::vector<Uint8> pending;
    std::uint64_t resample_phase = 0;
};
#endif
struct AnimationCurvePoint {
    double x = 0, value = 0, bezier_x0 = 0, bezier_y0 = 0, bezier_x1 = 0, bezier_y1 = 0;
};
struct AnimationCurveChannel {
    std::string_view name;
    int type = 0;
    std::uint32_t iterations = 0;
    std::vector<AnimationCurvePoint> points;
};
struct AnimationCurve {
    std::string_view name;
    std::vector<AnimationCurveChannel> channels;
};
struct GifFrame {
    std::vector<std::uint8_t> indices;
    std::uint16_t delay = 6;
};
struct GifCapture {
    int width = 0, height = 0;
    std::uint32_t clear_rgb = 0;
    std::size_t bytes = 0;
    std::vector<GifFrame> frames;
};
#if REGML_PS2 || REGML_LOW_MEMORY
constexpr std::size_t gif_capture_limit = 4 * 1024 * 1024;
#else
constexpr std::size_t gif_capture_limit = 32 * 1024 * 1024;
#endif
std::uint8_t gif_palette_index(std::uint32_t rgb) {
    return static_cast<std::uint8_t>((((rgb >> 16) & 255) >> 5) << 5 | (((rgb >> 8) & 255) >> 5) << 2 |
                                     ((rgb & 255) >> 6));
}
std::vector<std::uint8_t> gif_lzw(std::span<const std::uint8_t> indices) {
    std::vector<std::uint8_t> output;
    if (indices.empty())
        return output;
    output.reserve(indices.size());
    std::uint32_t bit_buffer = 0;
    unsigned bit_count = 0;
    auto write_code = [&](std::uint16_t code) {
        bit_buffer |= static_cast<std::uint32_t>(code) << bit_count;
        bit_count += 9;
        while (bit_count >= 8) {
            output.push_back(static_cast<std::uint8_t>(bit_buffer));
            bit_buffer >>= 8;
            bit_count -= 8;
        }
    };
    constexpr std::uint16_t clear_code = 256, end_code = 257;
    std::unordered_map<std::uint32_t, std::uint16_t> dictionary;
    dictionary.reserve(260);
    std::uint16_t next_code = 258;
    write_code(clear_code);
    std::uint16_t prefix = indices.front();
    for (std::size_t i = 1; i < indices.size(); ++i) {
        const auto suffix = indices[i];
        const auto key = (static_cast<std::uint32_t>(prefix) << 8) | suffix;
        if (const auto found = dictionary.find(key); found != dictionary.end()) {
            prefix = found->second;
            continue;
        }
        write_code(prefix);
        if (next_code < 512) {
            dictionary.emplace(key, next_code++);
        } else {
            write_code(clear_code);
            dictionary.clear();
            next_code = 258;
        }
        prefix = suffix;
    }
    write_code(prefix);
    write_code(end_code);
    if (bit_count)
        output.push_back(static_cast<std::uint8_t>(bit_buffer));
    return output;
}
bool write_gif(const GifCapture &capture, const std::filesystem::path &path) {
    if (capture.width <= 0 || capture.height <= 0 || capture.frames.empty())
        return false;
    std::ofstream output(path, std::ios::binary);
    if (!output)
        return false;
    auto byte = [&](std::uint8_t value) { output.put(static_cast<char>(value)); };
    auto word = [&](std::uint16_t value) {
        byte(static_cast<std::uint8_t>(value));
        byte(static_cast<std::uint8_t>(value >> 8));
    };
    output.write("GIF89a", 6);
    word(static_cast<std::uint16_t>(capture.width));
    word(static_cast<std::uint16_t>(capture.height));
    byte(0xf7);
    byte(gif_palette_index(capture.clear_rgb));
    byte(0);
    for (unsigned index = 0; index < 256; ++index) {
        const auto red = ((index >> 5) & 7) * 255 / 7;
        const auto green = ((index >> 2) & 7) * 255 / 7;
        const auto blue = (index & 3) * 255 / 3;
        byte(static_cast<std::uint8_t>(red));
        byte(static_cast<std::uint8_t>(green));
        byte(static_cast<std::uint8_t>(blue));
    }
    byte(0x21);
    byte(0xff);
    byte(11);
    output.write("NETSCAPE2.0", 11);
    byte(3);
    byte(1);
    word(0);
    byte(0);
    for (const auto &frame : capture.frames) {
        if (frame.indices.size() != static_cast<std::size_t>(capture.width) * capture.height)
            return false;
        byte(0x21);
        byte(0xf9);
        byte(4);
        byte(4);
        word(frame.delay);
        byte(0);
        byte(0);
        byte(0x2c);
        word(0);
        word(0);
        word(static_cast<std::uint16_t>(capture.width));
        word(static_cast<std::uint16_t>(capture.height));
        byte(0);
        byte(8);
        const auto compressed = gif_lzw(frame.indices);
        for (std::size_t offset = 0; offset < compressed.size();) {
            const auto count = std::min<std::size_t>(255, compressed.size() - offset);
            byte(static_cast<std::uint8_t>(count));
            output.write(reinterpret_cast<const char *>(compressed.data() + offset),
                         static_cast<std::streamsize>(count));
            offset += count;
        }
        byte(0);
    }
    byte(0x3b);
    output.flush();
    return output.good();
}
ArrayPtr matrix_value(const Matrix4 &matrix) {
    ArrayPtr result(new ArrayValue);
    result->row_lengths[0] = 16;
    for (std::size_t i = 0; i < matrix.size(); ++i)
        result->elements[static_cast<std::int32_t>(i)] = matrix[i];
    return result;
}
Matrix4 matrix_from_value(const Value &value) {
    const auto *array = std::get_if<ArrayPtr>(&value);
    if (!array || !*array)
        throw Error(ErrorKind::vm, "matrix argument must be a 16-element array");
    Matrix4 result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto *element = (*array)->elements.find(static_cast<std::int32_t>(i));
        if (!element)
            throw Error(ErrorKind::vm, "matrix argument must contain 16 values");
        result[i] = value_number(*element);
    }
    return result;
}
Matrix4 multiply_matrix(const Matrix4 &first, const Matrix4 &second) {
    Matrix4 result{};
    for (std::size_t column = 0; column < 4; ++column)
        for (std::size_t row = 0; row < 4; ++row)
            for (std::size_t inner = 0; inner < 4; ++inner)
                result[column * 4 + row] += second[inner * 4 + row] * first[column * 4 + inner];
    return result;
}
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
    std::vector<AnimationCurve> animation_curves;
    std::unordered_map<std::string, int> shader_uniform_ids;
    std::unordered_map<int, ShaderUniform> shader_uniforms;
    int next_shader_uniform = 1;
    std::map<std::pair<int, int>, int> sprite_texture_ids;
    std::unordered_map<int, std::pair<int, int>> sprite_textures;
    std::unordered_map<int, int> surface_texture_ids;
    std::unordered_map<int, int> texture_surfaces;
    std::unordered_set<std::string> loaded_texture_groups;
    std::unordered_map<int, int> shader_texture_stages;
    int next_sprite_texture = 1;
    int active_shader = -1;
    int shoujo_shader = -1, shoujo_time_uniform = -1, shoujo_width_uniform = -1;
    std::array<int, 3> shoujo_sampler_uniforms{-1, -1, -1};
    std::array<bool, 3> shoujo_texture_failed{};
    std::atomic<float> audio_master_gain{1.0f};
    Canvas canvas{640, 480, true};
    std::vector<Path> paths;
    int next_mp_grid_id = 0;
    std::unordered_map<int, MotionPlanningGrid> mp_grids;
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
    struct CollisionGridEntry {
        std::uint64_t cell = 0;
        std::size_t instance_index = 0;
    };
    std::vector<CollisionGridEntry> collision_grid_entries;
    std::vector<std::size_t> collision_grid_large_instances, collision_query_indices;
    std::vector<std::uint32_t> collision_candidate_stamps;
    std::size_t collision_target_instance_count = 0;
    std::uint32_t collision_candidate_generation = 0;
    bool collision_grid_dirty = true;
    bool collision_pass_active = false;
    std::uint64_t collision_geometry_generation = 0;
    std::vector<Room> rooms;
    std::unordered_map<int, Camera> cameras;
    std::array<Matrix4, 3> transform_matrices{identity_matrix(), identity_matrix(), identity_matrix()};
    int next_camera_id = 8, active_camera_id = 0;
    std::vector<int> order, scripts, global_initializers;
    std::unordered_map<std::string_view, int, VariableStoreHash, std::equal_to<>> code_ids;
    std::unordered_map<std::string, int, VariableStoreHash, std::equal_to<>> script_names;
    // Names refer to the persistent linked-package and GMRT function tables.
    std::unordered_map<std::string_view, std::uint8_t, VariableStoreHash, std::equal_to<>>
        builtin_dispatch_groups;
    std::vector<InstancePtr> instances;
    std::vector<std::vector<std::size_t>> object_instance_indices;
    std::unordered_map<int, std::size_t> instance_id_indices;
    std::unordered_map<const VariableStore *, Instance *> instance_store_owners;
    std::vector<std::uint32_t> object_ancestry_stamps;
    std::uint32_t object_ancestry_generation = 0;
    bool object_instance_index_dirty = true;
    std::unordered_map<std::uint64_t, std::vector<Instance *>> spatial_grid;
    std::vector<Instance *> spatial_large_instances, spatial_query_candidates;
    std::uint32_t spatial_query_generation = 0;
    bool spatial_grid_built = false;
    std::vector<InstancePtr> tick_instances;
    std::vector<InstancePtr> frame_instances;
    std::vector<InstancePtr> collision_instances;
    std::vector<InstancePtr> target_fallback;
    std::vector<InstancePtr> placed_instances;
    struct DrawEntry {
        double depth;
        std::size_t order;
        const Instance *instance;
    };
    struct LayerVisualEntry {
        double depth = 0;
        std::size_t layer_index = 0, element_index = 0, order = 0;
        int layer_id = -1, element_id = -1;
        bool background = false;
    };
    struct ParticleVisualEntry {
        double depth = 0;
        int system = -1;
        std::size_t order = 0;
    };
    std::vector<DrawEntry> draw_entries;
    std::vector<LayerVisualEntry> layer_visuals;
    std::vector<ParticleVisualEntry> particle_visuals;
    struct SavedRoom {
        std::vector<InstancePtr> instances;
        VariableStore engine;
    };
    std::unordered_map<int, SavedRoom> saved_rooms;
    VariableStore globals, engine;
    std::unordered_map<int, VariableStore> static_variables;
    std::unordered_set<int> static_initialized;
    KeyboardState held{}, pressed{}, previous_held{}, released{}, physical_held{}, cleared_until_release{};
    GamepadState gamepad{};
    double gamepad_axis_deadzone = 0.2;
    std::array<bool, 16> gamepad_previous{}, gamepad_released{};
    // ds maps can have any game data as keys, so they need their own storage
    // instead of sharing the bytecode name pool
    struct DynamicValue {
        using Storage = std::variant<std::int32_t,
                                     double,
                                     bool,
                                     PackageString,
                                     std::monostate,
                                     ArrayPtr,
                                     FunctionReference,
                                     CallablePtr,
                                     StructPtr,
                                     std::shared_ptr<std::string>>;
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
            return stored.size() == query.value.size() + 2 && stored[0] == (query.string_key ? 's' : 'n') &&
                   stored[1] == ':' && stored.substr(2) == query.value;
        }

        bool operator()(DynamicMapKeyView query, std::string_view stored) const noexcept {
            return (*this)(stored, query);
        }
    };
    struct DynamicMap : std::unordered_map<std::string, DynamicValue, DynamicMapHash, DynamicMapEqual> {
        using Base = std::unordered_map<std::string, DynamicValue, DynamicMapHash, DynamicMapEqual>;

        DynamicMap() { this->max_load_factor(1.5f); }

        bool set(DynamicMapKeyView key, const Value &value, bool only_if_absent) {
            if (const auto found = heterogeneous_find(*this, key); found != this->end()) {
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
    std::unordered_map<int, std::vector<Value>> ds_queues, ds_stacks;
    std::unordered_map<int, DataGrid> ds_grids;
    struct PriorityItem {
        Value value;
        double priority = 0;
    };
    std::unordered_map<int, std::vector<PriorityItem>> priorities;
    std::unordered_map<int, GameBuffer> buffers;
    std::unordered_map<int, ParticleTypeState> particle_types;
    std::unordered_map<int, ParticleSystemState> particle_systems;
    std::unordered_map<int, ParticleEmitterState> particle_emitters;
    std::unordered_map<int, AudioEmitterState> audio_emitters;
#if REGML_WITH_SDL2 && !REGML_WEB
    std::unordered_map<int, AudioRecorderInput> audio_recorders;
    int next_audio_recording_channel = 1;
#endif
    std::array<double, 3> audio_listener_position{};
    std::array<double, 6> audio_listener_orientation{0, 0, 1, 0, -1, 0};
    int audio_falloff_model = 0;
    struct DelayedCall {
        int id = -1;
        std::size_t due_frame = 0, period_frames = 0;
        Value callback = std::monostate{};
        InstancePtr self, other;
        StructPtr struct_self;
        bool loop = false;
    };
    std::vector<DelayedCall> delayed_calls;
    std::unordered_map<int, Canvas> surfaces;
    int next_surface = 1, surface_target = 0;
    std::vector<int> surface_target_stack;
    VideoPlayer video_player;
    int video_surface = -1;
    bool video_start_event_sent = false;
    bool video_end_event_sent = false;
    bool surface_depth_disabled = false;
    std::unordered_map<int, GifCapture> gif_captures;
    int next_gif_id = 1;
    std::unordered_set<int> hidden_tile_layers;
    std::unordered_map<int, std::pair<double, double>> tile_layer_offsets;
    int next_layer_element_id = 1'000'000, next_dynamic_layer_id = 1'000'000;
    bool vertex_format_building = false, force_draw_depth = false;
    bool gpu_texture_repeat = false;
    bool gui_draw_active = false;
    int next_vertex_format = 1, next_vertex_buffer = 1, active_vertex_buffer = -1, forced_depth = 0;
    std::vector<int> vertex_format_attributes;
    std::unordered_map<int, std::vector<int>> vertex_formats;
    std::unordered_map<int, VertexBufferData> vertex_buffers;
    int next_ds = 0;
    int next_buffer = 1, next_async_request = 1, next_delayed_call = 1;
    int next_particle_type = 1, next_particle_system = 1, next_particle_emitter = 1, next_audio_emitter = 1;
    std::unordered_map<int, std::unique_ptr<std::fstream>> files;
    int next_file = 0;
    std::filesystem::path save_dir;
    IniFile ini;
    unsigned draw_color = 0xffffff;
    double draw_alpha = 1;
    std::array<bool, 4> color_write_enable{true, true, true, true};
    bool blend_enabled = true;
    int gpu_blend_mode = 0;
    std::array<int, 4> gpu_blend_factors{5, 6, 5, 6};
    std::array<int, 2> gpu_blend_equations{0, 0};
    bool alpha_test_enabled = false;
    unsigned alpha_test_reference = 0;
    double comparison_epsilon = 0.00001;
    int draw_font = -1, halign = 0, valign = 0;
    std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();
    std::size_t frame = 0, instruction_count = 0, depth = 0, event_budget = 0;
    bool graphics_prefetch_pending = false;
    std::array<std::size_t, 128> active_program_stack{};
    bool profile_tick_timing = false;
    double last_logic_ms = 0, last_draw_ms = 0;
    std::array<double, 6> last_draw_phases_ms{};
    double last_draw_depth_visual_ms = 0, last_draw_instance_body_ms = 0;
    std::string last_draw_object_hotspots;
    std::array<double, 3> last_draw_visual_kind_ms{};
    std::array<std::uint64_t, 3> last_draw_visual_kind_calls{};
    std::string last_draw_layer_hotspots;
    double last_draw_event_ms = 0, last_draw_builtin_ms = 0;
    std::array<double, 4> last_draw_builtin_categories{};
    double max_draw_builtin_call_ms = 0;
    std::string_view slowest_draw_builtin;
    std::string slowest_draw_builtin_details;
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
        struct BranchCacheEntry {
            std::uint32_t offset = std::numeric_limits<std::uint32_t>::max();
            std::uint32_t instruction_index = 0;
        };
        std::vector<Value> stack, arguments;
        std::vector<InstancePtr> targets;
        std::vector<DataType> stack_types;
        std::array<VariableCacheEntry, 64> variable_cache{};
        std::array<BranchCacheEntry, 8> branch_cache{};
        std::uint32_t variable_cache_generation = 0;
        struct Environment {
            InstancePtr saved, saved_other;
            StructPtr saved_struct;
            std::vector<InstancePtr> choices;
            std::size_t index = 0;
            bool struct_scope = false;
        };
        std::vector<Environment> environments;
        std::size_t environment_count = 0;
    };
    std::array<VmScratch, 128> vm_scratch;
    std::filesystem::path game_dir, package_path;
    std::optional<std::uint32_t> fixed_seed;
    std::optional<std::filesystem::path> pending_game_change;
    mutable std::unordered_map<std::string, std::vector<std::uint8_t>> load_chunks;
    void update_gui_transform() {
        const double width = engine.contains("gui_width") ? value_number(engine.at("gui_width"))
                                                          : static_cast<double>(render_width);
        const double height = engine.contains("gui_height") ? value_number(engine.at("gui_height"))
                                                            : static_cast<double>(render_height);
        const double scale_x = std::isfinite(width) && width > 0 ? canvas.logical_width() / width : 1.0;
        const double scale_y = std::isfinite(height) && height > 0 ? canvas.logical_height() / height : 1.0;
        assets.view(0, 0, scale_x, scale_y, 0, 0);
    }
    explicit Impl(const std::filesystem::path &path,
                  std::optional<std::uint32_t> seed,
                  const std::filesystem::path &save_directory,
                  const GamepadState &initial_gamepad = {})
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
          assets(load_stage("graphics metadata",
                            [&] {
                                if (gmrt_project)
                                    return GameAssets(package, *gmrt_project);
                                return GameAssets(package);
                            })),
          audio(load_stage("audio metadata", [&] { return GameAudio(package, path.parent_path()); })),
          game_dir(path.parent_path()), package_path(path), fixed_seed(seed) {
        gamepad = initial_gamepad;
        if (gmrt_project) {
            const auto wad = load_stage("GMRT modules", [&] { return GmrtWad::load(path); });
            gmrt_modules.reserve(wad.modules().size());
            for (std::size_t i = 0; i < wad.modules().size(); ++i)
                gmrt_modules.push_back(
                    load_stage("GMIR module", [&] { return GmrtModule::parse(wad.read_module(i)); }));
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
            save_dir = default_save_root() / save_profile(package, path);
        load_stage("world metadata", [&] {
            load_world();
            load_shaders();
            load_animation_curves();
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
#if REGML_PS4
        constexpr std::int32_t os_type = 14;
#else
        constexpr std::int32_t os_type = 0;
#endif
        engine = {{"os_type", os_type},
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
                  {"tile_index_mask", std::int32_t{0x7ffff}},
                  {"tile_mirror", std::int32_t{1U << 28}},
                  {"tile_flip", std::int32_t{1U << 29}},
                  {"tile_rotate", std::int32_t{1U << 30}},
                  {"fps", 30.0},
                  {"fps_real", 30.0},
                  {"async_load", std::int32_t{-1}},
                  {"keyboard_key", std::int32_t{0}},
                  {"keyboard_lastkey", std::int32_t{0}},
                  {"keyboard_string", std::string{}},
                  {"mouse_x", 0.0},
                  {"mouse_y", 0.0},
                  {"mouse_button", std::int32_t{0}}};
#if REGML_PS4
        if (!globals.contains("savedata")) {
            const int savedata = next_ds++;
            maps.try_emplace(savedata);
            globals["savedata"] = std::int32_t{savedata};
        }
#endif
        load_stage("global initialization", [&] {
            for (const int code : global_initializers)
                execute(code, {}, {});
            return true;
        });
        load_stage("initial room", [&] {
            enter(order.at(0));
            return true;
        });
    }
    ~Impl() {
#if REGML_WITH_SDL2 && !REGML_WEB
        for (auto &[channel, recorder] : audio_recorders) {
            (void)channel;
            if (recorder.device) {
                SDL_PauseAudioDevice(recorder.device, 1);
                SDL_CloseAudioDevice(recorder.device);
            }
        }
#endif
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
    void load_animation_curves() {
        if (gmrt_project)
            return;
        const auto *chunk = package.find_chunk("ACRV");
        if (!chunk)
            return;
        if (chunk->size < 8)
            throw Error(ErrorKind::malformed, "ACRV chunk is truncated", chunk->data_offset);
        const auto version = record(chunk->data_offset, *chunk, 4).u32(0);
        if (version != 1)
            throw Error(ErrorKind::unsupported, "unsupported ACRV chunk version", chunk->data_offset);
        const auto entries = list(chunk->data_offset + 4, *chunk);
        animation_curves.reserve(entries.size());
        for (const auto entry : entries) {
            auto curve_record = record(entry, *chunk, chunk->data_offset + chunk->size - entry);
            curve_record.require(0, 12);
            AnimationCurve curve;
            const auto curve_name = curve_record.u32(0);
            if (curve_name)
                curve.name = package.string_pointer(curve_name);
            const auto channel_count = static_cast<std::size_t>(curve_record.u32(8));
            if (channel_count > 4096 || channel_count > (curve_record.size() - 12) / 16)
                throw Error(ErrorKind::malformed, "invalid ACRV channel count", entry + 8);
            curve.channels.reserve(channel_count);
            std::size_t offset = 12;
            for (std::size_t channel_index = 0; channel_index < channel_count; ++channel_index) {
                curve_record.require(offset, 16);
                AnimationCurveChannel channel;
                const auto channel_name = curve_record.u32(offset);
                if (channel_name)
                    channel.name = package.string_pointer(channel_name);
                channel.type = static_cast<int>(curve_record.u32(offset + 4));
                channel.iterations = curve_record.u32(offset + 8);
                const auto point_count = static_cast<std::size_t>(curve_record.u32(offset + 12));
                offset += 16;
                if (channel.type < 0 || channel.type > 2 || channel.iterations > 65536 ||
                    point_count > 65536 || point_count > (curve_record.size() - offset) / 24)
                    throw Error(ErrorKind::malformed, "invalid ACRV channel data", entry + offset - 4);
                channel.points.reserve(point_count);
                for (std::size_t point_index = 0; point_index < point_count; ++point_index) {
                    AnimationCurvePoint point;
                    point.x = regml::bit_cast<float>(curve_record.u32(offset));
                    point.value = regml::bit_cast<float>(curve_record.u32(offset + 4));
                    point.bezier_x0 = regml::bit_cast<float>(curve_record.u32(offset + 8));
                    point.bezier_y0 = regml::bit_cast<float>(curve_record.u32(offset + 12));
                    point.bezier_x1 = regml::bit_cast<float>(curve_record.u32(offset + 16));
                    point.bezier_y1 = regml::bit_cast<float>(curve_record.u32(offset + 20));
                    channel.points.push_back(point);
                    offset += 24;
                }
                curve.channels.push_back(std::move(channel));
            }
            animation_curves.push_back(std::move(curve));
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
    bool surface_exists(int id) const { return id == 0 || surfaces.contains(id); }
    int surface_texture_handle(int surface) {
        if (!surface_exists(surface))
            return -1;
        if (const auto found = surface_texture_ids.find(surface); found != surface_texture_ids.end())
            return found->second;
        const int handle = next_sprite_texture++;
        surface_texture_ids.emplace(surface, handle);
        texture_surfaces.emplace(handle, surface);
        return handle;
    }
    void forget_surface_texture(int surface) {
        const auto found = surface_texture_ids.find(surface);
        if (found == surface_texture_ids.end())
            return;
        texture_surfaces.erase(found->second);
        surface_texture_ids.erase(found);
        std::erase_if(shader_texture_stages,
                      [&](const auto &stage) { return stage.second == found->second; });
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
        const auto next_gradient =
            sample_shader_stage(shoujo_sampler_uniforms[0], gradient_x + 0.5 / 128.0, 0);
        const double mix_amount = fract(frag_x / (width / 128.0));
        std::array<double, 3> color{};
        for (std::size_t channel = 0; channel < color.size(); ++channel) {
            const unsigned shift = static_cast<unsigned>((2 - channel) * 8);
            color[channel] = ((gradient >> shift) & 255) * (1 - mix_amount) +
                             ((next_gradient >> shift) & 255) * mix_amount;
            color[channel] += std::fabs(-1.0 + fract(frag_x / width) * 2.0) * 127.5;
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
            color[channel] +=
                ((bubble >> shift) & 255) + ((bubble2 >> shift) & 255) * 0.15 + ((stars >> shift) & 255);
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
    void configure_blend(Canvas &target) const noexcept {
        target.blend_mode(gpu_blend_factors[0], gpu_blend_factors[1], gpu_blend_factors[2],
                          gpu_blend_factors[3], gpu_blend_equations[0], gpu_blend_equations[1]);
    }
    void configure_blend() noexcept {
        configure_blend(canvas);
        for (auto &[id, surface] : surfaces) {
            (void)id;
            configure_blend(surface);
        }
    }
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
            const int length =
                row_length == (*array)->row_lengths.end()
                    ? 0
                    : std::max<std::int32_t>(row_length->second - static_cast<std::int32_t>(offset), 0);
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
                const auto found =
                    std::find_if(object.events.begin(), object.events.end(),
                                 [key](const Object::EventCode &entry) { return entry.key == key; });
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
        if (const auto *glob = package.find_chunk("GLOB")) {
            if (glob->size < 4)
                throw Error(ErrorKind::malformed, "truncated global initializer table", glob->data_offset);
            const auto count = record(glob->data_offset, *glob, 4).u32(0);
            if (count > 1'000'000 || count > (glob->size - 4) / 4)
                throw Error(ErrorKind::malformed, "invalid global initializer table", glob->data_offset);
            global_initializers.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                const auto entry_offset = glob->data_offset + 4 + static_cast<std::size_t>(i) * 4;
                const auto code = record(entry_offset, *glob, 4).i32(0);
                code_valid(code, entry_offset);
                if (code >= 0)
                    global_initializers.push_back(code);
            }
        }
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
                path.closed = header.u32(8) != 0;
                path.precision = std::clamp<std::uint32_t>(header.u32(12), 1, 8);
                const auto count = header.u32(16);
                if (count > 65536)
                    throw Error(ErrorKind::malformed, "path point limit", p);
                const auto points = record(p + 20, *chunk, static_cast<std::size_t>(count) * 12);
                for (std::size_t j = 0; j < count; ++j) {
                    PathPoint point;
                    point.x = regml::bit_cast<float>(points.u32(j * 12));
                    point.y = regml::bit_cast<float>(points.u32(j * 12 + 4));
                    point.speed = regml::bit_cast<float>(points.u32(j * 12 + 8));
                    path.control_points.push_back(point);
                }
                rebuild_path(path);
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
                throw Error(ErrorKind::unsupported, "physics vertices exceed limit",
                            p + 64 + object_field_shift);
            const auto groups =
                list(p + 80 + object_field_shift + static_cast<std::size_t>(vertices) * 8, *ob);
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
            const bool modern_room = (r.u32(36) & (0x10000U | 0x20000U | 0x40000U)) != 0;
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
            placement.sx = regml::bit_cast<float>(ir.u32(20));
            placement.sy = regml::bit_cast<float>(ir.u32(24));
            if (linked.shared_code) {
                placement.image_speed = regml::bit_cast<float>(ir.u32(28));
                placement.image_index = ir.i32(32);
                placement.color = ir.u32(36);
                placement.rotation = regml::bit_cast<float>(ir.u32(40));
                placement.precreate = ir.i32(44);
            } else
                placement.precreate = ir.i32(36);
            if (placement.object < 0 || static_cast<std::size_t>(placement.object) >= objects.size())
                throw Error(ErrorKind::malformed, "invalid placed object", q + 8);
            code_valid(placement.create, q + 16);
            code_valid(placement.precreate, q + 36);
            room.instances.push_back(placement);
        }
        constexpr std::size_t layer_list_offset = 88;
        if (room.has_layers && r.u32(layer_list_offset)) {
            std::unordered_map<int, int> instance_layers;
            for (const auto layer_offset : list(r.u32(layer_list_offset), *ro)) {
                const auto header = record(layer_offset, *ro, 56);
                RoomLayer layer;
                layer.name = package.string_pointer(header.u32(0));
                layer.id = static_cast<int>(header.u32(4));
                layer.type = static_cast<int>(header.u32(8));
                layer.depth = header.i32(12);
                layer.x = regml::bit_cast<float>(header.u32(16));
                layer.y = regml::bit_cast<float>(header.u32(20));
                layer.origin_x = layer.x;
                layer.origin_y = layer.y;
                layer.hspeed = regml::bit_cast<float>(header.u32(24));
                layer.vspeed = regml::bit_cast<float>(header.u32(28));
                layer.visible = header.u32(32) != 0;
                const auto effect_count = header.u32(44);
                if (effect_count > 4096)
                    throw Error(ErrorKind::malformed, "room layer effect property limit", layer_offset + 44);
                const auto layer_data_offset =
                    layer_offset + 48 + static_cast<std::size_t>(effect_count) * 12;
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
                    const auto legacy_tile_list = data.u32(0);
                    const auto sprite_list = data.u32(4);
                    if (legacy_tile_list) {
                        const auto legacy_tiles = list(legacy_tile_list, *ro);
                        for (const auto tile_offset : legacy_tiles) {
                            const auto tile_record = record(tile_offset, *ro, 48);
                            Tile tile;
                            tile.id = tile_record.i32(8);
                            tile.element_id = next_layer_element_id++;
                            tile.x = tile_record.i32(0);
                            tile.y = tile_record.i32(4);
                            tile.source_x = tile_record.i32(12);
                            tile.source_y = tile_record.i32(16);
                            tile.width = tile_record.i32(20);
                            tile.height = tile_record.i32(24);
                            tile.depth = layer.depth;
                            tile.sx = regml::bit_cast<float>(tile_record.u32(36));
                            tile.sy = regml::bit_cast<float>(tile_record.u32(40));
                            tile.color = tile_record.u32(44);
                            tile.layer_id = layer.id;
                            tile.sprite_tile = true;
                            room.tiles.push_back(std::move(tile));
                        }
                    }
                    if (sprite_list) {
                        const auto sprites = list(sprite_list, *ro);
                        layer.sprites.reserve(sprites.size());
                        for (const auto sprite_offset : sprites) {
                            const auto sprite_record = record(sprite_offset, *ro, 44);
                            LayerSprite sprite;
                            sprite.id = next_layer_element_id++;
                            sprite.name = package.string_pointer(sprite_record.u32(0));
                            sprite.sprite = sprite_record.i32(4);
                            sprite.x = sprite_record.i32(8);
                            sprite.y = sprite_record.i32(12);
                            sprite.sx = regml::bit_cast<float>(sprite_record.u32(16));
                            sprite.sy = regml::bit_cast<float>(sprite_record.u32(20));
                            sprite.color = sprite_record.u32(24);
                            sprite.speed = regml::bit_cast<float>(sprite_record.u32(28));
                            sprite.speed_type = sprite_record.i32(32);
                            sprite.frame = regml::bit_cast<float>(sprite_record.u32(36));
                            sprite.rotation = regml::bit_cast<float>(sprite_record.u32(40));
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
                    background.frame = regml::bit_cast<float>(data.u32(28));
                    background.speed = regml::bit_cast<float>(data.u32(32));
                    background.speed_type = data.i32(36);
                    layer.backgrounds.push_back(background);
                } else if (layer.type == 4) {
                    layer.tile_element_id = next_layer_element_id++;
                    const auto tile_header = record(layer_data_offset, *ro, 12);
                    const int tileset = tile_header.i32(0);
                    const auto columns = tile_header.u32(4), rows = tile_header.u32(8);
                    layer.tile_set = tileset;
                    layer.tilemap_width = static_cast<int>(columns);
                    layer.tilemap_height = static_cast<int>(rows);
                    const auto cell_count = static_cast<std::uint64_t>(columns) * rows;
                    if (cell_count > 1'000'000)
                        throw Error(ErrorKind::malformed, "room tilemap cell limit", layer_data_offset);
                    const auto data =
                        record(layer_data_offset, *ro,
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
                            entry.cell_x = static_cast<int>(x);
                            entry.cell_y = static_cast<int>(y);
                            entry.tile_data = tile;
                            // Tile coordinates are layer-local; draw_tile() applies the layer
                            // position and origin when it renders the tile.
                            entry.x = static_cast<double>(x) * source->width;
                            entry.y = static_cast<double>(y) * source->height;
                            entry.source_x = source->x;
                            entry.source_y = source->y;
                            entry.width = source->width;
                            entry.height = source->height;
                            if (layer.tile_width == 0 || layer.tile_height == 0) {
                                layer.tile_width = source->width;
                                layer.tile_height = source->height;
                            }
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
            tile.sx = regml::bit_cast<float>(t.u32(36));
            tile.sy = regml::bit_cast<float>(t.u32(40));
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
        inst->spatial_order = instances.size() - 1;
        register_instance_store(inst);
        if (!object_instance_index_dirty && object_instance_indices.size() == objects.size()) {
            const auto index = instances.size() - 1;
            instance_id_indices.try_emplace(inst->id, index);
            add_instance_to_object_index(index);
        } else {
            object_instance_index_dirty = true;
        }
        if (spatial_grid_built)
            insert_spatial_instance(*inst);
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
    static std::uint64_t collision_cell_key(std::int32_t x, std::int32_t y) noexcept {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
               static_cast<std::uint32_t>(y);
    }
    static bool collision_cell_coordinate(double coordinate, std::int32_t &cell) noexcept {
        const double value = std::floor(coordinate / 64.0);
        if (!std::isfinite(value) || value < std::numeric_limits<std::int32_t>::min() ||
            value > std::numeric_limits<std::int32_t>::max())
            return false;
        cell = static_cast<std::int32_t>(value);
        return true;
    }
    static bool collision_cell_range(const Bounds &box,
                                     std::int32_t &left,
                                     std::int32_t &right,
                                     std::int32_t &top,
                                     std::int32_t &bottom) noexcept {
        return collision_cell_coordinate(box.left, left) && collision_cell_coordinate(box.right, right) &&
               collision_cell_coordinate(box.top, top) && collision_cell_coordinate(box.bottom, bottom);
    }
    static bool collision_cell_count(std::int32_t left,
                                     std::int32_t right,
                                     std::int32_t top,
                                     std::int32_t bottom,
                                     std::size_t &count) noexcept {
        constexpr std::size_t max_cells = 256;
        const auto columns = static_cast<std::int64_t>(right) - left + 1;
        const auto rows = static_cast<std::int64_t>(bottom) - top + 1;
        if (columns <= 0 || rows <= 0 || columns > static_cast<std::int64_t>(max_cells) ||
            rows > static_cast<std::int64_t>(max_cells) ||
            columns * rows > static_cast<std::int64_t>(max_cells))
            return false;
        count = static_cast<std::size_t>(columns * rows);
        return true;
    }
    void remove_spatial_instance(Instance &instance) {
        for (const auto &membership : instance.spatial_cells) {
            const auto found = spatial_grid.find(membership.cell);
            if (found == spatial_grid.end())
                continue;
            auto &bucket = found->second;
            std::size_t slot = membership.slot;
            if (slot >= bucket.size() || bucket[slot] != &instance) {
                const auto entry = std::find(bucket.begin(), bucket.end(), &instance);
                if (entry == bucket.end())
                    continue;
                slot = static_cast<std::size_t>(entry - bucket.begin());
            }
            const auto last = bucket.size() - 1;
            if (slot != last) {
                auto *moved = bucket[last];
                bucket[slot] = moved;
                for (auto &moved_membership : moved->spatial_cells)
                    if (moved_membership.cell == membership.cell) {
                        moved_membership.slot = slot;
                        break;
                    }
            }
            bucket.pop_back();
            if (bucket.empty())
                spatial_grid.erase(found);
        }
        instance.spatial_cells.clear();
        if (instance.spatial_large) {
            const auto found =
                std::find(spatial_large_instances.begin(), spatial_large_instances.end(), &instance);
            if (found != spatial_large_instances.end())
                spatial_large_instances.erase(found);
            instance.spatial_large = false;
        }
    }
    void insert_spatial_instance(Instance &instance) {
        instance.spatial_cells.clear();
        instance.spatial_large = false;
        if (!instance.alive)
            return;
        const int sprite = collision_sprite(instance);
        if (sprite < 0)
            return;
        if (!assets.sprite_exists(sprite)) {
            instance.spatial_large = true;
            spatial_large_instances.push_back(&instance);
            return;
        }
        const auto box = bounds(instance);
        if (!box.valid)
            return;
        std::int32_t left = 0, right = 0, top = 0, bottom = 0;
        std::size_t cell_count = 0;
        if (!collision_cell_range(box, left, right, top, bottom) ||
            !collision_cell_count(left, right, top, bottom, cell_count)) {
            instance.spatial_large = true;
            spatial_large_instances.push_back(&instance);
            return;
        }
        instance.spatial_cells.reserve(cell_count);
        for (std::int64_t y = top; y <= bottom; ++y)
            for (std::int64_t x = left; x <= right; ++x) {
                const auto cell =
                    collision_cell_key(static_cast<std::int32_t>(x), static_cast<std::int32_t>(y));
                auto &bucket = spatial_grid[cell];
                instance.spatial_cells.push_back({cell, bucket.size()});
                bucket.push_back(&instance);
            }
    }
    void update_spatial_instance(Instance &instance) {
        if (!spatial_grid_built)
            return;
        remove_spatial_instance(instance);
        insert_spatial_instance(instance);
    }
    void rebuild_spatial_grid() {
        spatial_grid_built = false;
        spatial_grid.clear();
        spatial_large_instances.clear();
        spatial_query_candidates.clear();
        for (std::size_t index = 0; index < instances.size(); ++index) {
            auto &instance = *instances[index];
            instance.spatial_cells.clear();
            instance.spatial_large = false;
            instance.spatial_order = index;
            instance.spatial_query_generation = 0;
        }
        for (const auto &instance : instances)
            insert_spatial_instance(*instance);
        spatial_grid_built = true;
    }
    bool collect_spatial_candidates(const Bounds &box, const Bounds *additional_box = nullptr) {
        spatial_query_candidates.clear();
        if (gmrt_project || instances.size() < 48)
            return false;
        if (!spatial_grid_built)
            rebuild_spatial_grid();
        if (++spatial_query_generation == 0) {
            for (const auto &instance : instances)
                instance->spatial_query_generation = 0;
            ++spatial_query_generation;
        }
        const auto add_candidate = [&](Instance *instance) {
            if (instance->spatial_query_generation != spatial_query_generation) {
                instance->spatial_query_generation = spatial_query_generation;
                spatial_query_candidates.push_back(instance);
            }
        };
        const auto append_box = [&](const Bounds &query) {
            if (!query.valid)
                return true;
            std::int32_t left = 0, right = 0, top = 0, bottom = 0;
            std::size_t cell_count = 0;
            if (!collision_cell_range(query, left, right, top, bottom) ||
                !collision_cell_count(left, right, top, bottom, cell_count))
                return false;
            (void)cell_count;
            for (auto *instance : spatial_large_instances)
                add_candidate(instance);
            for (std::int64_t y = top; y <= bottom; ++y)
                for (std::int64_t x = left; x <= right; ++x) {
                    const auto key =
                        collision_cell_key(static_cast<std::int32_t>(x), static_cast<std::int32_t>(y));
                    const auto found = spatial_grid.find(key);
                    if (found != spatial_grid.end())
                        for (auto *instance : found->second)
                            add_candidate(instance);
                }
            return true;
        };
        if (!append_box(box) || (additional_box && !append_box(*additional_box)))
            return false;
        std::sort(spatial_query_candidates.begin(), spatial_query_candidates.end(),
                  [](const Instance *left, const Instance *right) {
                      return left->spatial_order < right->spatial_order;
                  });
        return true;
    }
    static bool spatial_geometry_variable(std::string_view name) noexcept {
        return name == "x" || name == "y" || name == "sprite_index" || name == "mask_index" ||
               name == "image_xscale" || name == "image_yscale" || name == "image_angle";
    }
    void note_instance_geometry_write(const VariableStore *store, std::string_view name) {
        if (spatial_geometry_variable(name)) {
            note_collision_geometry_change();
            if (const auto found = instance_store_owners.find(store); found != instance_store_owners.end())
                update_spatial_instance(*found->second);
        } else if (name == "image_index") {
            note_collision_geometry_change();
        }
    }
    void note_collision_geometry_change() noexcept {
        if (collision_pass_active)
            ++collision_geometry_generation;
    }
    void rebuild_collision_grid() {
        collision_target_instance_count = 0;
        collision_grid_entries.clear();
        collision_grid_large_instances.clear();
        for (std::size_t index = 0; index < collision_instances.size(); ++index) {
            const auto &instance = collision_instances[index];
            if (!instance->alive || instance->object < 0 ||
                static_cast<std::size_t>(instance->object) >= objects.size())
                continue;
            ++collision_target_instance_count;
            const auto box = bounds(*instance);
            if (!box.valid)
                continue;
            std::int32_t left = 0, right = 0, top = 0, bottom = 0;
            std::size_t cell_count = 0;
            if (!collision_cell_range(box, left, right, top, bottom) ||
                !collision_cell_count(left, right, top, bottom, cell_count)) {
                collision_grid_large_instances.push_back(index);
                continue;
            }
            (void)cell_count;
            for (std::int64_t y = top; y <= bottom; ++y)
                for (std::int64_t x = left; x <= right; ++x)
                    collision_grid_entries.push_back(
                        {collision_cell_key(static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)),
                         index});
        }
        std::sort(collision_grid_entries.begin(), collision_grid_entries.end(),
                  [](const CollisionGridEntry &left, const CollisionGridEntry &right) {
                      return left.cell != right.cell ? left.cell < right.cell
                                                     : left.instance_index < right.instance_index;
                  });
        collision_candidate_stamps.resize(collision_instances.size());
        collision_grid_dirty = false;
    }
    bool collect_collision_candidates(const Bounds &box) {
        collision_query_indices.clear();
        if (!box.valid)
            return true;

        std::int32_t left = 0, right = 0, top = 0, bottom = 0;
        std::size_t cell_count = 0;
        if (!collision_cell_range(box, left, right, top, bottom) ||
            !collision_cell_count(left, right, top, bottom, cell_count))
            return false;
        (void)cell_count;
        if (++collision_candidate_generation == 0) {
            std::fill(collision_candidate_stamps.begin(), collision_candidate_stamps.end(), 0);
            ++collision_candidate_generation;
        }
        const auto add_candidate = [&](std::size_t index) {
            if (collision_candidate_stamps[index] != collision_candidate_generation) {
                collision_candidate_stamps[index] = collision_candidate_generation;
                collision_query_indices.push_back(index);
            }
        };
        for (const auto index : collision_grid_large_instances)
            add_candidate(index);
        for (std::int64_t y = top; y <= bottom; ++y)
            for (std::int64_t x = left; x <= right; ++x) {
                const auto key =
                    collision_cell_key(static_cast<std::int32_t>(x), static_cast<std::int32_t>(y));
                const auto first = std::lower_bound(
                    collision_grid_entries.begin(), collision_grid_entries.end(), key,
                    [](const CollisionGridEntry &entry, std::uint64_t cell) { return entry.cell < cell; });
                for (auto entry = first; entry != collision_grid_entries.end() && entry->cell == key; ++entry)
                    add_candidate(entry->instance_index);
            }
        std::sort(collision_query_indices.begin(), collision_query_indices.end());
        return true;
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
    void add_instance_to_object_index(std::size_t instance_index) {
        if (object_ancestry_stamps.size() != objects.size()) {
            object_ancestry_stamps.assign(objects.size(), 0);
            object_ancestry_generation = 0;
        }
        if (++object_ancestry_generation == 0) {
            std::fill(object_ancestry_stamps.begin(), object_ancestry_stamps.end(), 0);
            ++object_ancestry_generation;
        }
        int object = instances[instance_index]->object;
        for (std::size_t guard = 0;
             object >= 0 && static_cast<std::size_t>(object) < objects.size() && guard < objects.size();
             ++guard) {
            const auto index = static_cast<std::size_t>(object);
            if (object_ancestry_stamps[index] == object_ancestry_generation)
                break;
            object_ancestry_stamps[index] = object_ancestry_generation;
            object_instance_indices[index].push_back(instance_index);
            const int parent = objects[index].parent;
            if (parent == object)
                break;
            object = parent;
        }
    }
    void rebuild_object_instance_index() {
        object_instance_indices.resize(objects.size());
        for (auto &indices : object_instance_indices)
            indices.clear();
        instance_id_indices.clear();
        instance_id_indices.reserve(instances.size());
        for (std::size_t index = 0; index < instances.size(); ++index) {
            instance_id_indices.try_emplace(instances[index]->id, index);
            add_instance_to_object_index(index);
        }
        object_instance_index_dirty = false;
    }
    void ensure_object_instance_index() {
        if (object_instance_index_dirty || object_instance_indices.size() != objects.size())
            rebuild_object_instance_index();
    }
    InstancePtr find_alive_instance_by_id(int id) {
        ensure_object_instance_index();
        const auto found = instance_id_indices.find(id);
        if (found != instance_id_indices.end() && found->second < instances.size() &&
            instances[found->second]->id == id && instances[found->second]->alive)
            return instances[found->second];
        // IDs are unique in GameMaker. Keep a fallback for malformed projects that
        // assign the same explicit ID to more than one live placement.
        for (const auto &instance : instances)
            if (instance->alive && instance->id == id)
                return instance;
        return {};
    }
    InstancePtr find_first_alive_exact_object(int object) {
        ensure_object_instance_index();
        if (object < 0 || static_cast<std::size_t>(object) >= object_instance_indices.size())
            return {};
        for (const auto index : object_instance_indices[static_cast<std::size_t>(object)])
            if (instances[index]->alive && instances[index]->object == object)
                return instances[index];
        return {};
    }
    template <typename Visitor> bool visit_target_candidates(int target, Visitor &&visitor) {
        if (target == -3) {
            for (const auto &instance : instances)
                if (visitor(instance))
                    return true;
            return false;
        }
        ensure_object_instance_index();
        const std::vector<std::size_t> *object_instances = nullptr;
        if (target >= 0 && static_cast<std::size_t>(target) < object_instance_indices.size())
            object_instances = &object_instance_indices[static_cast<std::size_t>(target)];
        const auto id = instance_id_indices.find(target);
        bool id_emitted = false;
        const auto emit_id_if_before = [&](std::size_t index) {
            if (id != instance_id_indices.end() && !id_emitted && id->second < index) {
                id_emitted = true;
                return visitor(instances[id->second]);
            }
            return false;
        };
        if (object_instances) {
            for (const auto index : *object_instances) {
                if (emit_id_if_before(index))
                    return true;
                if (id != instance_id_indices.end() && index == id->second)
                    id_emitted = true;
                if (visitor(instances[index]))
                    return true;
            }
        }
        if (id != instance_id_indices.end() && !id_emitted && visitor(instances[id->second]))
            return true;
        return false;
    }
    void register_instance_store(const InstancePtr &instance) {
        if (instance)
            instance_store_owners.insert_or_assign(&instance->vars, instance.get());
    }
    void unregister_instance_store(const InstancePtr &instance) {
        if (instance)
            instance_store_owners.erase(&instance->vars);
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
        visit_target_candidates(target, [&](const InstancePtr &instance) {
            if (instance->alive && !instance->deactivated)
                out.push_back(instance);
            return false;
        });
        return out;
    }
    Instance *first_target(int target, const InstancePtr &self, const InstancePtr &other) {
        if (target == -1)
            return self.get();
        if (target == -2)
            return other && other->alive ? other.get() : nullptr;
        Instance *found = nullptr;
        visit_target_candidates(target, [&](const InstancePtr &instance) {
            if (instance->alive && !instance->deactivated) {
                found = instance.get();
                return true;
            }
            return false;
        });
        return found;
    }
    Instance *first_target_for_variable(int target, const InstancePtr &self, const InstancePtr &other) {
        if (target == -1)
            return self.get();
        if (target == -2)
            return other.get();
        // instance_destroy marks an instance dead immediately, but GameMaker keeps
        // its variables available until the calling event unwinds.  Destroyed
        // instances remain in this vector until the end of the frame, so allow a
        // direct variable read to resolve that pending instance during nested
        // event/script calls.
        Instance *first_alive = nullptr;
        Instance *first_any = nullptr;
        Instance *first_active = nullptr;
        visit_target_candidates(target, [&](const InstancePtr &instance) {
            if (!first_any)
                first_any = instance.get();
            if (!instance->alive)
                return false;
            if (!first_alive)
                first_alive = instance.get();
            if (!instance->deactivated) {
                first_active = instance.get();
                return true;
            }
            return false;
        });
        return first_active ? first_active : (first_alive ? first_alive : first_any);
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
        const auto draw_event_start =
            profile_draw_event ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
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
                    total +=
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                            .count();
                }
            }
        } draw_event_profile_guard{last_draw_event_ms, draw_event_depth, profile_draw_event,
                                   draw_event_start};
        if (!inst->alive || inst->deactivated)
            return;
        // In ordinary event execution, GML's `other` resolves to the current
        // scope. Collision and nested-scope events pass their explicit target.
        if (!other)
            other = inst;
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
#if REGML_WIIU
        for (const auto entry : event_code->code)
            wiiu_load_trace(std::string("event-code object=") + std::string(definition.name) +
                            " owner=" + std::string(objects[static_cast<std::size_t>(owner)].name) +
                            " code=" + std::to_string(entry) +
                            " name=" + std::string(package.code_name(static_cast<std::size_t>(entry))));
#endif
        event_stack.push_back({type, subtype, owner});
        struct Guard {
            std::vector<EventFrame> &frames;
            ~Guard() { frames.pop_back(); }
        } guard{event_stack};
        for (const auto entry : event_code->code)
            execute(entry, inst, {}, other);
    }
    void dispatch_video_async_event(std::string_view event_type) {
        const int map_id = next_ds++;
        auto [async_map, inserted] = maps.try_emplace(map_id);
        (void)inserted;
        async_map->second.set({true, "type"}, Value{std::string(event_type)}, false);

        const Value previous_async_load = engine.at("async_load");
        engine["async_load"] = std::int32_t{map_id};
        const auto event_targets = instances;
        try {
            for (const auto &target : event_targets) {
                if (target->alive &&
                    find_event_owner(objects[static_cast<std::size_t>(target->object)], {7, 70}) >= 0)
                    event(target, 7, 70);
            }
        } catch (...) {
            engine["async_load"] = previous_async_load;
            maps.erase(map_id);
            throw;
        }
        engine["async_load"] = previous_async_load;
        maps.erase(map_id);
    }
    int audio_recorder_count() const {
#if REGML_WITH_SDL2 && !REGML_WEB
        if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
            return 0;
        return std::max(0, SDL_GetNumAudioDevices(SDL_TRUE));
#else
        return 0;
#endif
    }
#if REGML_WITH_SDL2 && !REGML_WEB
    static void recorder_callback(void *context, Uint8 *stream, int length) noexcept {
        auto *recorder = static_cast<AudioRecorderInput *>(context);
        if (!recorder || length <= 0)
            return;
        constexpr std::size_t max_pending = 128 * 1024;
        const auto available = max_pending - std::min(max_pending, recorder->pending.size());
        const auto count = std::min<std::size_t>(available, static_cast<std::size_t>(length));
        const auto frame_bytes = std::max<unsigned>(1, recorder->format.channels) * sizeof(std::int16_t);
        const auto aligned_count = count - count % frame_bytes;
        if (!aligned_count)
            return;
        try {
            recorder->pending.insert(recorder->pending.end(), stream, stream + aligned_count);
        } catch (...) {
        }
    }
#endif
    void service_audio_recorders() {
#if REGML_WITH_SDL2 && !REGML_WEB
        if (audio_recorders.empty())
            return;
        std::vector<int> channels;
        channels.reserve(audio_recorders.size());
        for (const auto &[channel, recorder] : audio_recorders) {
            (void)recorder;
            channels.push_back(channel);
        }
        for (const auto channel : channels) {
            const auto found = audio_recorders.find(channel);
            if (found == audio_recorders.end() || !found->second.device)
                continue;
            auto &recorder = found->second;
            std::vector<Uint8> raw;
            SDL_LockAudioDevice(recorder.device);
            try {
                raw = recorder.pending;
                recorder.pending.clear();
            } catch (...) {
                SDL_UnlockAudioDevice(recorder.device);
                throw;
            }
            SDL_UnlockAudioDevice(recorder.device);
            if (raw.empty() || recorder.format.freq <= 0 || recorder.format.channels == 0)
                continue;
            const auto frame_bytes =
                static_cast<std::size_t>(recorder.format.channels) * sizeof(std::int16_t);
            const auto input_frames = raw.size() / frame_bytes;
            const auto *input = reinterpret_cast<const std::int16_t *>(raw.data());
            std::vector<std::int16_t> mono;
            mono.reserve(input_frames * 16000 / static_cast<std::size_t>(recorder.format.freq) + 2);
            for (std::size_t frame = 0; frame < input_frames; ++frame) {
                std::int32_t sum = 0;
                for (unsigned channel_index = 0; channel_index < recorder.format.channels; ++channel_index)
                    sum += input[frame * recorder.format.channels + channel_index];
                const auto sample = static_cast<std::int16_t>(sum / recorder.format.channels);
                recorder.resample_phase += 16000;
                while (recorder.resample_phase >= static_cast<std::uint64_t>(recorder.format.freq)) {
                    recorder.resample_phase -= static_cast<std::uint64_t>(recorder.format.freq);
                    mono.push_back(sample);
                }
            }
            if (mono.empty())
                continue;
            const int buffer_id = next_buffer++;
            GameBuffer audio_buffer;
            audio_buffer.bytes.resize(mono.size() * sizeof(std::int16_t));
            for (std::size_t sample = 0; sample < mono.size(); ++sample) {
                const auto encoded = static_cast<std::uint16_t>(mono[sample]);
                audio_buffer.bytes[sample * 2] = static_cast<std::uint8_t>(encoded);
                audio_buffer.bytes[sample * 2 + 1] = static_cast<std::uint8_t>(encoded >> 8);
            }
            buffers.emplace(buffer_id, std::move(audio_buffer));
            const int map_id = next_ds++;
            auto [async_map, inserted] = maps.try_emplace(map_id);
            (void)inserted;
            async_map->second.set({true, "buffer_id"}, Value{std::int32_t{buffer_id}}, false);
            async_map->second.set({true, "channel_index"}, Value{std::int32_t{channel}}, false);
            async_map->second.set({true, "data_len"}, Value{static_cast<double>(mono.size() * 2)}, false);
            const auto previous_async_load = engine.at("async_load");
            engine["async_load"] = std::int32_t{map_id};
            const auto event_targets = instances;
            try {
                for (const auto &target : event_targets)
                    if (target->alive &&
                        find_event_owner(objects[static_cast<std::size_t>(target->object)], {7, 73}) >= 0)
                        event(target, 7, 73);
            } catch (...) {
                engine["async_load"] = previous_async_load;
                maps.erase(map_id);
                buffers.erase(buffer_id);
                throw;
            }
            engine["async_load"] = previous_async_load;
            maps.erase(map_id);
            buffers.erase(buffer_id);
        }
#endif
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
                const PrefetchKey key{sprite, regml::bit_cast<std::uint64_t>(value)};
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
#if REGML_WIIU
        wiiu_load_trace(std::string("enter.begin room=") + std::to_string(id));
#endif
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
        object_instance_index_dirty = true;
        spatial_grid_built = false;
        std::erase_if(instances, [&](const auto &i) {
            const bool remove = !i->alive || number(i->vars.at("persistent")) == 0;
            if (remove)
                unregister_instance_store(i);
            return remove;
        });
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
        wiiu_load_trace("room-payload.begin");
        load_room_payload(rooms[static_cast<std::size_t>(id)]);
        wiiu_load_trace("room-payload.end");
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
            for (const auto &instance : saved->second.instances)
                register_instance_store(instance);
            for (const auto &[key, value] : saved->second.engine)
                engine[key] = value;
            saved_rooms.erase(saved);
        } else {
            wiiu_load_trace("room-instances.begin");
            for (const auto &p : room.instances)
                placed_instances.push_back(make(p));
            wiiu_load_trace("room-events.begin");
            for (std::size_t i = 0; i < placed_instances.size(); ++i) {
                const auto &p = room.instances[i];
                const auto instance_stage =
                    std::string("instance=") + std::to_string(i) + " object=" + std::to_string(p.object);
                wiiu_load_trace(instance_stage + " event14.begin");
                event(placed_instances[i], 14, 0);
                wiiu_load_trace(instance_stage + " event14.end");
                if (p.precreate >= 0) {
                    wiiu_load_trace(instance_stage + " precreate=" + std::to_string(p.precreate));
                    execute(p.precreate, placed_instances[i], {});
                    wiiu_load_trace(instance_stage + " precreate.end");
                }
                wiiu_load_trace(instance_stage + " create-event.begin");
                event(placed_instances[i], 0, 0);
                wiiu_load_trace(instance_stage + " create-event.end");
                if (p.create >= 0) {
                    wiiu_load_trace(instance_stage + " create=" + std::to_string(p.create));
                    execute(p.create, placed_instances[i], {});
                    wiiu_load_trace(instance_stage + " create.end");
                }
            }
            if (room.create >= 0 && !placed_instances.empty())
                execute(room.create, placed_instances[0], {});
            wiiu_load_trace("room-events.end");
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
        wiiu_load_trace("enter.end");
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
#if REGML_WIIU
        static std::size_t wiiu_trace_calls = 0;
        if (depth < 6 && event_budget < 10000 && wiiu_trace_calls < 200) {
            ++wiiu_trace_calls;
            wiiu_load_trace(std::string("execute-entry depth=") + std::to_string(depth) +
                            " code=" + std::string(package.code_name(static_cast<std::size_t>(code))));
        }
#endif
        ++depth;
        const auto program_index = gmrt_project ? std::numeric_limits<std::size_t>::max()
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
                auto handle = make_struct();
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
                gmrt_global_value = make_struct();
            GmrtExecutionContext context;
            context.self = self_value;
            context.other = other_value;
            context.scope = self_value;
            context.globals = gmrt_global_value;
            context.arguments.assign(arguments.begin(), arguments.end());
            std::vector<std::string> recent_gmrt_instructions;
            GmrtVmOptions vm_options;
            if (std::getenv("REGML_GMRT_DEBUG"))
                vm_options.trace = [&](const GmrtFunction &function, const GmrtBlock &,
                                       const GmrtInstruction &instruction, std::span<const Value> registers) {
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
                        instruction.arguments[1] < registers.size() &&
                        instruction.arguments[2] < registers.size()) {
                        const auto &left = registers[instruction.arguments[1]];
                        const auto &right = registers[instruction.arguments[2]];
                        text +=
                            " values=" + std::to_string(left.index()) + "," + std::to_string(right.index());
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
                if (name == "infinity")
                    return std::numeric_limits<double>::infinity();
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
                        properties = make_struct();
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
                        if (name == "depth")
                            found->second->manual_depth = true;
                        else if (name == "layer")
                            found->second->manual_depth = false;
                        return;
                    }
                    (*structure)->variables[name] = value;
                }
            };
            context.call = [&](std::string_view name, std::uint16_t, const Value &call_self,
                               const Value &call_other, const Value &scope,
                               std::span<const Value> call_arguments) -> Value {
                try {
                    return call(name, call_arguments, instance_for_value(call_self),
                                instance_for_value(call_other), struct_for_value(scope));
                } catch (const Error &error) {
                    std::string message =
                        "GMIR builtin " + std::string(name) + ": " + error.what() + " (argument types";
                    for (const auto &argument : call_arguments)
                        message += " " + std::to_string(argument.index());
                    message += ")";
                    message += " at";
                    for (const auto &instruction : recent_gmrt_instructions)
                        message += "\n  " + instruction;
                    throw Error(error.kind(), std::move(message), error.offset());
                } catch (const std::exception &error) {
                    throw Error(ErrorKind::vm, "GMIR builtin " + std::string(name) + ": " + error.what());
                }
            };
            GmrtVmResult result;
            try {
                result = execute_gmrt(gmrt_modules, reference.module, reference.function, std::move(context),
                                      std::move(vm_options));
            } catch (const Error &error) {
                std::string message =
                    "GMIR " + gmrt_modules.at(reference.module).functions.at(reference.function).global_name +
                    ": " + error.what() + " at";
                for (const auto &instruction : recent_gmrt_instructions)
                    message += "\n  " + instruction;
                throw Error(error.kind(), std::move(message), error.offset());
            } catch (const std::exception &error) {
                std::string message =
                    "GMIR " + gmrt_modules.at(reference.module).functions.at(reference.function).global_name +
                    ": " + error.what() + " at";
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
                    environment.saved_struct = {};
                    environment.struct_scope = false;
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
        scratch.branch_cache.fill(VmScratch::BranchCacheEntry{});
        const auto instruction_at = [&](std::size_t offset) -> std::optional<std::size_t> {
            if constexpr (build_instruction_index) {
                return program.instruction_at(offset);
            } else {
                if (offset > std::numeric_limits<std::uint32_t>::max())
                    return program.instruction_at(offset);
                const auto cache_offset = static_cast<std::uint32_t>(offset);
                auto &cached = scratch.branch_cache[(offset / 4) % scratch.branch_cache.size()];
                if (cached.offset == cache_offset)
                    return cached.instruction_index;
                const auto found = program.instruction_at(offset);
                if (found)
                    cached = {cache_offset, static_cast<std::uint32_t>(*found)};
                return found;
            }
        };
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
#if REGML_PS2
        const auto update_active_vm = [&](std::size_t active_pc, std::size_t active_instruction) {
            if (active_instruction >= program.instructions.size())
                return;
            const auto &active = program.instructions[active_instruction];
            regml_ps2_active_pc = static_cast<std::uint32_t>(active_pc);
            regml_ps2_active_opcode = static_cast<std::uint32_t>(active.opcode());
            regml_ps2_active_symbol = active.symbol_id;
        };
#endif
        try {
            while (pc < program.byte_length) {
                last_instruction_pc = pc;
                ++event_budget;
                // Check the runaway-event budget in batches to keep the guard
                // cheaper in the opcode loop. Overshoot is bounded to 63 ops.
                if ((event_budget & 63U) == 0 && event_budget > 2'000'000)
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
#if REGML_WIIU
                if (code == 44 && executed_in_code <= 120)
                    wiiu_load_trace(std::string("launch-param-step pc=") + std::to_string(pc) +
                                    " opcode=" + std::string(opcode_name(opcode)));
                if (event_budget % 1000 == 0)
                    wiiu_load_trace(
                        std::string("instruction-progress count=") + std::to_string(event_budget) +
                        " code=" + std::string(package.code_name(static_cast<std::size_t>(code))) +
                        " pc=" + std::to_string(pc) + " opcode=" + std::string(opcode_name(opcode)));
#endif
#if REGML_PS2
                if ((executed_in_code & 63U) == 1U)
                    update_active_vm(pc, instruction_index);
#endif
                current_stack_type =
                    opcode == Opcode::call || opcode == Opcode::call_v || opcode == Opcode::extended
                        ? DataType::variable
                        : ins.type1;
                if (opcode == Opcode::cmp) {
                    current_stack_type = DataType::boolean;
                } else if (opcode == Opcode::neg || opcode == Opcode::bit_not) {
                    current_stack_type = ins.type2();
                } else if (opcode == Opcode::add || opcode == Opcode::sub || opcode == Opcode::mul ||
                           opcode == Opcode::div || opcode == Opcode::rem || opcode == Opcode::mod) {
                    const auto right_type = ins.type1;
                    const auto left_type = ins.type2();
                    if (right_type == DataType::variable || left_type == DataType::variable)
                        current_stack_type = DataType::variable;
                    else if (opcode == Opcode::add && right_type == DataType::string &&
                             left_type == DataType::string)
                        current_stack_type = DataType::string;
                    else if (right_type == DataType::real || left_type == DataType::real)
                        current_stack_type = DataType::real;
                    else if (right_type == DataType::float32 || left_type == DataType::float32)
                        current_stack_type = DataType::float32;
                    else if (right_type == DataType::int64 || left_type == DataType::int64)
                        current_stack_type = DataType::int64;
                    else if (right_type == DataType::boolean && left_type == DataType::boolean)
                        current_stack_type = DataType::boolean;
                    else
                        current_stack_type = DataType::int32;
                } else if (opcode == Opcode::bit_and || opcode == Opcode::bit_or ||
                           opcode == Opcode::bit_xor || opcode == Opcode::shl || opcode == Opcode::shr) {
                    const auto right_type = ins.type1;
                    const auto left_type = ins.type2();
                    if (right_type == DataType::variable || left_type == DataType::variable)
                        current_stack_type = DataType::variable;
                    else if (right_type == DataType::boolean && left_type == DataType::boolean)
                        current_stack_type = DataType::boolean;
                    else
                        current_stack_type = DataType::int32;
                }
                std::size_t next = pc + ins.size;
                auto variable = [&](bool writing, Value value = std::monostate{}) -> Value {
                    const auto id = static_cast<std::size_t>(ins.symbol_id);
                    const auto kind = (ins.reference_value() >> 24) & 0xf8;
                    const int encoded_scope =
                        regml::bit_cast<std::int16_t>(static_cast<std::uint16_t>(ins.word));
                    const bool side_effect_variable =
                        ins.symbol_id != Instruction::no_symbol &&
                        (linked.variables[id].name == "depth" || linked.variables[id].name == "layer" ||
                         (writing &&
                          (linked.variables[id].name == "speed" || linked.variables[id].name == "direction" ||
                           linked.variables[id].name == "hspeed" || linked.variables[id].name == "vspeed")));
                    if (ins.symbol_id != Instruction::no_symbol && !side_effect_variable && kind == 0xa0) {
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
                                        note_instance_geometry_write(&self->vars, linked.variables[id].name);
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
#if REGML_WIIU
                    if (code == 44 && pc == 24)
                        wiiu_load_trace(std::string("local-store symbol=") + std::to_string(id) + " length=" +
                                        std::to_string(name.size()) + " name=" + std::string(name));
#endif
                    int scope = encoded_scope;
                    int array_index = -1;
                    StructPtr explicit_struct;
                    const bool dynamic_local =
                        name == "undefined" || name == "instance_count" || name.starts_with("bbox_") ||
                        name.starts_with("argument[") || name == "sprite_width" || name == "sprite_height" ||
                        name == "sprite_xoffset" || name == "sprite_yoffset" || name == "image_number";
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
                    if (!writing && opcode == Opcode::push_builtin && name == "infinity")
                        return std::numeric_limits<double>::infinity();
                    if (scope == -9) {
                        const auto receiver = pop();
                        if (const auto *object = std::get_if<StructPtr>(&receiver))
                            explicit_struct = *object;
                        else if (const auto *callable = std::get_if<CallablePtr>(&receiver);
                                 callable && *callable) {
                            if (!(*callable)->properties)
                                (*callable)->properties = make_struct();
                            explicit_struct = (*callable)->properties;
                        } else {
                            scope = number(receiver);
                        }
                    }
                    if (!explicit_struct && !struct_self && !writing && name.starts_with("bbox_")) {
                        const auto *found = first_target_for_variable(scope, self, other);
                        if (!found)
                            return std::monostate{};
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
                        const auto *found = first_target_for_variable(scope, self, other);
                        if (!found)
                            return std::monostate{};
                        const int id = number(found->vars.at("sprite_index"));
                        if (!assets.sprite_exists(id))
                            return 0.0;
                        const auto &spr = assets.sprite(id);
                        if (name == "sprite_width")
                            return static_cast<double>(spr.width) *
                                   std::fabs(value_number(found->vars.at("image_xscale")));
                        if (name == "sprite_height")
                            return static_cast<double>(spr.height) *
                                   std::fabs(value_number(found->vars.at("image_yscale")));
                        if (name == "sprite_xoffset")
                            return static_cast<double>(spr.ox) *
                                   value_number(found->vars.at("image_xscale"));
                        if (name == "sprite_yoffset")
                            return static_cast<double>(spr.oy) *
                                   value_number(found->vars.at("image_yscale"));
                        return static_cast<double>(spr.frames.size());
                    }
                    VariableStore *store = nullptr;
                    bool multi_target = false;
                    const bool struct_scope = scope == -1 || scope == -6;
                    const bool struct_store = explicit_struct || (struct_self && struct_scope);
                    const bool static_store = scope == -16;
                    if (explicit_struct)
                        store = &explicit_struct->variables;
                    else if (struct_self && struct_scope)
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
                    // Global-script initializers run without an instance. Their implicit
                    // self-scope writes still initialize globals (including script
                    // function values such as `console`).
                    else if (scope == -1 && !self)
                        store = &globals;
                    else if ((scope == -1 || scope == -6) && self)
                        store = &self->vars;
                    else if (scope == -2 && other)
                        store = &other->vars;
                    else {
                        multi_target = writing;
                        if (!writing) {
                            auto *found = first_target_for_variable(scope, self, other);
                            if (!found)
                                return std::monostate{};
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
                            name.starts_with("argument") && name.size() > 8 &&
                            std::all_of(name.begin() + 8, name.end(),
                                        [](unsigned char ch) { return std::isdigit(ch) != 0; }))
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
                                    length =
                                        std::max<std::int32_t>(length, static_cast<std::int32_t>(col + 1));
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
                                length = std::max<std::int32_t>(length, static_cast<std::int32_t>(col + 1));
                                return value;
                            }
                            const auto length = (*array)->row_lengths.find(row);
                            if (length == (*array)->row_lengths.end() || col >= length->second)
                                return std::monostate{};
                            const auto *element = (*array)->elements.find(array_index);
                            return element ? *element : Value{std::int32_t{0}};
                        }
                        const bool cacheable =
                            !multi_target && !explicit_struct && name != "depth" && name != "layer" &&
                            (store != &engine || (name != "event_type" && name != "event_number"));
                        auto &cached = scratch.variable_cache[id % scratch.variable_cache.size()];
                        const bool cache_hit = cacheable && cached.generation == variable_cache_generation &&
                                               cached.symbol == id && cached.store == store && cached.value;
                        if (writing) {
                            Value *destination_pointer = nullptr;
                            if (cache_hit) {
                                destination_pointer = cached.value;
                            } else if (store == &locals && indexed_name.empty()) {
#if REGML_WIIU
                                if (code == 44 && pc == 24)
                                    wiiu_load_trace("local-store.insert.begin");
#endif
                                destination_pointer = &store->try_emplace(name).first->second;
#if REGML_WIIU
                                if (code == 44 && pc == 24)
                                    wiiu_load_trace("local-store.insert.end");
#endif
                            } else {
                                destination_pointer = &(*store)[name];
                            }
                            Value &destination = *destination_pointer;
                            destination = value;
                            if (!struct_store && !static_store && store != &engine && store != &globals &&
                                store != &locals)
                                note_instance_geometry_write(store, name);
                            if (!struct_store && !static_store && store != &engine && store != &globals &&
                                store != &locals && (name == "depth" || name == "layer")) {
                                const bool manual_depth = name == "depth";
                                bool marked = false;
                                auto mark_instance = [&](const InstancePtr &instance) {
                                    if (instance && &instance->vars == store) {
                                        instance->manual_depth = manual_depth;
                                        marked = true;
                                    }
                                };
                                mark_instance(self);
                                mark_instance(other);
                                if (!marked)
                                    if (const auto found = instance_store_owners.find(store);
                                        found != instance_store_owners.end())
                                        found->second->manual_depth = manual_depth;
                            }
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
                        visit_target_candidates(scope, [&](const InstancePtr &target) {
                            if (target->alive && !target->deactivated) {
                                access(&target->vars);
                                found = true;
                            }
                            return false;
                        });
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
                        push(value_truthy(v), ins.type2());
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
                            if (size == 0)
                                throw Error(ErrorKind::vm, "DUP stack size mismatch", program.origin + pc);
                            // The host stack stores one complete Value per slot. Newer
                            // GameMaker bytecode can request a byte span that ends inside
                            // a host slot, so round up to the containing value instead of
                            // aborting the whole event on a size-boundary mismatch.
                            bytes = size >= bytes ? 0 : bytes - size;
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
                        const auto original_size = stack.size();
                        if (duplicate_count > 4096 - original_size)
                            throw Error(ErrorKind::vm, "native VM stack limit", program.origin + pc);
                        const auto first = original_size - duplicate_count;
                        stack.reserve(original_size + duplicate_count);
                        stack_types.reserve(original_size + duplicate_count);
                        for (std::size_t i = first; i < original_size; ++i) {
                            Value duplicate = stack[i];
                            push(std::move(duplicate), stack_types[i]);
                        }
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
                    if (opcode == Opcode::cmp && (std::holds_alternative<std::monostate>(left) ||
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
                    if (opcode == Opcode::cmp && (ins.comparison() == 3 || ins.comparison() == 4)) {
                        const auto is_numeric = [](const Value &value) {
                            return std::holds_alternative<std::int32_t>(value) ||
                                   std::holds_alternative<double>(value) ||
                                   std::holds_alternative<bool>(value);
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
                        push(regml::bit_cast<std::int32_t>(static_cast<std::uint32_t>(number(left))
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
                            throw Error(ErrorKind::vm, "CALLV received an empty callable",
                                        program.origin + pc);
                        const auto caller = instance_for_value(receiver, self, other);
                        result =
                            (*callable)->invoke(args, caller ? std::optional<int>{caller->id} : std::nullopt);
                    } else if (const auto *reference = std::get_if<FunctionReference>(&function)) {
                        if (const auto *object = std::get_if<StructPtr>(&receiver))
                            result = invoke_function(reference->symbol_id, args, {}, other, *object);
                        else
                            result = invoke_function(reference->symbol_id, args,
                                                     instance_for_value(receiver, self, other), other);
                    } else {
                        throw Error(ErrorKind::unsupported, "CALLV requires a function value",
                                    program.origin + pc);
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
                    const bool condition = value_truthy(pop());
                    if (condition == (opcode == Opcode::branch_true))
                        next = ins.branch_target_value();
                    break;
                }
                case Opcode::push_env: {
                    const int scope = number(pop());
                    int target = scope;
                    StructPtr struct_target;
                    bool target_is_struct = false;
                    if (scope == -9) {
                        Value target_value = pop();
                        if (const auto *value = std::get_if<StructPtr>(&target_value)) {
                            struct_target = *value;
                            target_is_struct = true;
                        } else if (std::holds_alternative<std::monostate>(target_value)) {
                            // `with (undefined)` has no instances to visit. Several games use
                            // undefined as the empty value for an optional instance handle.
                            target = -4;
                        } else {
                            // `with` accepts a numeric instance ID, but arbitrary numeric
                            // sentinels are valid expressions too. An ID outside the VM's
                            // 32-bit instance/object range cannot match anything, so treat it
                            // like noone instead of aborting the enclosing event.
                            const double target_number = value_number(target_value);
                            target = std::isfinite(target_number) && target_number >= INT32_MIN &&
                                             target_number <= INT32_MAX
                                         ? static_cast<int>(target_number)
                                         : -4;
                        }
                    }
                    if (environment_count == environments.size())
                        environments.emplace_back();
                    auto &environment = environments[environment_count];
                    environment.saved = self;
                    environment.saved_other = other;
                    environment.saved_struct = struct_self;
                    environment.index = 0;
                    environment.struct_scope = false;
                    environment.choices.clear();
                    if (target_is_struct) {
                        if (struct_target) {
                            if (code == 8376 && std::getenv("REGML_TRACE_PARTICLE_SCOPE")) {
                                static std::size_t particle_scope_traces = 0;
                                if (particle_scope_traces++ < 32) {
                                    std::cerr << "particle struct scope pc=" << std::hex
                                              << (program.origin + pc) << std::dec;
                                    constexpr std::string_view fields[] = {"timer",
                                                                           "x",
                                                                           "y",
                                                                           "hspeed",
                                                                           "vspeed",
                                                                           "gravity",
                                                                           "friction",
                                                                           "image_index",
                                                                           "image_speed",
                                                                           "image_alpha",
                                                                           "fadeout_speed",
                                                                           "sprite_index",
                                                                           "animation_mode",
                                                                           "lifetime",
                                                                           "particle_step_func"};
                                    for (const auto field : fields) {
                                        const auto value = struct_target->variables.find(field);
                                        std::cerr << " " << field << '='
                                                  << (value == struct_target->variables.end()
                                                          ? "<missing>"
                                                          : value_text(value->second));
                                    }
                                    std::cerr << '\n';
                                }
                            }
                            environment.struct_scope = true;
                            ++environment_count;
                            other = self;
                            self.reset();
                            struct_self = std::move(struct_target);
                        } else {
                            environment.saved.reset();
                            environment.saved_other.reset();
                            environment.saved_struct = {};
                            next = ins.branch_target_value();
                            const auto found = instruction_at(next);
                            if (found) {
                                const auto &end = program.instructions.at(*found);
                                if (end.opcode() == Opcode::pop_env)
                                    next += end.size;
                            }
                        }
                        break;
                    }
                    if (target == -1) {
                        if (self)
                            environment.choices.push_back(self);
                    } else if (target == -2) {
                        if (other && other->alive)
                            environment.choices.push_back(other);
                    } else {
                        visit_target_candidates(target, [&](const InstancePtr &instance) {
                            if (instance->alive && !instance->deactivated)
                                environment.choices.push_back(instance);
                            return false;
                        });
                    }
                    if (environment.choices.empty()) {
                        environment.saved.reset();
                        environment.saved_other.reset();
                        environment.saved_struct = {};
                        next = ins.branch_target_value();
                        const auto found = instruction_at(next);
                        if (found) {
                            const auto &end = program.instructions.at(*found);
                            if (end.opcode() == Opcode::pop_env)
                                next += end.size;
                        }
                    } else {
                        ++environment_count;
                        other = self;
                        self = environment.choices[0];
                        struct_self = {};
                    }
                    break;
                }
                case Opcode::pop_env: {
                    if (environment_count == 0)
                        throw Error(ErrorKind::vm, "environment underflow");
                    auto &e = environments[environment_count - 1];
                    if (e.struct_scope) {
                        self = e.saved;
                        other = e.saved_other;
                        struct_self = e.saved_struct;
                        --environment_count;
                        e.saved.reset();
                        e.saved_other.reset();
                        e.saved_struct = {};
                        e.struct_scope = false;
                        break;
                    }
                    if (!ins.environment_exit() && ++e.index < e.choices.size()) {
                        self = e.choices[e.index];
                        struct_self = {};
                        next = ins.branch_target_value();
                    } else {
                        self = e.saved;
                        other = e.saved_other;
                        struct_self = e.saved_struct;
                        --environment_count;
                        e.choices.clear();
                        if (e.choices.capacity() > 64)
                            std::vector<InstancePtr>{}.swap(e.choices);
                        e.saved.reset();
                        e.saved_other.reset();
                        e.saved_struct = {};
                    }
                    break;
                }
                case Opcode::extended: {
                    const auto sub = regml::bit_cast<std::int16_t>(static_cast<std::uint16_t>(ins.word));
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
                                length = std::max<std::int32_t>(length,
                                                                static_cast<std::int32_t>(index % 32000 + 1));
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
                        length = std::max<std::int32_t>(length, static_cast<std::int32_t>(index % 32000 + 1));
                    } else if (sub == -5) {
                        (void)pop();
                    } else if (sub == -6) {
                        push(static_initialized.contains(code));
                    } else if (sub == -7) {
                        static_initialized.insert(code);
                    } else if (sub == -8 || sub == -9) {
                        // The VM owns array values through reference-counted ArrayPtr values, so
                        // savearef/restorearef's temporary GC roots need no extra bookkeeping.
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
                    const auto target = instruction_at(next);
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
#if REGML_PS2
            update_active_vm(last_instruction_pc, instruction_index);
#endif
            if (!e.offset() ||
                (*e.offset() >= program.origin && *e.offset() < program.origin + program.byte_length))
                current_code = code;
            current_vm_address = e.offset().value_or(program.origin + pc);
            std::string error_text = e.what();
            if (std::getenv("REGML_TRACE_VM_INSTRUCTION") &&
                (error_text.find("numeric argument requires a scalar number") != std::string::npos ||
                 error_text.find("integer value out of range") != std::string::npos)) {
                const auto absolute_offset = e.offset().value_or(program.origin + last_instruction_pc);
                auto instruction_pc = last_instruction_pc;
                if (absolute_offset >= program.origin &&
                    absolute_offset < program.origin + program.byte_length)
                    instruction_pc = absolute_offset - program.origin;
                error_text += " [instruction pc=" + std::to_string(instruction_pc);
                if (const auto instruction_index = program.instruction_at(instruction_pc))
                    error_text += " opcode=" +
                                  std::string(opcode_name(program.instructions[*instruction_index].opcode()));
                error_text += "]";
            }
#if REGML_PS2
            ps2_error_trace(package.code_name(static_cast<std::size_t>(code)), pc, e.what());
#endif
            throw Error(e.kind(),
                        std::move(error_text) + " in " +
                            std::string(package.code_name(static_cast<std::size_t>(code))),
                        e.offset().value_or(program.origin + pc));
        } catch (const std::bad_alloc &) {
#if REGML_PS2
            update_active_vm(last_instruction_pc, instruction_index);
#endif
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
#if REGML_PS2
            update_active_vm(last_instruction_pc, instruction_index);
#endif
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
        // A struct can be the receiver of a method call, but it is never an
        // instance id. Callers that need struct context handle it separately.
        if (std::holds_alternative<StructPtr>(value))
            return {};
        const auto id = static_cast<int>(value_number(value));
        if (id == -1)
            return self;
        if (id == -2)
            return other;
        return find_alive_instance_by_id(id);
    }
    Value invoke_function(std::uint32_t symbol_id,
                          std::span<const Value> args,
                          InstancePtr self,
                          InstancePtr other,
                          StructPtr struct_self = {}) {
        if (symbol_id >= linked.functions.size())
            throw Error(ErrorKind::vm, "function reference is out of range");
        const auto &name = linked.functions[symbol_id].name;
#if REGML_WIIU
        wiiu_load_trace(std::string("invoke-function id=") + std::to_string(symbol_id) + " name=" + name);
#endif
        if (const auto found = code_ids.find(name); found != code_ids.end())
            return execute(found->second, self, args, other, std::move(struct_self));
        return call(name, args, self, other, std::move(struct_self));
    }
    void emit_particles(int system_id, int emitter_id, int type_id, int requested) {
        auto &system = particle_systems.at(system_id);
        const auto &emitter = particle_emitters.at(emitter_id);
        const auto &type = particle_types.at(type_id);
        if (emitter.system != system_id)
            throw Error(ErrorKind::vm, "particle emitter belongs to a different system");
        int count = requested;
        if (emitter.relative) {
            const double area = std::fabs((emitter.x_max - emitter.x_min) * (emitter.y_max - emitter.y_min));
            count = static_cast<int>(
                std::clamp(std::ceil(area * std::max(0, requested) / 100.0 / 256.0), 0.0, 10'000.0));
        } else if (count < 0) {
            std::uniform_int_distribution<int> chance(1, std::min(100'000, -count));
            count = chance(random) == 1 ? 1 : 0;
        }
        if (count <= 0)
            return;
        count = std::min(count, 10'000);
        if (system.particles.size() + static_cast<std::size_t>(count) > 50'000)
            throw Error(ErrorKind::vm, "particle system exceeds 50000 active particles");
        std::uniform_real_distribution<double> unit_random(0.0, 1.0);
        auto random_between = [&](double minimum, double maximum) {
            if (minimum > maximum)
                std::swap(minimum, maximum);
            return std::uniform_real_distribution<double>(minimum, maximum)(random);
        };
        auto shaped_unit = [&](int distribution) {
            const double u = unit_random(random);
            if (distribution == 1)
                return (u + unit_random(random)) * 0.5;
            if (distribution == 2)
                return 1.0 - (u + unit_random(random)) * 0.5;
            return u;
        };
        for (int i = 0; i < count; ++i) {
            double x = emitter.x_min + (emitter.x_max - emitter.x_min) * shaped_unit(emitter.distribution);
            double y = emitter.y_min + (emitter.y_max - emitter.y_min) * shaped_unit(emitter.distribution);
            if (emitter.shape == 1 || emitter.shape == 2) {
                const double angle = unit_random(random) * 2 * 3.141592653589793;
                const double u = unit_random(random);
                const double v = unit_random(random);
                double radius = emitter.distribution == 1   ? (u + v) * 0.5
                                : emitter.distribution == 2 ? 1.0 - (u + v) * 0.5
                                                            : std::sqrt(u);
                if (emitter.shape == 2)
                    radius = shaped_unit(emitter.distribution);
                const double rx = (emitter.x_max - emitter.x_min) * 0.5;
                const double ry = (emitter.y_max - emitter.y_min) * 0.5;
                x = (emitter.x_min + emitter.x_max) * 0.5 + rx * radius * std::cos(angle);
                y = (emitter.y_min + emitter.y_max) * 0.5 + ry * radius * std::sin(angle);
            } else if (emitter.shape == 3) {
                const double t = shaped_unit(emitter.distribution);
                x = emitter.x_min + (emitter.x_max - emitter.x_min) * t;
                y = emitter.y_min + (emitter.y_max - emitter.y_min) * t;
            }
            ParticleState particle;
            particle.type = type_id;
            particle.life =
                std::max(1, static_cast<int>(std::round(random_between(type.life_min, type.life_max))));
            particle.x = x;
            particle.y = y;
            particle.direction = random_between(type.direction_min, type.direction_max);
            particle.speed = random_between(type.speed_min, type.speed_max);
            particle.size = random_between(type.size_min, type.size_max);
            if (type.random_start && type.sprite >= 0 && assets.sprite_exists(type.sprite)) {
                const auto frame_count = assets.sprite(type.sprite).frames.size();
                if (frame_count)
                    particle.image_index = random_between(0, static_cast<double>(frame_count));
            }
            system.particles.push_back(particle);
        }
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
                     name.starts_with("draw_surface") || name.starts_with("draw_tile") || name == "draw_self")
                draw_builtin_category = 1;
            else if (name.starts_with("draw_"))
                draw_builtin_category = 2;
            else if (name.starts_with("surface_"))
                draw_builtin_category = 3;
        }
        const bool profile_draw_builtin = draw_builtin_category >= 0;
        const auto draw_builtin_start =
            profile_draw_builtin ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const std::string_view draw_object_name =
            self && self->object >= 0 && static_cast<std::size_t>(self->object) < objects.size()
                ? std::string_view(objects[static_cast<std::size_t>(self->object)].name)
                : std::string_view{};
        struct DrawBuiltinProfileGuard {
            double &total;
            std::array<double, 4> &categories;
            double &maximum;
            std::string_view &slowest_name;
            std::string &slowest_details;
            int category;
            bool enabled;
            std::chrono::steady_clock::time_point start;
            std::string_view name;
            std::span<const Value> arguments;
            const std::unordered_map<int, Canvas> &surfaces;
            const Canvas &canvas;
            const GameAssets &assets;
            int surface_target;
            std::string_view object_name;
            ~DrawBuiltinProfileGuard() {
                if (enabled) {
                    const double elapsed =
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                            .count();
                    total += elapsed;
                    categories[static_cast<std::size_t>(category)] += elapsed;
                    if (elapsed > maximum) {
                        maximum = elapsed;
                        slowest_name = name;
                        slowest_details.clear();
                        if (name == "draw_sprite_ext") {
                            const auto scalar = [&](std::size_t index) {
                                if (index >= arguments.size())
                                    return 0.0;
                                if (const auto *value = std::get_if<std::int32_t>(&arguments[index]))
                                    return static_cast<double>(*value);
                                if (const auto *value = std::get_if<double>(&arguments[index]))
                                    return *value;
                                if (const auto *value = std::get_if<bool>(&arguments[index]))
                                    return *value ? 1.0 : 0.0;
                                return 0.0;
                            };
                            const int sprite_id = static_cast<int>(scalar(0));
                            std::ostringstream details;
                            details << std::fixed << std::setprecision(2) << "object=" << object_name
                                    << ";sprite=" << sprite_id;
                            if (assets.sprite_exists(sprite_id)) {
                                const auto &sprite = assets.sprite(sprite_id);
                                details << ':' << sprite.name << ';' << sprite.width << 'x' << sprite.height;
                            }
                            details << ";x=" << scalar(2) << ";y=" << scalar(3) << ";sx=" << scalar(4)
                                    << ";sy=" << scalar(5) << ";angle=" << scalar(6) << ";tint=" << scalar(7)
                                    << ";opacity=" << scalar(8);
                            slowest_details = details.str();
                        } else if (name == "draw_surface_ext") {
                            const auto scalar = [&](std::size_t index) {
                                if (index >= arguments.size())
                                    return 0.0;
                                if (const auto *value = std::get_if<std::int32_t>(&arguments[index]))
                                    return static_cast<double>(*value);
                                if (const auto *value = std::get_if<double>(&arguments[index]))
                                    return *value;
                                if (const auto *value = std::get_if<bool>(&arguments[index]))
                                    return *value ? 1.0 : 0.0;
                                return 0.0;
                            };
                            const int surface_id = static_cast<int>(scalar(0));
                            const Canvas *source = surface_id == 0 ? &canvas : nullptr;
                            if (!source) {
                                if (const auto found = surfaces.find(surface_id); found != surfaces.end())
                                    source = &found->second;
                            }
                            const Canvas *target = &canvas;
                            if (surface_target != 0) {
                                if (const auto found = surfaces.find(surface_target); found != surfaces.end())
                                    target = &found->second;
                            }
                            std::ostringstream details;
                            details << std::fixed << std::setprecision(2) << "surface_id=" << surface_id
                                    << ";x=" << scalar(1) << ";y=" << scalar(2) << ";sx=" << scalar(3)
                                    << ";sy=" << scalar(4) << ";angle=" << scalar(5) << ";tint=" << scalar(6)
                                    << ";opacity=" << scalar(7);
                            if (source)
                                details << ";source=" << source->width() << 'x' << source->height();
                            details << ";target=" << target->width() << 'x' << target->height()
                                    << ";render_scale=" << target->render_scale()
                                    << ";pixel_shader=" << target->pixel_shader_active();
                            slowest_details = details.str();
                        } else if (name == "draw_text_ext_transformed") {
                            const auto scalar = [&](std::size_t index) {
                                if (index >= arguments.size())
                                    return 0.0;
                                if (const auto *value = std::get_if<std::int32_t>(&arguments[index]))
                                    return static_cast<double>(*value);
                                if (const auto *value = std::get_if<double>(&arguments[index]))
                                    return *value;
                                if (const auto *value = std::get_if<bool>(&arguments[index]))
                                    return *value ? 1.0 : 0.0;
                                return 0.0;
                            };
                            std::size_t text_bytes = 0;
                            if (arguments.size() > 2) {
                                if (const auto *value = std::get_if<std::string>(&arguments[2]))
                                    text_bytes = value->size();
                            }
                            std::ostringstream details;
                            details << std::fixed << std::setprecision(2) << "text_bytes=" << text_bytes
                                    << ";sx=" << scalar(5) << ";sy=" << scalar(6) << ";angle=" << scalar(7)
                                    << ";line_height=" << scalar(3) << ";max_width=" << scalar(4);
                            slowest_details = details.str();
                        }
                    }
                }
            }
        } draw_builtin_profile_guard{last_draw_builtin_ms,
                                     last_draw_builtin_categories,
                                     max_draw_builtin_call_ms,
                                     slowest_draw_builtin,
                                     slowest_draw_builtin_details,
                                     draw_builtin_category,
                                     profile_draw_builtin,
                                     draw_builtin_start,
                                     name,
                                     args,
                                     surfaces,
                                     canvas,
                                     assets,
                                     surface_target,
                                     draw_object_name};
        auto n = [&](std::size_t i) {
            if (i >= args.size())
                throw Error(ErrorKind::vm, "missing built-in argument");
            const auto &argument = args[i];
            if (std::holds_alternative<std::int32_t>(argument) || std::holds_alternative<double>(argument) ||
                std::holds_alternative<bool>(argument))
                return value_number(argument);
            if (std::holds_alternative<std::monostate>(argument))
                return 0.0;
            if (value_is_string(argument)) {
                const std::string source(value_string_view(argument));
                char *end = nullptr;
                errno = 0;
                const double converted = std::strtod(source.c_str(), &end);
                while (end && *end && std::isspace(static_cast<unsigned char>(*end)))
                    ++end;
                if (end == source.c_str() || !end || *end || errno == ERANGE || !std::isfinite(converted))
                    throw Error(ErrorKind::vm, "cannot convert string argument " + std::to_string(i) +
                                                   " to a number for " + std::string(name));
                return converted;
            }
            if (const auto *structure = std::get_if<StructPtr>(&argument); structure && *structure) {
                if (const auto instance = gmrt_instance_values.find(structure->get());
                    instance != gmrt_instance_values.end() && instance->second && instance->second->alive)
                    return static_cast<double>(instance->second->id);
            }
            throw Error(ErrorKind::vm, "numeric argument " + std::to_string(i) + " to " + std::string(name) +
                                           " cannot convert " + value_text(argument) + " to a number");
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
                while (!path.empty() && (path.back() == '"' || path.back() == '\'' || path.back() == ')' ||
                                         path.back() == ','))
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
                            working_directory = requested_path.has_extension() ? requested_path.parent_path()
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
                const auto package_name =
                    launch_package ? launch_package->filename() : std::filesystem::path("data.win");
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
                    const auto found =
                        std::find_if(current.layers.begin(), current.layers.end(),
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
        auto find_tilemap_layer = [&](int id) -> RoomLayer * {
            if (current_room < 0 || static_cast<std::size_t>(current_room) >= rooms.size())
                return nullptr;
            auto &current = rooms[static_cast<std::size_t>(current_room)];
            const auto found =
                std::find_if(current.layers.begin(), current.layers.end(),
                             [&](const RoomLayer &layer) { return layer.tile_element_id == id; });
            return found == current.layers.end() ? nullptr : &*found;
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
        auto current_canvas = [&]() -> Canvas & {
            while (surface_target != 0) {
                const auto found = surfaces.find(surface_target);
                if (found != surfaces.end())
                    return found->second;
                surface_target = surface_target_stack.empty() ? 0 : surface_target_stack.back();
                if (!surface_target_stack.empty())
                    surface_target_stack.pop_back();
            }
            return this->canvas;
        };
        configure_shader(current_canvas());
        auto update_particles = [&](ParticleSystemState &system) {
            std::erase_if(system.particles, [&](ParticleState &particle) {
                const auto type = particle_types.find(particle.type);
                if (type == particle_types.end() || particle.life <= 0)
                    return true;
                const auto &settings = type->second;
                const double gravity_angle = settings.gravity_direction * 3.141592653589793 / 180.0;
                double vx = particle.speed * std::cos(particle.direction * 3.141592653589793 / 180.0);
                double vy = -particle.speed * std::sin(particle.direction * 3.141592653589793 / 180.0);
                vx += settings.gravity * std::cos(gravity_angle);
                vy -= settings.gravity * std::sin(gravity_angle);
                particle.speed = std::hypot(vx, vy);
                if (particle.speed > 0)
                    particle.direction = std::atan2(-vy, vx) * 180.0 / 3.141592653589793;
                particle.x += vx;
                particle.y += vy;
                ++particle.age;
                if (settings.animated)
                    particle.image_index += 1;
                return particle.age >= particle.life;
            });
        };
        auto draw_particles = [&](ParticleSystemState &system) {
            const auto previous_color = draw_color;
            const double previous_alpha = draw_alpha;
            for (const auto &particle : system.particles) {
                const auto type = particle_types.find(particle.type);
                if (type == particle_types.end())
                    continue;
                const auto &settings = type->second;
                const double t = std::clamp(static_cast<double>(particle.age) /
                                                static_cast<double>(std::max(1, particle.life)),
                                            0.0, 1.0);
                const double alpha =
                    t < 0.5 ? settings.alpha[0] + (settings.alpha[1] - settings.alpha[0]) * (t * 2)
                            : settings.alpha[1] + (settings.alpha[2] - settings.alpha[1]) * ((t - 0.5) * 2);
                const int sprite = settings.sprite;
                if (sprite >= 0 && assets.sprite_exists(sprite)) {
                    const auto count = assets.sprite(sprite).frames.size();
                    const double image =
                        count ? std::fmod(std::max(0.0, particle.image_index), static_cast<double>(count))
                              : 0;
                    const double sx = particle.size * settings.scale_x;
                    const double sy = (settings.stretch ? std::max(0.25, particle.speed) : particle.size) *
                                      settings.scale_y;
                    assets.draw_sprite(current_canvas(), sprite, image, particle.x, particle.y, sx, sy, 0,
                                       0xffffff, alpha);
                } else {
                    current_canvas().color(0xffffff);
                    current_canvas().alpha(alpha);
                    assets.draw_circle(
                        current_canvas(), particle.x, particle.y,
                        std::max(1.0, particle.size * std::max(settings.scale_x, settings.scale_y) * 2),
                        false);
                }
            }
            draw_color = previous_color;
            draw_alpha = previous_alpha;
            current_canvas().color(previous_color);
            current_canvas().alpha(previous_alpha);
        };
        // Tilemap floor drawing calls these lookups repeatedly. Handle them
        // before the grouped builtin dispatcher so they cannot be skipped by
        // a stale cached dispatch group.
        if (name == "tilemap_get_cell_x_at_pixel" || name == "tilemap_get_cell_y_at_pixel") {
            auto *layer = find_tilemap_layer(static_cast<int>(n(0)));
            if (!layer || layer->tile_width <= 0 || layer->tile_height <= 0)
                return std::int32_t{0};
            auto &offset = tile_layer_offsets[layer->id];
            const double map_x = layer->x - layer->origin_x + offset.first;
            const double map_y = layer->y - layer->origin_y + offset.second;
            const bool x_axis = name == "tilemap_get_cell_x_at_pixel";
            const double pixel = n(x_axis ? 1 : 2);
            const double map = x_axis ? map_x : map_y;
            const int tile_size = x_axis ? layer->tile_width : layer->tile_height;
            return static_cast<std::int32_t>(std::floor((pixel - map) / tile_size));
        }
        const auto cached_builtin = builtin_dispatch_groups.find(name);
        std::optional<std::uint8_t> cached_builtin_group;
        if (cached_builtin != builtin_dispatch_groups.end())
            cached_builtin_group = cached_builtin->second;
        const auto dispatch_builtin = [&](std::uint8_t group, auto &&dispatcher) -> std::optional<Value> {
            if (cached_builtin_group && *cached_builtin_group != group)
                return std::nullopt;
            if (auto handled = dispatcher()) {
                if (!cached_builtin_group)
                    builtin_dispatch_groups.try_emplace(name, group);
                return std::move(handled);
            }
            if (cached_builtin_group) {
                builtin_dispatch_groups.erase(name);
                cached_builtin_group.reset();
            }
            return std::nullopt;
        };
        auto dispatch_builtins_0 = [&]() REGML_NOINLINE_LAMBDA -> std::optional<Value> {
            if (name == "@@This@@") {
                if (struct_self)
                    return struct_self;
                return self ? Value{static_cast<std::int32_t>(self->id)} : Value{std::int32_t{-1}};
            }
            if (name == "@@NullObject@@")
                return std::monostate{};
            // Legacy GameMaker bytecode uses these VM internals to bracket
            // try/catch regions. Normal execution only needs their marker calls;
            // exception unwinding into a guest catch block is not modeled yet.
            if (name == "@@try_hook@@" || name == "@@try_unhook@@" || name == "@@finish_catch@@")
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
                    return (*callable)->properties &&
                           (*callable)->properties->variables.contains(property_name);
                if (const auto *array = std::get_if<ArrayPtr>(&args[0]); array && *array) {
                    const auto &elements = (*array)->elements;
                    if (property_name == "length")
                        return true;
                    int index = -1;
                    const auto parsed = std::from_chars(property_name.data(),
                                                        property_name.data() + property_name.size(), index);
                    return parsed.ec == std::errc{} &&
                           parsed.ptr == property_name.data() + property_name.size() && index >= 0 &&
                           elements.contains(index);
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
                else if (const auto *callable = std::get_if<CallablePtr>(&args[1]); callable && *callable) {
                    existing_callable = *callable;
                    symbol_id = existing_callable->symbol_id;
                }
                if (symbol_id == Instruction::no_symbol && (!existing_callable || !existing_callable->invoke))
                    throw Error(ErrorKind::unsupported, "method requires a function or callable value");
                InstancePtr target;
                StructPtr target_struct;
                if (const auto *value = std::get_if<StructPtr>(&args[0])) {
                    target_struct = *value;
                } else if (struct_self && std::holds_alternative<std::int32_t>(args[0]) &&
                           std::get<std::int32_t>(args[0]) == -1)
                    target_struct = struct_self;
                else if (!std::holds_alternative<std::monostate>(args[0]))
                    target = instance_for_value(args[0], self, other);
                auto callable = make_callable();
                callable->symbol_id = symbol_id;
                callable->bound_struct = target_struct;
                if (symbol_id != Instruction::no_symbol)
                    callable->invoke = [this, symbol_id, target, target_struct](
                                           std::span<const Value> call_args, std::optional<int> other_id) {
                        InstancePtr call_other;
                        if (other_id)
                            call_other = find_alive_instance_by_id(*other_id);
                        return invoke_function(symbol_id, call_args, target, call_other, target_struct);
                    };
                else
                    callable->invoke = [existing_callable](std::span<const Value> call_args,
                                                           std::optional<int> other_id) {
                        return existing_callable->invoke(call_args, other_id);
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
                const auto &constructor_name = linked.functions[symbol_id].name;
#if REGML_WIIU
                wiiu_load_trace(std::string("new-object symbol=") + std::to_string(symbol_id) +
                                " name=" + constructor_name);
#endif
                auto object = make_struct();
#if REGML_WIIU
                wiiu_load_trace("new-object.allocated");
#endif
                if (const auto code = code_ids.find(constructor_name); code != code_ids.end()) {
#if REGML_WIIU
                    wiiu_load_trace(std::string("new-object.execute-code=") + std::to_string(code->second));
#endif
                    execute(code->second, {}, args.subspan(1), other, object);
                } else if (const auto script = heterogeneous_find(script_names, constructor_name);
                           script != script_names.end()) {
#if REGML_WIIU
                    wiiu_load_trace(std::string("new-object.execute-script=") +
                                    std::to_string(script->second));
#endif
                    execute(script->second, {}, args.subspan(1), other, object);
                } else {
#if REGML_WIIU
                    wiiu_load_trace("new-object.call-fallback");
#endif
                    call(constructor_name, args.subspan(1), {}, other, object);
                }
#if REGML_WIIU
                wiiu_load_trace("new-object.complete");
#endif
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
            if (const auto it = heterogeneous_find(script_names, name); it != script_names.end())
                return execute(it->second, self, args, other, struct_self);
            if (name == "script_execute") {
                if (args.empty())
                    throw Error(ErrorKind::vm, "script_execute requires a script or function");
                if (const auto *function = std::get_if<FunctionReference>(&args[0]))
                    return invoke_function(function->symbol_id, args.subspan(1), self, other, struct_self);
                if (const auto *callable = std::get_if<CallablePtr>(&args[0])) {
                    if (!*callable || !(*callable)->invoke)
                        throw Error(ErrorKind::vm, "script_execute received an empty callable");
                    return (*callable)->invoke(args.subspan(1),
                                               self ? std::optional<int>{self->id} : std::nullopt);
                }
                const auto id = static_cast<std::size_t>(n(0));
                if (id >= scripts.size())
                    throw Error(ErrorKind::vm, "script ID out of range");
                return execute(scripts[id], self, args.subspan(1), other, struct_self);
            }
            if (name == "call_later") {
                if (args.size() < 3)
                    throw Error(ErrorKind::vm, "call_later requires a period, unit and callback");
                const double period = n(0);
                const int unit = static_cast<int>(n(1));
                if (unit != 0 && unit != 1)
                    throw Error(ErrorKind::vm, "call_later received an invalid time unit");
                if (!std::holds_alternative<FunctionReference>(args[2]) &&
                    !std::holds_alternative<CallablePtr>(args[2]))
                    throw Error(ErrorKind::vm, "call_later callback must be a method or script function");
                double room_speed = 30;
                if (const auto speed = engine.find("room_speed"); speed != engine.end())
                    room_speed = value_number(speed->second);
                if (!std::isfinite(room_speed) || room_speed <= 0)
                    room_speed = 30;
                const double requested_frames =
                    unit == 1 ? std::floor(period) : std::ceil(period * room_speed);
                const auto delay = static_cast<std::size_t>(std::clamp(requested_frames, 1.0, 100'000'000.0));
                const int id = next_delayed_call++;
                delayed_calls.push_back({id, frame + delay, delay, args[2], self, other, struct_self,
                                         args.size() > 3 && value_number(args[3]) != 0});
                return std::int32_t{id};
            }
            if (name == "call_cancel") {
                if (args.empty())
                    throw Error(ErrorKind::vm, "call_cancel requires a time-source handle");
                const int id = number(args[0]);
                std::erase_if(delayed_calls, [id](const DelayedCall &timer) { return timer.id == id; });
                return std::monostate{};
            }
            if (name == "part_system_create") {
                const int id = next_particle_system++;
                particle_systems.try_emplace(id);
                return std::int32_t{id};
            }
            if (name == "part_system_exists")
                return particle_systems.contains(number(args[0]));
            if (name == "part_system_destroy_all") {
                particle_systems.clear();
                particle_emitters.clear();
                return std::monostate{};
            }
            if (name == "part_system_destroy") {
                const int id = number(args[0]);
                particle_systems.erase(id);
                std::erase_if(particle_emitters,
                              [id](const auto &entry) { return entry.second.system == id; });
                return std::monostate{};
            }
            if (name == "part_system_update" || name == "part_system_drawit" || name == "part_system_depth" ||
                name == "part_system_automatic_draw" || name == "part_system_automatic_update") {
                const int id = number(args[0]);
                auto &system = particle_systems.at(id);
                if (name == "part_system_update")
                    update_particles(system);
                else if (name == "part_system_drawit")
                    draw_particles(system);
                else if (name == "part_system_depth") {
                    if (args.size() > 1)
                        system.depth = number(args[1]);
                    else
                        return std::int32_t{system.depth};
                } else if (name == "part_system_automatic_draw")
                    system.automatic_draw = value_number(args[1]) != 0;
                else
                    system.automatic_update = value_number(args[1]) != 0;
                return std::monostate{};
            }
            if (name == "part_type_create") {
                const int id = next_particle_type++;
                particle_types.try_emplace(id);
                return std::int32_t{id};
            }
            if (name == "part_type_exists")
                return particle_types.contains(number(args[0]));
            if (name == "part_type_destroy") {
                particle_types.erase(number(args[0]));
                return std::monostate{};
            }
            if (name == "part_type_sprite" || name == "part_type_direction" || name == "part_type_speed" ||
                name == "part_type_life" || name == "part_type_size" || name == "part_type_gravity" ||
                name == "part_type_alpha3" || name == "part_type_blend" || name == "part_type_death" ||
                name == "part_type_scale") {
                auto &type = particle_types.at(number(args[0]));
                if (name == "part_type_sprite") {
                    type.sprite = number(args[1]);
                    type.animated = value_number(args[2]) != 0;
                    type.stretch = value_number(args[3]) != 0;
                    type.random_start = value_number(args[4]) != 0;
                } else if (name == "part_type_direction") {
                    type.direction_min = n(1);
                    type.direction_max = n(2);
                } else if (name == "part_type_speed") {
                    type.speed_min = n(1);
                    type.speed_max = n(2);
                } else if (name == "part_type_life") {
                    type.life_min = std::max(1, number(args[1]));
                    type.life_max = std::max(type.life_min, number(args[2]));
                } else if (name == "part_type_size") {
                    type.size_min = n(1);
                    type.size_max = n(2);
                } else if (name == "part_type_scale") {
                    type.scale_x = n(1);
                    type.scale_y = n(2);
                } else if (name == "part_type_gravity") {
                    type.gravity = n(1);
                    type.gravity_direction = n(2);
                } else if (name == "part_type_alpha3") {
                    type.alpha = {n(1), n(2), n(3)};
                } else if (name == "part_type_blend")
                    type.additive = value_number(args[1]) != 0;
                else {
                    type.death_type = number(args[1]);
                    type.death_count = number(args[2]);
                }
                return std::monostate{};
            }
            if (name == "part_emitter_create") {
                const int system = number(args[0]);
                if (!particle_systems.contains(system))
                    throw Error(ErrorKind::vm, "part_emitter_create received an invalid particle system");
                const int id = next_particle_emitter++;
                particle_emitters.emplace(id, ParticleEmitterState{system});
                return std::int32_t{id};
            }
            if (name == "part_emitter_exists") {
                const int system = number(args[0]), id = number(args[1]);
                const auto emitter = particle_emitters.find(id);
                return emitter != particle_emitters.end() && emitter->second.system == system;
            }
            if (name == "part_emitter_destroy") {
                const int system = number(args[0]), id = number(args[1]);
                const auto emitter = particle_emitters.find(id);
                if (emitter != particle_emitters.end() && emitter->second.system == system)
                    particle_emitters.erase(emitter);
                return std::monostate{};
            }
            if (name == "part_emitter_destroy_all") {
                const int system = number(args[0]);
                std::erase_if(particle_emitters,
                              [system](const auto &entry) { return entry.second.system == system; });
                return std::monostate{};
            }
            if (name == "part_emitter_region") {
                const int system = number(args[0]), id = number(args[1]);
                auto &emitter = particle_emitters.at(id);
                if (emitter.system != system)
                    throw Error(ErrorKind::vm, "particle emitter belongs to a different system");
                emitter.x_min = n(2);
                emitter.x_max = n(3);
                emitter.y_min = n(4);
                emitter.y_max = n(5);
                emitter.shape = number(args[6]);
                emitter.distribution = number(args[7]);
                return std::monostate{};
            }
            if (name == "part_emitter_relative") {
                const int system_id = number(args[0]), emitter_id = number(args[1]);
                auto &emitter = particle_emitters.at(emitter_id);
                if (emitter.system != system_id)
                    throw Error(ErrorKind::vm, "particle emitter belongs to a different system");
                emitter.relative = value_number(args[2]) != 0;
                return std::monostate{};
            }
            if (name == "part_emitter_burst" || name == "part_emitter_stream") {
                const int system_id = number(args[0]), emitter_id = number(args[1]),
                          type_id = number(args[2]);
                const int requested = number(args[3]);
                auto &emitter = particle_emitters.at(emitter_id);
                if (!particle_systems.contains(system_id) || emitter.system != system_id ||
                    !particle_types.contains(type_id))
                    throw Error(ErrorKind::vm, "particle emitter call used an invalid resource");
                if (name == "part_emitter_stream") {
                    emitter.stream_type = requested == 0 ? -1 : type_id;
                    emitter.stream_count = requested;
                } else
                    emit_particles(system_id, emitter_id, type_id, requested);
                return std::monostate{};
            }
            if (name == "os_get_language")
                return std::string("en");
            if (name == "clipboard_set_text") {
                if (args.empty())
                    throw Error(ErrorKind::vm, "clipboard_set_text requires text");
#if REGML_WITH_SDL2 && !REGML_WEB
                const auto clipboard_text = value_text(args[0]);
                SDL_SetClipboardText(clipboard_text.c_str());
#endif
                return std::monostate{};
            }
            if (name == "application_get_position") {
                ArrayPtr position(new ArrayValue);
                position->row_lengths[0] = 4;
                position->elements[0] = 0;
                position->elements[1] = 0;
                position->elements[2] = this->canvas.logical_width();
                position->elements[3] = this->canvas.logical_height();
                return position;
            }
            if (name == "date_current_datetime") {
                const auto now = std::chrono::system_clock::now().time_since_epoch();
                return std::chrono::duration<double>(now).count() / 86400.0 + 25569.0;
            }
            if (name == "date_second_span") {
                const double seconds = std::fabs(n(0) - n(1)) * 86400.0;
                return std::isfinite(seconds) ? std::floor(seconds + 1e-6) : 0.0;
            }
            if (name == "date_get_year" || name == "date_get_month" || name == "date_get_day" ||
                name == "date_get_hour" || name == "date_get_minute" || name == "date_get_second") {
                const double serial = n(0);
                if (!std::isfinite(serial) || std::fabs(serial) > 3'000'000.0)
                    return std::int32_t{0};
                const auto epoch = std::chrono::sys_days{std::chrono::year{1899} / std::chrono::December /
                                                         std::chrono::day{30}};
                const auto timestamp = epoch + std::chrono::duration<double>{serial * 86400.0};
                const auto day = std::chrono::floor<std::chrono::days>(timestamp);
                const std::chrono::year_month_day calendar{day};
                const auto seconds =
                    std::chrono::duration_cast<std::chrono::seconds>(timestamp - day).count();
                if (name == "date_get_year")
                    return static_cast<std::int32_t>(static_cast<int>(calendar.year()));
                if (name == "date_get_month")
                    return static_cast<std::int32_t>(static_cast<unsigned>(calendar.month()));
                if (name == "date_get_day")
                    return static_cast<std::int32_t>(static_cast<unsigned>(calendar.day()));
                if (name == "date_get_hour")
                    return static_cast<std::int32_t>(seconds / 3600);
                if (name == "date_get_minute")
                    return static_cast<std::int32_t>((seconds / 60) % 60);
                return static_cast<std::int32_t>(seconds % 60);
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
                auto info = make_struct();
#if REGML_PS4
                info->variables["platform"] = std::string("PS4");
#elif defined(_WIN32)
                info->variables["platform"] = std::string("Windows");
#elif defined(__APPLE__)
                info->variables["platform"] = std::string("macOS");
#elif defined(__linux__)
                info->variables["platform"] = std::string("Linux");
#else
                info->variables["platform"] = std::string("Unknown");
#endif
#if REGML_PS4
                info->variables["device"] = std::string("PlayStation 4");
#else
                info->variables["device"] = std::string("Desktop");
#endif
                info->variables["version"] = std::string("reGML");
                info->variables["language"] = std::string("en");
                return info;
            }
            if (name == "show_message") {
                std::cerr << text(0) << '\n';
                return std::monostate{};
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
            if (name == "asset_get_tags") {
                if (args.empty())
                    throw Error(ErrorKind::vm, "missing built-in argument");
                std::optional<std::uint32_t> handle;
                const auto make_handle = [](std::uint32_t type, std::size_t index) {
                    return (type << 24) | (static_cast<std::uint32_t>(index) & 0x00ffffffU);
                };
                if (value_is_string(args[0])) {
                    const auto requested = text(0);
                    for (std::size_t i = 0; i < objects.size() && !handle; ++i)
                        if (objects[i].name == requested)
                            handle = make_handle(1, i);
                    for (std::size_t i = 0; i < assets.sprite_count() && !handle; ++i)
                        if (assets.sprite_name(static_cast<int>(i)) == requested)
                            handle = make_handle(2, i);
                    for (std::size_t i = 0; i < rooms.size() && !handle; ++i)
                        if (rooms[i].name == requested)
                            handle = make_handle(3, i);
                    if (const auto sound = audio.find_sound_id(requested); sound && !handle)
                        handle = make_handle(4, static_cast<std::size_t>(*sound));
                    for (const auto &[script_name, script_id] : script_names)
                        if (script_name == requested && !handle) {
                            handle = make_handle(6, static_cast<std::size_t>(script_id));
                            break;
                        }
                } else {
                    const auto raw_index = n(0);
                    if (std::isfinite(raw_index) && raw_index >= 0 && std::trunc(raw_index) == raw_index) {
                        if (args.size() > 1) {
                            const auto raw_type = n(1);
                            if (std::isfinite(raw_type) && raw_type >= 0 && raw_type <= 255 &&
                                std::trunc(raw_type) == raw_type && raw_index <= 0x00ffffff)
                                handle = make_handle(static_cast<std::uint32_t>(raw_type),
                                                     static_cast<std::size_t>(raw_index));
                        } else if (raw_index <= std::numeric_limits<std::uint32_t>::max()) {
                            handle = static_cast<std::uint32_t>(raw_index);
                        }
                    }
                }
                ArrayPtr result(new ArrayValue);
                if (handle) {
                    const auto tags = package.asset_tags(*handle);
                    result->row_lengths[0] = static_cast<std::int32_t>(tags.size());
                    for (std::size_t i = 0; i < tags.size(); ++i)
                        result->elements[static_cast<std::int32_t>(i)] = std::string(tags[i]);
                }
                return result;
            }
            if (name == "script_exists") {
                const int id = static_cast<int>(n(0));
                if (gmrt_project)
                    return gmrt_project->asset_exists(id, 6);
                return id >= 0 && static_cast<std::size_t>(id) < scripts.size();
            }
            return std::nullopt;
        };
        if (auto handled = dispatch_builtin(0, dispatch_builtins_0))
            return std::move(*handled);
        auto dispatch_builtins_1 = [&]() REGML_NOINLINE_LAMBDA -> std::optional<Value> {
            if (name == "room_exists" || name == "object_exists") {
                const auto id = static_cast<int>(n(0));
                if (id < 0)
                    return false;
                if (name == "room_exists")
                    return static_cast<std::size_t>(id) < rooms.size() &&
                           !rooms[static_cast<std::size_t>(id)].name.empty();
                return static_cast<std::size_t>(id) < objects.size() &&
                       !objects[static_cast<std::size_t>(id)].name.empty();
            }
            if (name == "object_get_sprite") {
                const int id = static_cast<int>(n(0));
                return id >= 0 && static_cast<std::size_t>(id) < objects.size()
                           ? Value{std::int32_t{objects[static_cast<std::size_t>(id)].sprite}}
                           : Value{std::int32_t{-1}};
            }
            if (name == "object_get_persistent" || name == "object_set_persistent") {
                const int id = number(args[0]);
                if (id < 0 || static_cast<std::size_t>(id) >= objects.size())
                    return name == "object_get_persistent" ? Value{false} : Value{std::monostate{}};
                auto &object = objects[static_cast<std::size_t>(id)];
                if (name == "object_set_persistent") {
                    if (args.size() < 2)
                        throw Error(ErrorKind::vm, "object_set_persistent requires an object and a value");
                    object.persistent = value_number(args[1]) != 0;
                    return std::monostate{};
                }
                return object.persistent;
            }
            if (name == "object_get_name") {
                const int id = static_cast<int>(n(0));
                return id >= 0 && static_cast<std::size_t>(id) < objects.size()
                           ? Value{std::string(objects[static_cast<std::size_t>(id)].name)}
                           : Value{std::string{}};
            }
            if (name == "object_get_parent") {
                const int id = number(args[0]);
                if (id < 0 || static_cast<std::size_t>(id) >= objects.size())
                    return std::int32_t{-1};
                const int parent = objects[static_cast<std::size_t>(id)].parent;
                return std::int32_t{parent < 0 ? -100 : parent};
            }
            if (name == "object_is_ancestor") {
                const int child = number(args[0]), ancestor = number(args[1]);
                if (child < 0 || ancestor < 0 || static_cast<std::size_t>(child) >= objects.size() ||
                    static_cast<std::size_t>(ancestor) >= objects.size())
                    return false;
                int parent = objects[static_cast<std::size_t>(child)].parent;
                for (std::size_t depth = 0;
                     parent >= 0 && static_cast<std::size_t>(parent) < objects.size() &&
                     depth < objects.size();
                     ++depth) {
                    if (parent == ancestor)
                        return true;
                    parent = objects[static_cast<std::size_t>(parent)].parent;
                }
                return false;
            }
            if (name == "variable_instance_get_names" || name == "variable_struct_get_names") {
                std::vector<std::string> names;
                const auto append_names = [&](const auto &variables) {
                    names.reserve(variables.size());
                    for (const auto &[key, value] : variables) {
                        (void)value;
                        names.emplace_back(key);
                    }
                };
                if (name == "variable_struct_get_names") {
                    if (const auto *structure = std::get_if<StructPtr>(&args[0]); structure && *structure) {
                        if (const auto instance = gmrt_instance_values.find(structure->get());
                            instance != gmrt_instance_values.end())
                            append_names(instance->second->vars);
                        else
                            append_names((*structure)->variables);
                    } else if (const auto *callable = std::get_if<CallablePtr>(&args[0]);
                               callable && *callable && (*callable)->properties)
                        append_names((*callable)->properties->variables);
                } else if (value_is_string(args[0]) && value_text(args[0]) == "global") {
                    append_names(globals);
                } else {
                    const int id = number(args[0]);
                    if (const auto instance = find_alive_instance_by_id(id))
                        append_names(instance->vars);
                }
                std::sort(names.begin(), names.end());
                ArrayPtr result(new ArrayValue);
                result->row_lengths[0] = static_cast<std::int32_t>(names.size());
                result->elements.reserve_dense(names.size());
                for (std::size_t i = 0; i < names.size(); ++i)
                    result->elements[static_cast<std::int32_t>(i)] = std::move(names[i]);
                return result;
            }
            if (name == "tile_get_index" || name == "tile_get_flip" || name == "tile_get_mirror" ||
                name == "tile_get_rotate" || name == "tile_get_empty" || name == "tile_set_index" ||
                name == "tile_set_flip" || name == "tile_set_mirror" || name == "tile_set_rotate" ||
                name == "tile_set_empty") {
                constexpr std::uint32_t index_mask = 0x7ffffU;
                constexpr std::uint32_t mirror_mask = 1U << 28;
                constexpr std::uint32_t flip_mask = 1U << 29;
                constexpr std::uint32_t rotate_mask = 1U << 30;
                auto tile = static_cast<std::uint32_t>(number(args[0]));
                if (name == "tile_get_index")
                    return static_cast<std::int32_t>(tile & index_mask);
                if (name == "tile_get_empty")
                    return (tile & index_mask) == 0;
                if (name == "tile_get_flip")
                    return (tile & flip_mask) != 0;
                if (name == "tile_get_mirror")
                    return (tile & mirror_mask) != 0;
                if (name == "tile_get_rotate")
                    return (tile & rotate_mask) != 0;
                if (name == "tile_set_index")
                    tile = (tile & ~index_mask) | (static_cast<std::uint32_t>(number(args[1])) & index_mask);
                else if (name == "tile_set_empty")
                    tile &= ~index_mask;
                else {
                    const std::uint32_t mask = name == "tile_set_flip"     ? flip_mask
                                               : name == "tile_set_mirror" ? mirror_mask
                                                                           : rotate_mask;
                    if (n(1) != 0)
                        tile |= mask;
                    else
                        tile &= ~mask;
                }
                return static_cast<std::int32_t>(tile);
            }
            if (name == "variable_struct_get" || name == "variable_struct_exists" ||
                (name == "variable_instance_exists" && !args.empty() &&
                 (std::holds_alternative<StructPtr>(args[0]) ||
                  std::holds_alternative<CallablePtr>(args[0])))) {
                if (args.size() < 2 || !value_is_string(args[1]))
                    throw Error(ErrorKind::vm, std::string(name) + " requires a struct and string name");
                const auto key = value_string_view(args[1]);
                const Value *value = nullptr;
                if (const auto *structure = std::get_if<StructPtr>(&args[0]); structure && *structure) {
                    if (const auto instance = gmrt_instance_values.find(structure->get());
                        instance != gmrt_instance_values.end()) {
                        const auto found = instance->second->vars.find(key);
                        if (found != instance->second->vars.end())
                            value = &found->second;
                    } else {
                        const auto found = (*structure)->variables.find(key);
                        if (found != (*structure)->variables.end())
                            value = &found->second;
                    }
                } else if (const auto *callable = std::get_if<CallablePtr>(&args[0]);
                           callable && *callable && (*callable)->properties) {
                    const auto found = (*callable)->properties->variables.find(key);
                    if (found != (*callable)->properties->variables.end())
                        value = &found->second;
                }
                if (name == "variable_struct_exists" || name == "variable_instance_exists")
                    return value != nullptr;
                return value ? *value : Value{std::monostate{}};
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
                        (*callable)->properties = make_struct();
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
            if (name == "array_height_2d") {
                const auto *array = std::get_if<ArrayPtr>(&args[0]);
                if (!array || !*array)
                    return std::int32_t{0};
                std::int32_t height = 0;
                for (const auto &[row, length] : (*array)->row_lengths) {
                    (void)length;
                    if (row >= 0 && row < 32000)
                        height = std::max(height, row + 1);
                }
                return height;
            }
            if (name == "array_length_2d") {
                const auto *array = std::get_if<ArrayPtr>(&args[0]);
                const int row = number(args[1]);
                if (!array || !*array || row < 0 || row >= 32000)
                    return std::int32_t{0};
                if (const auto found = (*array)->row_lengths.find(row); found != (*array)->row_lengths.end())
                    return found->second;
                std::int32_t length = 0;
                (*array)->elements.for_each([&](std::int32_t index, const Value &) {
                    if (index >= row * 32000 && index < (row + 1) * 32000)
                        length = std::max(length, index - row * 32000 + 1);
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
            if (name == "array_pop") {
                if (args.empty())
                    throw Error(ErrorKind::vm, "array_pop requires an array");
                auto *array = std::get_if<ArrayPtr>(&args[0]);
                if (!array || !*array)
                    throw Error(ErrorKind::vm, "array_pop requires an array");
                auto &length = (*array)->row_lengths[0];
                if (length == 0)
                    (*array)->elements.for_each([&](std::int32_t index, const Value &) {
                        if (index >= 0 && index < 32000)
                            length = std::max(length, index + 1);
                    });
                if (length <= 0)
                    return std::monostate{};
                const int last = --length;
                const auto *value = (*array)->elements.find(last);
                const Value result = value ? *value : Value{std::monostate{}};
                (*array)->elements.erase(last);
                return result;
            }
            if (name == "array_resize" || name == "array_insert" || name == "array_sort") {
                if (args.empty())
                    throw Error(ErrorKind::vm, std::string(name) + " requires an array");
                auto *array = std::get_if<ArrayPtr>(&args[0]);
                if (!array || !*array)
                    throw Error(ErrorKind::vm, std::string(name) + " requires an array");
                auto &elements = (*array)->elements;
                auto &length = (*array)->row_lengths[0];
                if (length == 0)
                    elements.for_each([&](std::int32_t index, const Value &) {
                        if (index >= 0 && index < 32000)
                            length = std::max(length, index + 1);
                    });
                if (name == "array_resize") {
                    if (args.size() < 2)
                        throw Error(ErrorKind::vm, "array_resize requires a new length");
                    const int new_length = number(n(1));
                    if (new_length < 0 || new_length > 32000)
                        throw Error(ErrorKind::vm, "array_resize length must be between 0 and 32000");
                    if (new_length < length)
                        for (int i = new_length; i < length; ++i)
                            elements.erase(i);
                    else if (new_length > length) {
                        elements.reserve_dense(static_cast<std::size_t>(new_length));
                        for (int i = length; i < new_length; ++i)
                            elements[i] = std::int32_t{0};
                    }
                    length = new_length;
                    return std::monostate{};
                }
                if (name == "array_insert") {
                    if (args.size() < 3)
                        throw Error(ErrorKind::vm, "array_insert requires at least one value");
                    const std::int32_t raw_index = number(n(1));
                    const std::int32_t index =
                        std::max<std::int32_t>(0, raw_index < 0 ? length + raw_index : raw_index);
                    const auto insertion_count = args.size() - 2;
                    if (insertion_count > 32000 ||
                        static_cast<std::size_t>(std::max<std::int32_t>(length, index)) + insertion_count >
                            32000)
                        throw Error(ErrorKind::vm, "array_insert exceeds the one-dimensional array limit");
                    const std::int32_t old_length = length;
                    const std::int32_t new_length = std::max<std::int32_t>(old_length, index) +
                                                    static_cast<std::int32_t>(insertion_count);
                    std::vector<Value> contents(static_cast<std::size_t>(new_length), Value{std::int32_t{0}});
                    for (int destination = 0; destination < index && destination < old_length; ++destination)
                        if (const auto *value = elements.find(destination))
                            contents[static_cast<std::size_t>(destination)] = *value;
                    for (std::size_t inserted = 0; inserted < insertion_count; ++inserted)
                        contents[static_cast<std::size_t>(index) + inserted] = args[inserted + 2];
                    for (int source = index; source < old_length; ++source)
                        if (const auto *value = elements.find(source))
                            contents[static_cast<std::size_t>(source) + insertion_count] = *value;
                    for (int i = 0; i < std::max(old_length, new_length); ++i)
                        elements.erase(i);
                    elements.reserve_dense(static_cast<std::size_t>(new_length));
                    for (int i = 0; i < new_length; ++i)
                        elements[i] = std::move(contents[static_cast<std::size_t>(i)]);
                    length = new_length;
                    return std::monostate{};
                }
                if (args.size() < 2)
                    throw Error(ErrorKind::vm, "array_sort requires a sort mode or comparator");
                std::vector<Value> contents(static_cast<std::size_t>(length), Value{std::int32_t{0}});
                bool all_strings = length != 0;
                for (int i = 0; i < length; ++i) {
                    if (const auto *value = elements.find(i)) {
                        contents[static_cast<std::size_t>(i)] = *value;
                        all_strings = all_strings && value_is_string(*value);
                    } else
                        all_strings = false;
                }
                const auto *function = std::get_if<FunctionReference>(&args[1]);
                const auto *callable = std::get_if<CallablePtr>(&args[1]);
                if (function || (callable && *callable)) {
                    auto compare = [&](const Value &left, const Value &right) {
                        const std::array<Value, 2> compare_args{left, right};
                        Value result;
                        if (function)
                            result =
                                invoke_function(function->symbol_id, compare_args, self, other, struct_self);
                        else if ((*callable)->invoke)
                            result = (*callable)->invoke(compare_args,
                                                         self ? std::optional<int>{self->id} : std::nullopt);
                        else if ((*callable)->symbol_id != Instruction::no_symbol)
                            result = invoke_function((*callable)->symbol_id, compare_args,
                                                     (*callable)->bound_struct ? InstancePtr{} : self, other,
                                                     (*callable)->bound_struct ? (*callable)->bound_struct
                                                                               : struct_self);
                        return value_number(result) < 0;
                    };
                    std::stable_sort(contents.begin(), contents.end(), compare);
                } else {
                    auto less = [&](const Value &left, const Value &right) {
                        if (all_strings)
                            return value_string_view(left) < value_string_view(right);
                        return value_number(left) < value_number(right);
                    };
                    std::stable_sort(contents.begin(), contents.end(), less);
                    const bool ascending = std::holds_alternative<bool>(args[1]) ? std::get<bool>(args[1])
                                                                                 : value_number(args[1]) != 0;
                    if (!ascending)
                        std::reverse(contents.begin(), contents.end());
                }
                for (int i = 0; i < length; ++i)
                    elements[i] = std::move(contents[static_cast<std::size_t>(i)]);
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
            if (name == "array_copy") {
                if (args.size() < 5)
                    throw Error(ErrorKind::vm, "array_copy requires destination, destination index, source, "
                                               "source index, and length");
                auto *destination = std::get_if<ArrayPtr>(&args[0]);
                const auto *source = std::get_if<ArrayPtr>(&args[2]);
                if (!destination || !*destination || !source || !*source)
                    throw Error(ErrorKind::vm, "array_copy requires valid arrays");
                auto array_length = [](const ArrayPtr &array) {
                    if (const auto found = array->row_lengths.find(0); found != array->row_lengths.end())
                        return found->second;
                    std::int32_t length = 0;
                    array->elements.for_each([&](std::int32_t index, const Value &) {
                        if (index >= 0 && index < 32000)
                            length = std::max(length, index + 1);
                    });
                    return length;
                };
                auto &target = **destination;
                const auto &input = **source;
                const int destination_length = array_length(*destination);
                const int source_length = array_length(*source);
                const int raw_destination_index = number(args[1]);
                const int raw_source_index = number(args[3]);
                const int raw_count = number(args[4]);
                const long long count = raw_count < 0 ? -static_cast<long long>(raw_count) : raw_count;
                if (count == 0)
                    return std::monostate{};
                long long destination_index =
                    raw_destination_index < 0
                        ? static_cast<long long>(destination_length) + raw_destination_index
                        : raw_destination_index;
                destination_index = std::max<long long>(0, destination_index);
                const long long source_index = raw_source_index < 0
                                                   ? static_cast<long long>(source_length) + raw_source_index
                                                   : raw_source_index;
                const long long source_step = raw_count < 0 ? -1 : 1;
                const long long new_length =
                    std::max<long long>(destination_length, destination_index + count);
                if (count > 32000 || new_length > 32000)
                    throw Error(ErrorKind::vm, "array_copy exceeds the one-dimensional array limit");
                const long long last_source = source_index + (count - 1) * source_step;
                if (source_index < 0 || source_index >= source_length || last_source < 0 ||
                    last_source >= source_length)
                    throw Error(ErrorKind::vm, "array_copy source range is out of bounds");
                std::vector<Value> copied;
                copied.reserve(static_cast<std::size_t>(count));
                for (long long index = 0; index < count; ++index) {
                    const auto *value =
                        input.elements.find(static_cast<std::int32_t>(source_index + index * source_step));
                    copied.push_back(value ? *value : Value{std::int32_t{0}});
                }
                for (int index = destination_length; index < destination_index; ++index)
                    target.elements[index] = std::int32_t{0};
                for (long long index = 0; index < count; ++index)
                    target.elements[static_cast<std::int32_t>(destination_index + index)] =
                        std::move(copied[static_cast<std::size_t>(index)]);
                target.row_lengths[0] = static_cast<std::int32_t>(new_length);
                return std::monostate{};
            }
            if (name == "is_real")
                return !value_is_string(args[0]) && !std::holds_alternative<std::monostate>(args[0]);
            if (name == "string_upper" || name == "string_lower") {
                auto str = text(0);
                for (auto &c : str)
                    c = static_cast<char>(name == "string_upper"
                                              ? std::toupper(static_cast<unsigned char>(c))
                                              : std::tolower(static_cast<unsigned char>(c)));
                return str;
            }
            if (name == "string_digits") {
                std::string result;
                const auto source = text(0);
                result.reserve(source.size());
                for (const unsigned char character : source)
                    if (character >= '0' && character <= '9')
                        result.push_back(static_cast<char>(character));
                return result;
            }
            if (name == "string_lettersdigits") {
                std::string result;
                const auto source = text(0);
                result.reserve(source.size());
                for (const unsigned char character : source)
                    if ((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
                        (character >= '0' && character <= '9'))
                        result.push_back(static_cast<char>(character));
                return result;
            }
            if (name == "string_starts_with")
                return text(0).starts_with(text(1));
            if (name == "string_count") {
                const auto needle = text(0), source = text(1);
                if (needle.empty())
                    return 0.0;
                std::size_t count = 0, offset = 0;
                while ((offset = source.find(needle, offset)) != std::string::npos) {
                    ++count;
                    offset += needle.size();
                }
                return static_cast<double>(count);
            }
            if (name == "string_trim_start") {
                std::string result = text(0);
                if (args.size() > 1) {
                    const auto *substrings = std::get_if<ArrayPtr>(&args[1]);
                    if (substrings && *substrings) {
                        std::int32_t length = 0;
                        if (const auto found = (*substrings)->row_lengths.find(0);
                            found != (*substrings)->row_lengths.end())
                            length = found->second;
                        else
                            (*substrings)->elements.for_each([&](std::int32_t index, const Value &) {
                                if (index >= 0 && index < 32000)
                                    length = std::max(length, index + 1);
                            });
                        bool removed = true;
                        while (removed) {
                            removed = false;
                            for (int i = 0; i < length; ++i) {
                                const auto *entry = (*substrings)->elements.find(i);
                                if (!entry || !value_is_string(*entry))
                                    continue;
                                const auto prefix = value_text(*entry);
                                if (!prefix.empty() && result.starts_with(prefix)) {
                                    result.erase(0, prefix.size());
                                    removed = true;
                                    break;
                                }
                            }
                        }
                        return result;
                    }
                }
                constexpr std::string_view whitespace = " \t\r\n\f\v";
                const auto first = result.find_first_not_of(whitespace);
                if (first == std::string::npos)
                    result.clear();
                else if (first != 0)
                    result.erase(0, first);
                return result;
            }
            if (name == "string_format") {
                if (args.size() < 3)
                    throw Error(ErrorKind::vm, "string_format requires a value, width, and decimal count");
                const int width = std::clamp(static_cast<int>(n(1)), 0, 128);
                const int decimals = std::clamp(static_cast<int>(n(2)), 0, 18);
                std::ostringstream output;
                output << std::fixed << std::setprecision(decimals) << std::setw(width) << n(0);
                return output.str();
            }
            if (name == "is_numeric") {
                if (std::holds_alternative<std::int32_t>(args[0]) ||
                    std::holds_alternative<double>(args[0]) || std::holds_alternative<bool>(args[0]))
                    return true;
                const auto value = text(0);
                const auto first = value.find_first_not_of(" \t\r\n");
                if (first == std::string::npos)
                    return false;
                char *end = nullptr;
                std::strtod(value.c_str() + first, &end);
                if (!end || end == value.c_str() + first)
                    return false;
                const auto last = value.find_last_not_of(" \t\r\n");
                return end == value.c_str() + last + 1;
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
                return str.substr(static_cast<std::size_t>(start),
                                  static_cast<std::size_t>(std::max(0.0, n(2))));
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
                if (found) {
                    const auto variable_name = text(1);
                    found->vars[variable_name] = args[2];
                    note_instance_geometry_write(&found->vars, variable_name);
                    if (variable_name == "depth")
                        found->manual_depth = true;
                    else if (variable_name == "layer")
                        found->manual_depth = false;
                }
                return std::monostate{};
            }
            if (name == "variable_global_get")
                return globals.at(text(0));
            if (name == "variable_global_set") {
                globals[text(0)] = args[1];
                return std::monostate{};
            }
            return std::nullopt;
        };
        if (auto handled = dispatch_builtin(1, dispatch_builtins_1))
            return std::move(*handled);
        auto dispatch_builtins_2 = [&]() REGML_NOINLINE_LAMBDA -> std::optional<Value> {
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
                const auto found =
                    name == "ds_priority_delete_min"
                        ? std::min_element(
                              queue.begin(), queue.end(),
                              [](const auto &a, const auto &b) { return a.priority < b.priority; })
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
            if (name == "ds_map_add_list" || name == "ds_map_replace_list") {
                if (args.size() < 3)
                    throw Error(ErrorKind::vm, std::string(name) + " requires a map, key, and list");
                const auto &list = lists.at(number(args[2]));
                ArrayPtr nested(new ArrayValue);
                nested->row_lengths[0] = static_cast<std::int32_t>(list.size());
                for (std::size_t index = 0; index < list.size(); ++index)
                    nested->elements[static_cast<std::int32_t>(index)] = list[index];
                auto &map = maps.at(number(args[0]));
                std::string key_storage;
                const auto key = text_view(1, key_storage);
                map.set({true, key}, nested, name == "ds_map_add_list");
                return std::monostate{};
            }
            if (name == "ds_map_exists" || name == "ds_map_find_value") {
                const auto &map = maps.at(static_cast<int>(n(0)));
                std::string key_storage;
                const DynamicMapKeyView key{value_is_string(args[1]), text_view(1, key_storage)};
                if (name == "ds_map_exists")
                    return heterogeneous_find(map, key) != map.end();
                const auto it = heterogeneous_find(map, key);
                return it == map.end() ? Value{std::monostate{}} : it->second.to_value();
            }
            if (name == "ds_map_destroy") {
                maps.erase(static_cast<int>(n(0)));
                return std::monostate{};
            }
            if (name == "ds_map_clear") {
                maps.at(number(args[0])).clear();
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
                JsonObject object;
                object.reserve(map.size());
                for (const auto &[key, value] : map) {
                    const auto decoded = value.to_value();
                    JsonValue serialized;
                    if (value_is_string(decoded))
                        serialized = std::string(value_string_view(decoded));
                    else if (const auto *boolean = std::get_if<bool>(&decoded))
                        serialized = *boolean;
                    else if (std::holds_alternative<std::monostate>(decoded))
                        serialized = std::monostate{};
                    else if (const auto *array = std::get_if<ArrayPtr>(&decoded); array && *array) {
                        JsonValue::Array values;
                        int length = 0;
                        if (const auto found = (*array)->row_lengths.find(0);
                            found != (*array)->row_lengths.end())
                            length = found->second;
                        values.reserve(static_cast<std::size_t>(std::max(0, length)));
                        for (int index = 0; index < length; ++index) {
                            const auto *element = (*array)->elements.find(index);
                            const Value item = element ? *element : Value{std::int32_t{0}};
                            if (value_is_string(item))
                                values.emplace_back(std::string(value_string_view(item)));
                            else if (const auto *item_boolean = std::get_if<bool>(&item))
                                values.emplace_back(*item_boolean);
                            else if (std::holds_alternative<std::monostate>(item))
                                values.emplace_back(std::monostate{});
                            else
                                values.emplace_back(JsonNumber{value_text(item)});
                        }
                        serialized = std::move(values);
                    } else
                        serialized = JsonNumber{value_text(decoded)};
                    object.emplace_back(key.size() >= 2 ? key.substr(2) : key, std::move(serialized));
                }
                return json_encode(object);
            }
            if (name == "json_decode") {
                const int id = next_ds++;
                auto &map = maps[id];
                for (auto &[key, decoded] : json_decode(text(0))) {
                    Value value = std::monostate{};
                    if (const auto *string = std::get_if<std::string>(&decoded.value))
                        value = *string;
                    else if (const auto *boolean = std::get_if<bool>(&decoded.value))
                        value = *boolean;
                    else if (const auto *number = std::get_if<JsonNumber>(&decoded.value)) {
                        try {
                            value = std::stod(number->text);
                        } catch (...) {
                            value = number->text;
                        }
                    }
                    map.set({true, std::move(key)}, value, false);
                }
                return std::int32_t{id};
            }
            if (name == "ds_list_create") {
                const int id = next_ds++;
                lists[id] = {};
                return std::int32_t{id};
            }
            if (name == "ds_list_copy") {
                const int id = next_ds++;
                lists[id] = lists.at(number(args[0]));
                return std::int32_t{id};
            }
            if (name == "ds_queue_create" || name == "ds_stack_create") {
                const int id = next_ds++;
                (name == "ds_queue_create" ? ds_queues : ds_stacks)[id] = {};
                return std::int32_t{id};
            }
            if (name == "ds_grid_create") {
                const int width = number(args[0]), height = number(args[1]);
                if (width <= 0 || height <= 0 || width > 32000 || height > 32000 ||
                    static_cast<std::uint64_t>(width) * height > 1'000'000)
                    throw Error(ErrorKind::vm, "ds_grid_create dimensions are out of range");
                const int id = next_ds++;
                DataGrid grid;
                grid.width = width;
                grid.height = height;
                grid.cells.assign(static_cast<std::size_t>(width) * height, std::int32_t{0});
                ds_grids.emplace(id, std::move(grid));
                return std::int32_t{id};
            }
            if (name == "ds_list_add") {
                auto &list = lists.at(static_cast<int>(n(0)));
                list.insert(list.end(), args.begin() + 1, args.end());
                return std::monostate{};
            }
            if (name == "ds_list_clear") {
                lists.at(static_cast<int>(n(0))).clear();
                return std::monostate{};
            }
            if (name == "ds_list_delete" || name == "ds_list_insert" || name == "ds_list_replace" ||
                name == "ds_list_set") {
                auto &list = lists.at(static_cast<int>(n(0)));
                const auto position = static_cast<long long>(std::floor(n(1)));
                if (name == "ds_list_delete") {
                    if (position >= 0 && static_cast<std::size_t>(position) < list.size())
                        list.erase(list.begin() + position);
                } else if (name == "ds_list_insert") {
                    if (args.size() < 3)
                        throw Error(ErrorKind::vm, "ds_list_insert requires a value");
                    const auto index =
                        static_cast<std::size_t>(std::clamp<long long>(position, 0, list.size()));
                    list.insert(list.begin() + static_cast<std::ptrdiff_t>(index), args[2]);
                } else if (args.size() >= 3 && position >= 0 &&
                           static_cast<std::size_t>(position) < list.size()) {
                    list[static_cast<std::size_t>(position)] = args[2];
                }
                return std::monostate{};
            }
            if (name == "ds_list_find_index") {
                if (args.size() < 2)
                    throw Error(ErrorKind::vm, "ds_list_find_index requires a value");
                const auto &list = lists.at(static_cast<int>(n(0)));
                const auto &wanted = args[1];
                const auto equal = [&](const Value &value) {
                    if (value_is_string(value) && value_is_string(wanted))
                        return value_string_view(value) == value_string_view(wanted);
                    const bool value_numeric = std::holds_alternative<std::int32_t>(value) ||
                                               std::holds_alternative<double>(value) ||
                                               std::holds_alternative<bool>(value);
                    const bool wanted_numeric = std::holds_alternative<std::int32_t>(wanted) ||
                                                std::holds_alternative<double>(wanted) ||
                                                std::holds_alternative<bool>(wanted);
                    if (value_numeric && wanted_numeric)
                        return value_number(value) == value_number(wanted);
                    return value.index() == wanted.index() && value_text(value) == value_text(wanted);
                };
                const auto found = std::find_if(list.begin(), list.end(), equal);
                return found == list.end() ? Value{std::int32_t{-1}}
                                           : Value{static_cast<std::int32_t>(found - list.begin())};
            }
            if (name == "ds_list_shuffle") {
                auto &list = lists.at(static_cast<int>(n(0)));
                std::shuffle(list.begin(), list.end(), random);
                return std::monostate{};
            }
            if (name == "ds_list_sort") {
                auto &list = lists.at(static_cast<int>(n(0)));
                auto less = [&](const Value &left, const Value &right) {
                    const bool left_numeric = std::holds_alternative<std::int32_t>(left) ||
                                              std::holds_alternative<double>(left) ||
                                              std::holds_alternative<bool>(left);
                    const bool right_numeric = std::holds_alternative<std::int32_t>(right) ||
                                               std::holds_alternative<double>(right) ||
                                               std::holds_alternative<bool>(right);
                    if (left_numeric && right_numeric)
                        return value_number(left) < value_number(right);
                    return value_text(left) < value_text(right);
                };
                std::stable_sort(list.begin(), list.end(), less);
                if (args.size() > 1 && n(1) == 0)
                    std::reverse(list.begin(), list.end());
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
            if (name == "ds_list_empty")
                return lists.at(static_cast<int>(n(0))).empty();
            if (name == "ds_list_size")
                return static_cast<double>(lists.at(static_cast<int>(n(0))).size());
            if (name == "ds_list_find_value") {
                const auto &list = lists.at(number(n(0)));
                const int index = number(n(1));
                if (index < 0 || static_cast<std::size_t>(index) >= list.size())
                    return std::monostate{};
                return list[static_cast<std::size_t>(index)];
            }
            if (name == "ds_list_destroy") {
                lists.erase(static_cast<int>(n(0)));
                return std::monostate{};
            }
            if (name == "ds_stack_push" || name == "ds_queue_enqueue") {
                auto &values = (name == "ds_stack_push" ? ds_stacks : ds_queues).at(number(args[0]));
                values.insert(values.end(), args.begin() + 1, args.end());
                return std::monostate{};
            }
            if (name == "ds_stack_pop" || name == "ds_stack_top" || name == "ds_queue_dequeue" ||
                name == "ds_queue_head" || name == "ds_queue_tail") {
                const bool stack = name.starts_with("ds_stack_");
                auto &values = (stack ? ds_stacks : ds_queues).at(number(args[0]));
                if (values.empty())
                    return std::monostate{};
                const std::size_t index = name == "ds_queue_tail" ? values.size() - 1
                                          : name == "ds_queue_head" || name == "ds_queue_dequeue"
                                              ? 0
                                              : values.size() - 1;
                const Value result = values[index];
                if (name == "ds_stack_pop" || name == "ds_queue_dequeue")
                    values.erase(values.begin() + static_cast<std::ptrdiff_t>(index));
                return result;
            }
            if (name == "ds_stack_size" || name == "ds_queue_size") {
                const auto &values = (name == "ds_stack_size" ? ds_stacks : ds_queues).at(number(args[0]));
                return static_cast<std::int32_t>(values.size());
            }
            if (name == "ds_stack_empty" || name == "ds_queue_empty") {
                const auto &values = (name == "ds_stack_empty" ? ds_stacks : ds_queues).at(number(args[0]));
                return values.empty();
            }
            if (name == "ds_stack_clear" || name == "ds_queue_clear") {
                auto &values = (name == "ds_stack_clear" ? ds_stacks : ds_queues).at(number(args[0]));
                values.clear();
                return std::monostate{};
            }
            if (name == "ds_stack_destroy") {
                ds_stacks.erase(number(args[0]));
                return std::monostate{};
            }
            if (name == "ds_queue_destroy") {
                ds_queues.erase(number(args[0]));
                return std::monostate{};
            }
            if (name == "ds_grid_destroy") {
                ds_grids.erase(number(args[0]));
                return std::monostate{};
            }
            if (name == "ds_grid_clear_all" || name == "ds_grid_clear" || name == "ds_grid_get" ||
                name == "ds_grid_set" || name == "ds_grid_add" || name == "ds_grid_multiply" ||
                name == "ds_grid_resize") {
                auto found = ds_grids.find(number(args[0]));
                if (found == ds_grids.end())
                    return name == "ds_grid_get" ? Value{std::int32_t{0}} : Value{std::monostate{}};
                auto &grid = found->second;
                if (name == "ds_grid_clear_all") {
                    const Value value = args.size() > 1 ? args[1] : Value{std::int32_t{0}};
                    std::fill(grid.cells.begin(), grid.cells.end(), value);
                    return std::monostate{};
                }
                if (name == "ds_grid_resize") {
                    const int width = number(args[1]), height = number(args[2]);
                    if (width <= 0 || height <= 0 || static_cast<std::uint64_t>(width) * height > 1'000'000)
                        throw Error(ErrorKind::vm, "ds_grid_resize dimensions are out of range");
                    std::vector<Value> resized(static_cast<std::size_t>(width) * height, std::int32_t{0});
                    for (int row = 0; row < std::min(height, grid.height); ++row)
                        for (int column = 0; column < std::min(width, grid.width); ++column)
                            resized[static_cast<std::size_t>(row) * width + column] =
                                grid.cells[static_cast<std::size_t>(row) * grid.width + column];
                    grid.width = width;
                    grid.height = height;
                    grid.cells = std::move(resized);
                    return std::monostate{};
                }
                if (name == "ds_grid_clear") {
                    const int x1 = std::max(0, std::min(number(args[1]), number(args[3])));
                    const int y1 = std::max(0, std::min(number(args[2]), number(args[4])));
                    const int x2 = std::min(grid.width - 1, std::max(number(args[1]), number(args[3])));
                    const int y2 = std::min(grid.height - 1, std::max(number(args[2]), number(args[4])));
                    const Value value = args.size() > 5 ? args[5] : Value{std::int32_t{0}};
                    for (int row = y1; row <= y2; ++row)
                        for (int column = x1; column <= x2; ++column)
                            grid.cells[static_cast<std::size_t>(row) * grid.width + column] = value;
                    return std::monostate{};
                }
                const int column = number(args[1]), row = number(args[2]);
                if (column < 0 || row < 0 || column >= grid.width || row >= grid.height)
                    return name == "ds_grid_get" ? Value{std::int32_t{0}} : Value{std::monostate{}};
                auto &cell = grid.cells[static_cast<std::size_t>(row) * grid.width + column];
                if (name == "ds_grid_get")
                    return cell;
                if (name == "ds_grid_set") {
                    cell = args[3];
                    return std::monostate{};
                }
                if (name == "ds_grid_add")
                    cell = value_number(cell) + n(3);
                else if (name == "ds_grid_multiply")
                    cell = value_number(cell) * n(3);
                return std::monostate{};
            }
            if (name == "ds_exists") {
                const int id = static_cast<int>(n(0)), type = static_cast<int>(n(1));
                if (type == 1)
                    return maps.contains(id);
                if (type == 2)
                    return lists.contains(id);
                if (type == 3)
                    return ds_queues.contains(id);
                if (type == 4)
                    return ds_stacks.contains(id);
                if (type == 5)
                    return priorities.contains(id);
                if (type == 6)
                    return ds_grids.contains(id);
                return false;
            }
            return std::nullopt;
        };
        if (auto handled = dispatch_builtin(2, dispatch_builtins_2))
            return std::move(*handled);
        auto dispatch_builtins_3 = [&]() REGML_NOINLINE_LAMBDA -> std::optional<Value> {
            if (name == "buffer_exists")
                return buffers.contains(number(args[0]));
            if (name == "buffer_create") {
                const auto size = static_cast<std::size_t>(std::clamp(n(0), 0.0, 64.0 * 1024 * 1024));
                const int id = next_buffer++;
                const int type = args.size() > 1 ? number(args[1]) : 0;
                buffers.emplace(id, GameBuffer{std::vector<std::uint8_t>(size), 0, type});
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
                if (!bytes.empty() && !input.read(reinterpret_cast<char *>(bytes.data()),
                                                  static_cast<std::streamsize>(bytes.size())))
                    return std::int32_t{-1};
                const int id = next_buffer++;
                buffers.emplace(id, GameBuffer{std::move(bytes), 0, 0});
                return std::int32_t{id};
            }
            if (name == "buffer_get_size")
                return static_cast<double>(buffers.at(static_cast<int>(n(0))).bytes.size());
            if (name == "buffer_seek") {
                auto &buffer = buffers.at(static_cast<int>(n(0)));
                const int base = static_cast<int>(n(1));
                const auto offset = static_cast<std::int64_t>(n(2));
                std::int64_t origin = 0;
                if (base == 1)
                    origin = static_cast<std::int64_t>(buffer.position);
                else if (base == 2)
                    origin = static_cast<std::int64_t>(buffer.bytes.size());
                else if (base != 0)
                    throw Error(ErrorKind::vm, "buffer_seek received an invalid seek base");
                const auto destination = origin + offset;
                if (buffer.type == 2 && !buffer.bytes.empty()) {
                    const auto size = static_cast<std::int64_t>(buffer.bytes.size());
                    buffer.position = static_cast<std::size_t>((destination % size + size) % size);
                } else
                    buffer.position = static_cast<std::size_t>(std::clamp<std::int64_t>(
                        destination, 0, static_cast<std::int64_t>(buffer.bytes.size())));
                return static_cast<double>(buffer.position);
            }
            if (name == "buffer_copy") {
                if (args.size() < 5)
                    throw Error(ErrorKind::vm, "buffer_copy requires source, source offset, size, "
                                               "destination and destination offset");
                const int source_id = number(args[0]), destination_id = number(args[3]);
                if (source_id == destination_id)
                    return std::monostate{};
                const auto source_offset =
                    static_cast<std::size_t>(std::clamp(n(1), 0.0, 64.0 * 1024 * 1024));
                const auto requested_size =
                    static_cast<std::size_t>(std::clamp(n(2), 0.0, 64.0 * 1024 * 1024));
                const auto destination_offset =
                    static_cast<std::size_t>(std::clamp(n(4), 0.0, 64.0 * 1024 * 1024));
                auto &source_buffer = buffers.at(source_id);
                auto &destination_buffer = buffers.at(destination_id);
                const auto &source = source_buffer.bytes;
                if (source_offset >= source.size() || requested_size == 0)
                    return std::monostate{};
                const auto count = std::min(requested_size, source.size() - source_offset);
                auto &destination = destination_buffer.bytes;
                if (destination_offset + count > 64 * 1024 * 1024)
                    throw Error(ErrorKind::vm, "buffer_copy exceeds size limit");
                std::vector<std::uint8_t> copy(source.begin() + static_cast<std::ptrdiff_t>(source_offset),
                                               source.begin() +
                                                   static_cast<std::ptrdiff_t>(source_offset + count));
                if (destination_buffer.type == 1 && destination_offset + count > destination.size())
                    destination.resize(destination_offset + count);
                if (destination_offset < destination.size()) {
                    const auto writable = std::min(copy.size(), destination.size() - destination_offset);
                    std::copy_n(copy.begin(), writable,
                                destination.begin() + static_cast<std::ptrdiff_t>(destination_offset));
                }
                return std::monostate{};
            }
            if (name == "buffer_md5") {
                if (args.size() < 3)
                    throw Error(ErrorKind::vm, "buffer_md5 requires a buffer, offset and size");
                const auto &bytes = buffers.at(number(args[0])).bytes;
                const auto offset =
                    static_cast<std::size_t>(std::clamp(n(1), 0.0, static_cast<double>(bytes.size())));
                const auto size = static_cast<std::size_t>(
                    std::clamp(n(2), 0.0, static_cast<double>(bytes.size() - offset)));
                return md5_hex(std::span<const std::uint8_t>(bytes).subspan(offset, size));
            }
            if (name == "buffer_get_surface" || name == "buffer_set_surface") {
                if (args.size() < 3)
                    throw Error(ErrorKind::vm, std::string(name) + " requires a buffer, surface and offset");
                auto &buffer = buffers.at(number(args[0]));
                const int surface_id = number(args[1]);
                Canvas *surface = surface_id == 0 ? &this->canvas : nullptr;
                if (surface_id != 0) {
                    const auto found = surfaces.find(surface_id);
                    if (found != surfaces.end())
                        surface = &found->second;
                }
                if (!surface)
                    return std::monostate{};
                const auto offset = static_cast<std::size_t>(std::clamp(n(2), 0.0, 64.0 * 1024 * 1024));
                const auto pixels = surface->pixels();
                const auto byte_count = pixels.size() * 4;
                if (offset + byte_count > 64 * 1024 * 1024 ||
                    (name == "buffer_set_surface" && offset + byte_count > buffer.bytes.size()))
                    return std::monostate{};
                if (name == "buffer_get_surface") {
                    if (offset + byte_count > buffer.bytes.size())
                        return std::monostate{};
                    for (std::size_t i = 0; i < pixels.size(); ++i) {
                        const auto pixel = pixels[i];
                        const auto destination = offset + i * 4;
                        buffer.bytes[destination] = static_cast<std::uint8_t>(pixel >> 16);
                        buffer.bytes[destination + 1] = static_cast<std::uint8_t>(pixel >> 8);
                        buffer.bytes[destination + 2] = static_cast<std::uint8_t>(pixel);
                        buffer.bytes[destination + 3] = static_cast<std::uint8_t>(pixel >> 24);
                    }
                } else {
                    auto output = surface->pixels();
                    for (std::size_t i = 0; i < output.size(); ++i) {
                        const auto source = offset + i * 4;
                        output[i] = (static_cast<std::uint32_t>(buffer.bytes[source + 3]) << 24) |
                                    (static_cast<std::uint32_t>(buffer.bytes[source]) << 16) |
                                    (static_cast<std::uint32_t>(buffer.bytes[source + 1]) << 8) |
                                    buffer.bytes[source + 2];
                    }
                }
                return std::monostate{};
            }
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
                    write_integer(regml::bit_cast<std::uint32_t>(static_cast<float>(value_number(value))), 4);
                else if (type == 9)
                    write_integer(regml::bit_cast<std::uint64_t>(value_number(value)), 8);
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
                    return static_cast<double>(
                        regml::bit_cast<float>(static_cast<std::uint32_t>(read_integer(4))));
                if (type == 9)
                    return regml::bit_cast<double>(read_integer(8));
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
            if (name == "gif_open") {
                if (args.size() < 2)
                    throw Error(ErrorKind::vm, "gif_open requires width and height");
                if (!std::isfinite(n(0)) || !std::isfinite(n(1)) || n(0) < 1 || n(1) < 1 || n(0) > 4096 ||
                    n(1) > 4096 || n(0) * n(1) > gif_capture_limit)
                    return std::int32_t{-1};
                GifCapture capture;
                capture.width = static_cast<int>(n(0));
                capture.height = static_cast<int>(n(1));
                if (args.size() > 2) {
                    const auto color = static_cast<unsigned>(number(args[2]));
                    capture.clear_rgb = ((color & 255) << 16) | (color & 0xff00) | ((color >> 16) & 255);
                }
                const int id = next_gif_id++;
                try {
                    gif_captures.emplace(id, std::move(capture));
                } catch (const std::bad_alloc &) {
                    return std::int32_t{-1};
                }
                return std::int32_t{id};
            }
            if (name == "gif_add_surface") {
                if (args.size() < 3)
                    throw Error(ErrorKind::vm, "gif_add_surface requires a GIF, surface, and delay");
                const int gif_id = number(args[0]), surface_id = number(args[1]);
                const auto capture_it = gif_captures.find(gif_id);
                if (capture_it == gif_captures.end())
                    return std::int32_t{-1};
                const Canvas *source = nullptr;
                if (surface_id == 0)
                    source = &this->canvas;
                else if (const auto surface = surfaces.find(surface_id); surface != surfaces.end())
                    source = &surface->second;
                if (!source)
                    return std::int32_t{-1};
                const auto pixel_count = static_cast<std::size_t>(capture_it->second.width) *
                                         static_cast<std::size_t>(capture_it->second.height);
                if (pixel_count > gif_capture_limit - capture_it->second.bytes)
                    return std::int32_t{-1};
                const int source_x = args.size() > 3 ? number(args[3]) : 0;
                const int source_y = args.size() > 4 ? number(args[4]) : 0;
                GifFrame frame;
                try {
                    frame.indices.resize(pixel_count);
                    const auto delay = args.size() > 2 ? n(2) * 100.0 : 6.0;
                    if (!std::isfinite(delay))
                        return std::int32_t{-1};
                    frame.delay = static_cast<std::uint16_t>(std::clamp(std::llround(delay), 0LL, 65535LL));
                    const int width = capture_it->second.width, height = capture_it->second.height;
                    const auto pixels = source->pixels();
                    const auto clear = capture_it->second.clear_rgb;
                    const unsigned clear_r = (clear >> 16) & 255, clear_g = (clear >> 8) & 255,
                                   clear_b = clear & 255;
                    for (int y = 0; y < height; ++y)
                        for (int x = 0; x < width; ++x) {
                            const auto px = static_cast<std::int64_t>(source_x) + x;
                            const auto py = static_cast<std::int64_t>(source_y) + y;
                            std::uint32_t rgb = clear;
                            if (px >= 0 && py >= 0 && px < source->width() && py < source->height()) {
                                const auto rgba =
                                    pixels.empty() ? source->pixel(static_cast<int>(px), static_cast<int>(py))
                                                   : pixels[static_cast<std::size_t>(py) * source->width() +
                                                            static_cast<std::size_t>(px)];
                                const unsigned alpha = rgba >> 24, inverse = 255 - alpha;
                                const unsigned red = ((rgba >> 16) & 255) * alpha + clear_r * inverse;
                                const unsigned green = ((rgba >> 8) & 255) * alpha + clear_g * inverse;
                                const unsigned blue = (rgba & 255) * alpha + clear_b * inverse;
                                rgb = (((red + 127) / 255) << 16) | (((green + 127) / 255) << 8) |
                                      ((blue + 127) / 255);
                            }
                            frame.indices[static_cast<std::size_t>(y) * width + x] = gif_palette_index(rgb);
                        }
                    const auto frame_bytes = frame.indices.size();
                    capture_it->second.frames.push_back(std::move(frame));
                    capture_it->second.bytes += frame_bytes;
                } catch (const std::bad_alloc &) {
                    return std::int32_t{-1};
                }
                return std::int32_t{0};
            }
            if (name == "gif_save") {
                if (args.size() < 2)
                    throw Error(ErrorKind::vm, "gif_save requires a GIF and filename");
                const int gif_id = number(args[0]);
                const auto found = gif_captures.find(gif_id);
                if (found == gif_captures.end())
                    return std::int32_t{-1};
                auto capture = std::move(found->second);
                gif_captures.erase(found);
                const std::filesystem::path requested(text(1));
                const auto path = requested.is_absolute() ? requested : save_dir / requested;
                std::error_code error;
                if (!path.parent_path().empty())
                    std::filesystem::create_directories(path.parent_path(), error);
                if (error)
                    return std::int32_t{-1};
                try {
                    return std::int32_t{write_gif(capture, path) ? 0 : -1};
                } catch (const std::bad_alloc &) {
                    return std::int32_t{-1};
                }
            }
            if (name == "screen_save" || name == "screen_save_part") {
                if (args.empty() || (name == "screen_save_part" && args.size() < 5))
                    throw Error(ErrorKind::vm, std::string(name) + " requires a filename and crop bounds");
                const std::filesystem::path requested(text(0));
                const auto path = requested.is_absolute() ? requested : save_dir / requested;
                std::error_code error;
                if (!path.parent_path().empty())
                    std::filesystem::create_directories(path.parent_path(), error);
                if (error)
                    throw Error(ErrorKind::io, "cannot create screenshot directory");
                auto save_image = [&](const Canvas &image) {
                    auto extension = path.extension().string();
                    std::transform(extension.begin(), extension.end(), extension.begin(),
                                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                    if (extension == ".bmp")
                        image.save_bmp(path);
                    else if (extension.empty() || extension == ".png")
                        image.save_png(path);
                    else
                        throw Error(ErrorKind::unsupported, "screenshots support PNG and BMP filenames");
                };
                if (name == "screen_save")
                    save_image(this->canvas);
                else {
                    const int x = number(args[1]), y = number(args[2]), width = number(args[3]),
                              height = number(args[4]);
                    if (width <= 0 || height <= 0 || width > 8192 || height > 8192)
                        return std::monostate{};
                    Canvas cropped(width, height);
                    const auto source = this->canvas.pixels();
                    auto destination = cropped.pixels();
                    for (int row = 0; row < height; ++row)
                        for (int column = 0; column < width; ++column) {
                            const int source_x = x + column, source_y = y + row;
                            if (source_x >= 0 && source_y >= 0 && source_x < this->canvas.width() &&
                                source_y < this->canvas.height())
                                destination[static_cast<std::size_t>(row) * width + column] =
                                    source[static_cast<std::size_t>(source_y) * this->canvas.width() +
                                           source_x];
                        }
                    save_image(cropped);
                }
                return std::monostate{};
            }
            if (name == "sprite_add") {
                if (args.size() < 6 || number(args[1]) != 0)
                    return std::int32_t{-1};
                const std::filesystem::path requested(text(0));
                std::array<std::filesystem::path, 3> candidates{
                    requested.is_absolute() ? requested : game_dir / requested,
                    requested.is_absolute() ? requested : save_dir / requested, requested};
                for (const auto &candidate : candidates) {
                    std::error_code error;
                    if (!std::filesystem::is_regular_file(candidate, error))
                        continue;
                    return std::int32_t{assets.create_sprite_from_file(candidate, value_number(args[2]) != 0,
                                                                       value_number(args[3]) != 0,
                                                                       number(args[4]), number(args[5]))};
                }
                return std::int32_t{-1};
            }
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
                std::filesystem::copy_file(source, destination,
                                           std::filesystem::copy_options::overwrite_existing, error);
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
                    throw Error(ErrorKind::vm,
                                std::string(name) + " requires buffer, filename, offset and size");
                auto &buffer = buffers.at(static_cast<int>(n(0)));
                const auto offset = static_cast<std::size_t>(std::clamp(n(2), 0.0, 64.0 * 1024 * 1024));
                const double requested_size = n(3);
                const auto size =
                    requested_size < 0
                        ? 64U * 1024U * 1024U
                        : static_cast<std::size_t>(std::clamp(requested_size, 0.0, 64.0 * 1024 * 1024));
                const auto filename = text(1);
                const auto path = save_path(filename);
                bool succeeded = false;
                if (name == "buffer_load_async") {
                    std::ifstream input(path, std::ios::binary);
                    if (!input)
                        input.open(game_dir / filename, std::ios::binary);
                    if (input) {
                        std::vector<std::uint8_t> bytes(size);
                        input.read(reinterpret_cast<char *>(bytes.data()),
                                   static_cast<std::streamsize>(size));
                        bytes.resize(static_cast<std::size_t>(input.gcount()));
                        if (offset + bytes.size() > buffer.bytes.size())
                            buffer.bytes.resize(offset + bytes.size());
                        std::copy(bytes.begin(), bytes.end(),
                                  buffer.bytes.begin() + static_cast<std::ptrdiff_t>(offset));
                        succeeded = static_cast<bool>(input) || input.eof();
                    }
                } else {
                    if (!prepare_save_directory())
                        return std::int32_t{-1};
                    std::ofstream output(path, std::ios::binary | std::ios::trunc);
                    if (output && offset < buffer.bytes.size()) {
                        const auto count = std::min(size, buffer.bytes.size() - offset);
                        output.write(reinterpret_cast<const char *>(buffer.bytes.data() + offset),
                                     static_cast<std::streamsize>(count));
                    }
                    succeeded = static_cast<bool>(output);
                }
                const auto request_id = next_async_request++;
                const int map_id = next_ds++;
                auto [async_map, inserted] = maps.try_emplace(map_id);
                (void)inserted;
                async_map->second.set({true, "id"}, Value{std::int32_t{request_id}}, false);
                async_map->second.set({true, "status"}, Value{succeeded}, false);
                const auto previous_async_load = engine.at("async_load");
                engine["async_load"] = std::int32_t{map_id};
                const auto event_targets = instances;
                for (const auto &target : event_targets)
                    if (target->alive &&
                        find_event_owner(objects[static_cast<std::size_t>(target->object)], {7, 72}) >= 0)
                        event(target, 7, 72);
                engine["async_load"] = previous_async_load;
                maps.erase(map_id);
                return std::int32_t{request_id};
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
                if (ini.opened())
                    call("ini_close", {}, self, other);
                std::string contents_storage;
                const auto contents = text_view(0, contents_storage);
                if (name == "ini_open") {
                    ini.open_file(save_path(std::string(contents)));
                } else {
                    ini.open_from_string(contents);
                }
                return std::monostate{};
            }
            if (name == "ini_read_real" || name == "ini_read_string") {
                std::string section_storage, key_storage;
                const auto section = text_view(0, section_storage);
                const auto key = text_view(1, key_storage);
                if (name == "ini_read_string")
                    return ini.read_string(section, key, args[2]);
                return ini.read_real(section, key, args[2]);
            }
            if (name == "ini_write_real" || name == "ini_write_string") {
                std::string section_storage, key_storage, value_storage;
                ini.write(text_view(0, section_storage), text_view(1, key_storage),
                          text_view(2, value_storage));
                return std::monostate{};
            }
            if (name == "ini_section_exists") {
                std::string section_storage;
                return ini.section_exists(text_view(0, section_storage));
            }
            if (name == "ini_key_exists") {
                std::string section_storage, key_storage;
                return ini.key_exists(text_view(0, section_storage), text_view(1, key_storage));
            }
            if (name == "ini_section_delete") {
                std::string section_storage;
                ini.delete_section(text_view(0, section_storage));
                return std::monostate{};
            }
            if (name == "ini_key_delete") {
                std::string section_storage, key_storage;
                ini.delete_key(text_view(0, section_storage), text_view(1, key_storage));
                return std::monostate{};
            }
            if (name == "ini_close") {
                ini.close([&] { return prepare_save_directory(); });
                return std::monostate{};
            }
            return std::nullopt;
        };
        if (auto handled = dispatch_builtin(3, dispatch_builtins_3))
            return std::move(*handled);
        auto dispatch_builtins_4 = [&]() REGML_NOINLINE_LAMBDA -> std::optional<Value> {
            if (name == "steam_initialised" || name == "steam_is_overlay_enabled" ||
                name == "steam_file_exists")
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
            if (name == "randomize" || name == "randomise") {
                if (!fixed_seed)
                    random.seed(static_cast<std::uint32_t>(
                        std::chrono::steady_clock::now().time_since_epoch().count()));
                return std::monostate{};
            }
            if (name == "random_set_seed") {
                const auto seed = static_cast<std::uint32_t>(number(args[0]));
                fixed_seed = seed;
                random.seed(seed);
                return std::monostate{};
            }
            auto animation_channel_struct = [](const AnimationCurveChannel &channel) {
                StructPtr result = make_struct();
                result->variables["name"] = std::string(channel.name);
                result->variables["type"] = std::int32_t{channel.type};
                result->variables["iterations"] = static_cast<double>(channel.iterations);
                ArrayPtr points(new ArrayValue);
                points->row_lengths[0] = static_cast<std::int32_t>(channel.points.size());
                for (std::size_t i = 0; i < channel.points.size(); ++i) {
                    StructPtr point = make_struct();
                    point->variables["posx"] = channel.points[i].x;
                    point->variables["value"] = channel.points[i].value;
                    points->elements[static_cast<std::int32_t>(i)] = std::move(point);
                }
                result->variables["points"] = std::move(points);
                return result;
            };
            auto find_animation_curve = [&](const Value &value) -> const AnimationCurve * {
                if (value_is_string(value)) {
                    const auto curve_name = value_text(value);
                    const auto found =
                        std::find_if(animation_curves.begin(), animation_curves.end(),
                                     [&](const AnimationCurve &curve) { return curve.name == curve_name; });
                    return found == animation_curves.end() ? nullptr : &*found;
                }
                if (std::holds_alternative<std::int32_t>(value) || std::holds_alternative<double>(value)) {
                    const int id = number(value);
                    return id >= 0 && static_cast<std::size_t>(id) < animation_curves.size()
                               ? &animation_curves[static_cast<std::size_t>(id)]
                               : nullptr;
                }
                if (const auto *structure = std::get_if<StructPtr>(&value); structure && *structure) {
                    const auto found = (*structure)->variables.find("name");
                    if (found != (*structure)->variables.end() && value_is_string(found->second)) {
                        const auto curve_name = value_text(found->second);
                        const auto curve = std::find_if(
                            animation_curves.begin(), animation_curves.end(),
                            [&](const AnimationCurve &entry) { return entry.name == curve_name; });
                        return curve == animation_curves.end() ? nullptr : &*curve;
                    }
                }
                return nullptr;
            };
            auto evaluate_animation_points = [](std::span<const AnimationCurvePoint> points, int type,
                                                double position) {
                if (points.empty())
                    return 0.0;
                if (points.size() == 1 || position <= points.front().x)
                    return points.front().value;
                if (position >= points.back().x)
                    return points.back().value;
                const auto upper =
                    std::upper_bound(points.begin(), points.end(), position,
                                     [](double x, const AnimationCurvePoint &point) { return x < point.x; });
                const auto right_index = static_cast<std::size_t>(upper - points.begin());
                const auto left_index = right_index - 1;
                const auto &left = points[left_index];
                const auto &right = points[right_index];
                const double span = right.x - left.x;
                if (span <= 0)
                    return right.value;
                const double t = std::clamp((position - left.x) / span, 0.0, 1.0);
                if (type == 0)
                    return left.value + (right.value - left.value) * t;
                if (type == 2) {
                    const auto cubic = [](double a, double b, double c, double d, double t_value) {
                        const double one_minus_t = 1 - t_value;
                        return one_minus_t * one_minus_t * one_minus_t * a +
                               3 * one_minus_t * one_minus_t * t_value * b +
                               3 * one_minus_t * t_value * t_value * c + t_value * t_value * t_value * d;
                    };
                    const double control_x1 = left.x + left.bezier_x1;
                    const double control_x2 = right.x + right.bezier_x0;
                    const double control_y1 = left.value + left.bezier_y1;
                    const double control_y2 = right.value + right.bezier_y0;
                    double lo = 0, hi = 1;
                    for (int i = 0; i < 24; ++i) {
                        const double mid = (lo + hi) * 0.5;
                        if (cubic(left.x, control_x1, control_x2, right.x, mid) < position)
                            lo = mid;
                        else
                            hi = mid;
                    }
                    return cubic(left.value, control_y1, control_y2, right.value, (lo + hi) * 0.5);
                }
                const auto &p0 = points[left_index == 0 ? left_index : left_index - 1];
                const auto &p3 = points[std::min(right_index + 1, points.size() - 1)];
                const double t2 = t * t, t3 = t2 * t;
                return 0.5 * ((2 * left.value) + (-p0.value + right.value) * t +
                              (2 * p0.value - 5 * left.value + 4 * right.value - p3.value) * t2 +
                              (-p0.value + 3 * left.value - 3 * right.value + p3.value) * t3);
            };
            if (name == "audio_get_recorder_count")
                return std::int32_t{audio_recorder_count()};
            if (name == "audio_get_recorder_info") {
                if (args.empty())
                    throw Error(ErrorKind::vm, "audio_get_recorder_info requires a recorder index");
                const int index = number(args[0]);
                std::string device_name;
#if REGML_WITH_SDL2 && !REGML_WEB
                if (index >= 0 && index < audio_recorder_count())
                    if (const char *name = SDL_GetAudioDeviceName(index, SDL_TRUE))
                        device_name = name;
#endif
                const int map_id = next_ds++;
                auto [info, inserted] = maps.try_emplace(map_id);
                (void)inserted;
                info->second.set({true, "name"}, Value{std::move(device_name)}, false);
                info->second.set({true, "index"}, Value{std::int32_t{index}}, false);
                info->second.set({true, "data_format"}, Value{std::int32_t{4}}, false);
                info->second.set({true, "sample_rate"}, Value{std::int32_t{16000}}, false);
                info->second.set({true, "channels"}, Value{std::int32_t{1}}, false);
                return std::int32_t{map_id};
            }
            if (name == "audio_start_recording") {
                if (args.empty())
                    throw Error(ErrorKind::vm, "audio_start_recording requires a recorder index");
#if REGML_WITH_SDL2 && !REGML_WEB
                const int source_index = number(args[0]);
                if (source_index < 0 || source_index >= audio_recorder_count())
                    return std::int32_t{-1};
                const char *device_name = SDL_GetAudioDeviceName(source_index, SDL_TRUE);
                if (!device_name)
                    return std::int32_t{-1};
                const int channel = next_audio_recording_channel++;
                auto [recorder_it, inserted] = audio_recorders.try_emplace(channel);
                (void)inserted;
                auto &recorder = recorder_it->second;
                try {
                    recorder.pending.reserve(128 * 1024);
                } catch (const std::bad_alloc &) {
                    audio_recorders.erase(recorder_it);
                    return std::int32_t{-1};
                }
                SDL_AudioSpec requested{}, obtained{};
                requested.freq = 16000;
                requested.format = AUDIO_S16SYS;
                requested.channels = 1;
                requested.samples = 512;
                requested.callback = &Impl::recorder_callback;
                requested.userdata = &recorder;
                const auto device =
                    SDL_OpenAudioDevice(device_name, SDL_TRUE, &requested, &obtained,
                                        SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
                if (!device || obtained.format != AUDIO_S16SYS || obtained.freq <= 0 ||
                    obtained.channels == 0) {
                    if (device)
                        SDL_CloseAudioDevice(device);
                    audio_recorders.erase(recorder_it);
                    return std::int32_t{-1};
                }
                recorder.device = device;
                recorder.format = obtained;
                SDL_PauseAudioDevice(device, 0);
                return std::int32_t{channel};
#else
                return std::int32_t{-1};
#endif
            }
            if (name == "audio_stop_recording") {
                if (args.empty())
                    throw Error(ErrorKind::vm, "audio_stop_recording requires a recording channel");
#if REGML_WITH_SDL2 && !REGML_WEB
                const int channel = number(args[0]);
                if (const auto found = audio_recorders.find(channel); found != audio_recorders.end()) {
                    SDL_PauseAudioDevice(found->second.device, 1);
                    SDL_CloseAudioDevice(found->second.device);
                    audio_recorders.erase(found);
                }
#endif
                return std::monostate{};
            }
            if (name == "animcurve_get" || name == "animcurve_get_channel" ||
                name == "animcurve_get_channel_index") {
                if (args.empty())
                    throw Error(ErrorKind::vm, std::string(name) + " requires a curve");
                const auto *curve = find_animation_curve(args[0]);
                if (!curve)
                    throw Error(ErrorKind::vm, "animation curve resource does not exist");
                if (name == "animcurve_get") {
                    StructPtr result = make_struct();
                    result->variables["name"] = std::string(curve->name);
                    ArrayPtr channels(new ArrayValue);
                    channels->row_lengths[0] = static_cast<std::int32_t>(curve->channels.size());
                    for (std::size_t i = 0; i < curve->channels.size(); ++i)
                        channels->elements[static_cast<std::int32_t>(i)] =
                            animation_channel_struct(curve->channels[i]);
                    result->variables["channels"] = std::move(channels);
                    return result;
                }
                if (args.size() < 2)
                    throw Error(ErrorKind::vm, std::string(name) + " requires a channel name or index");
                std::size_t channel_index = curve->channels.size();
                if (value_is_string(args[1])) {
                    const auto channel_name = value_text(args[1]);
                    for (std::size_t i = 0; i < curve->channels.size(); ++i)
                        if (curve->channels[i].name == channel_name) {
                            channel_index = i;
                            break;
                        }
                } else if (std::holds_alternative<std::int32_t>(args[1]) ||
                           std::holds_alternative<double>(args[1])) {
                    const int index = number(args[1]);
                    if (index >= 0)
                        channel_index = static_cast<std::size_t>(index);
                }
                if (channel_index >= curve->channels.size()) {
                    if (name == "animcurve_get_channel_index")
                        return std::int32_t{-1};
                    throw Error(ErrorKind::vm, "animation curve channel does not exist");
                }
                return name == "animcurve_get_channel_index"
                           ? Value{static_cast<std::int32_t>(channel_index)}
                           : Value{animation_channel_struct(curve->channels[channel_index])};
            }
            if (name == "animcurve_channel_evaluate") {
                if (args.size() < 2)
                    throw Error(ErrorKind::vm, "animcurve_channel_evaluate requires channel and position");
                const auto *channel_ptr = std::get_if<StructPtr>(&args[0]);
                if (!channel_ptr || !*channel_ptr)
                    return 0.0;
                const auto type_value = (*channel_ptr)->variables.find("type");
                const auto points_value = (*channel_ptr)->variables.find("points");
                if (type_value == (*channel_ptr)->variables.end() ||
                    points_value == (*channel_ptr)->variables.end())
                    return 0.0;
                const auto *points = std::get_if<ArrayPtr>(&points_value->second);
                if (!points || !*points)
                    return 0.0;
                const auto length = (*points)->row_lengths.find(0);
                if (length == (*points)->row_lengths.end() || length->second <= 0)
                    return 0.0;
                std::vector<AnimationCurvePoint> values;
                values.reserve(static_cast<std::size_t>(length->second));
                for (std::int32_t i = 0; i < length->second; ++i) {
                    const auto *point_value = (*points)->elements.find(i);
                    const auto *point_ptr = point_value ? std::get_if<StructPtr>(point_value) : nullptr;
                    if (!point_ptr || !*point_ptr)
                        continue;
                    const auto x = (*point_ptr)->variables.find("posx");
                    const auto y = (*point_ptr)->variables.find("value");
                    if (x == (*point_ptr)->variables.end() || y == (*point_ptr)->variables.end())
                        continue;
                    values.push_back({value_number(x->second), value_number(y->second)});
                }
                const double position = std::clamp(n(1), 0.0, 1.0);
                return evaluate_animation_points(values, static_cast<int>(value_number(type_value->second)),
                                                 position);
            }
            auto audio_falloff_gain = [&](double x, double y, double z, double reference, double maximum,
                                          double factor) {
                if (audio_falloff_model == 0)
                    return 1.0;
                const double dx = x - audio_listener_position[0];
                const double dy = y - audio_listener_position[1];
                const double dz = z - audio_listener_position[2];
                const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (!std::isfinite(distance) || !std::isfinite(reference) || !std::isfinite(maximum) ||
                    !std::isfinite(factor) || maximum <= reference || reference <= 0)
                    return distance <= std::max(0.0, maximum) ? 1.0 : 0.0;
                const bool clamped =
                    audio_falloff_model == 2 || audio_falloff_model == 5 || audio_falloff_model == 7;
                const bool scaled = audio_falloff_model == 3 || audio_falloff_model == 8;
                const double d =
                    clamped ? std::clamp(distance, reference, maximum) : std::min(distance, maximum);
                double gain = 1;
                switch (audio_falloff_model) {
                case 1:
                case 2:
                case 3:
                    gain = reference / (reference + factor * (d - reference));
                    break;
                case 4:
                case 5:
                    gain = 1 - factor * (d - reference) / (maximum - reference);
                    break;
                case 6:
                case 7:
                case 8:
                    gain = std::pow(d / reference, -factor);
                    break;
                default:
                    return 1.0;
                }
                gain = std::clamp(gain, 0.0, 1.0);
                if (scaled && distance > reference && maximum > 0) {
                    const double remaining =
                        std::clamp((maximum - distance) / (maximum - reference), 0.0, 1.0);
                    gain *= std::pow(remaining, distance / maximum);
                }
                return gain;
            };
            auto refresh_audio_emitter = [&](AudioEmitterState &emitter) {
                std::erase_if(emitter.voices, [&](const auto &entry) {
                    const int voice = entry.first;
                    if (!audio.playing(voice) && !audio.is_paused(voice))
                        return true;
                    audio.gain(voice,
                               entry.second[0] * emitter.gain *
                                   audio_falloff_gain(emitter.x, emitter.y, emitter.z, emitter.falloff_ref,
                                                      emitter.falloff_max, emitter.falloff_factor));
                    audio.pitch(voice, entry.second[1] * emitter.pitch);
                    return false;
                });
            };
            if (name == "audio_falloff_set_model") {
                audio_falloff_model = std::clamp(number(args[0]), 0, 8);
                for (auto &[id, emitter] : audio_emitters)
                    refresh_audio_emitter(emitter);
                return std::monostate{};
            }
            if (name == "audio_emitter_create") {
                const int id = next_audio_emitter++;
                audio_emitters.try_emplace(id);
                return std::int32_t{id};
            }
            if (name == "audio_emitter_exists")
                return audio_emitters.contains(number(args[0]));
            if (name == "audio_emitter_free") {
                const int id = number(args[0]);
                if (const auto emitter = audio_emitters.find(id); emitter != audio_emitters.end()) {
                    for (const auto &[voice, properties] : emitter->second.voices)
                        audio.stop(voice);
                    audio_emitters.erase(emitter);
                }
                return std::monostate{};
            }
            if (name == "audio_listener_position") {
                audio_listener_position = {n(0), n(1), n(2)};
                for (auto &[id, emitter] : audio_emitters)
                    refresh_audio_emitter(emitter);
                return std::monostate{};
            }
            if (name == "audio_listener_orientation") {
                for (std::size_t i = 0; i < audio_listener_orientation.size(); ++i)
                    audio_listener_orientation[i] = n(i);
                return std::monostate{};
            }
            if (name == "audio_emitter_position" || name == "audio_emitter_velocity" ||
                name == "audio_emitter_falloff" || name == "audio_emitter_gain" ||
                name == "audio_emitter_pitch" || name == "audio_emitter_get_gain" ||
                name == "audio_emitter_get_pitch" || name == "audio_emitter_get_x" ||
                name == "audio_emitter_get_y" || name == "audio_emitter_get_z" ||
                name == "audio_emitter_get_vx" || name == "audio_emitter_get_vy" ||
                name == "audio_emitter_get_vz") {
                auto &emitter = audio_emitters.at(number(args[0]));
                if (name == "audio_emitter_position") {
                    emitter.x = n(1);
                    emitter.y = n(2);
                    emitter.z = n(3);
                    refresh_audio_emitter(emitter);
                } else if (name == "audio_emitter_velocity") {
                    emitter.vx = n(1);
                    emitter.vy = n(2);
                    emitter.vz = n(3);
                } else if (name == "audio_emitter_falloff") {
                    emitter.falloff_ref = n(1);
                    emitter.falloff_max = n(2);
                    emitter.falloff_factor = n(3);
                    refresh_audio_emitter(emitter);
                } else if (name == "audio_emitter_gain") {
                    emitter.gain = n(1);
                    refresh_audio_emitter(emitter);
                } else if (name == "audio_emitter_pitch") {
                    emitter.pitch = n(1);
                    refresh_audio_emitter(emitter);
                } else if (name == "audio_emitter_get_gain")
                    return emitter.gain;
                else if (name == "audio_emitter_get_pitch")
                    return emitter.pitch;
                else if (name == "audio_emitter_get_x")
                    return emitter.x;
                else if (name == "audio_emitter_get_y")
                    return emitter.y;
                else if (name == "audio_emitter_get_z")
                    return emitter.z;
                else if (name == "audio_emitter_get_vx")
                    return emitter.vx;
                else if (name == "audio_emitter_get_vy")
                    return emitter.vy;
                else
                    return emitter.vz;
                return std::monostate{};
            }
            if (name == "audio_play_sound_on") {
                auto &emitter = audio_emitters.at(number(args[0]));
                const int voice = audio.play(number(args[1]), n(2) != 0, n(3));
                if (voice >= 0) {
                    const double base_gain = args.size() > 4 ? n(4) : 1.0;
                    const double base_pitch = args.size() > 6 ? std::max(0.0001, n(6)) : 1.0;
                    emitter.voices[voice] = {base_gain, base_pitch};
                    audio.gain(voice,
                               base_gain * emitter.gain *
                                   audio_falloff_gain(emitter.x, emitter.y, emitter.z, emitter.falloff_ref,
                                                      emitter.falloff_max, emitter.falloff_factor));
                    audio.pitch(voice, base_pitch * emitter.pitch);
                    if (args.size() > 5)
                        audio.position(voice, n(5));
                }
                return std::int32_t{voice};
            }
            if (name == "audio_play_sound_at") {
                const double x = n(1), y = n(2), z = n(3);
                const double reference = n(4), maximum = n(5), factor = n(6);
                const int voice = audio.play(number(args[0]), n(7) != 0, n(8));
                if (voice >= 0) {
                    const double gain = args.size() > 9 ? n(9) : 1.0;
                    audio.gain(voice, gain * audio_falloff_gain(x, y, z, reference, maximum, factor));
                    if (args.size() > 10)
                        audio.position(voice, n(10));
                    if (args.size() > 11)
                        audio.pitch(voice, std::max(0.0001, n(11)));
                }
                return std::int32_t{voice};
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
            if (name == "audio_exists")
                return audio.sound_exists(static_cast<int>(n(0)));
            if (name == "audio_get_name")
                return std::string(audio.sound_name(static_cast<int>(n(0))));
            if (gmrt_project && name.starts_with("audio_")) {
                if (name == "audio_play_sound" || name == "audio_create_stream")
                    return std::int32_t{-1};
                if (name == "audio_is_playing")
                    return false;
                if (name == "audio_is_paused")
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
            if (name == "audio_is_paused")
                return audio.is_paused(static_cast<int>(n(0)));
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
            return std::nullopt;
        };
        if (auto handled = dispatch_builtin(4, dispatch_builtins_4))
            return std::move(*handled);
        auto dispatch_builtins_5 = [&]() REGML_NOINLINE_LAMBDA -> std::optional<Value> {
            if (name == "texture_prefetch") {
                if (!args.empty() && value_is_string(args[0])) {
                    const auto group = text(0);
                    if (assets.prefetch_texture_group(group))
                        loaded_texture_groups.insert(group);
                    return std::monostate{};
                }
                const auto found = sprite_textures.find(static_cast<int>(n(0)));
                if (found != sprite_textures.end())
                    assets.prefetch(found->second.first);
                return std::monostate{};
            }
            if (name == "texture_is_ready") {
                if (!args.empty() && value_is_string(args[0]))
                    return loaded_texture_groups.contains(text(0));
                return sprite_textures.contains(static_cast<int>(n(0))) ||
                       (texture_surfaces.contains(static_cast<int>(n(0))) &&
                        surface_exists(texture_surfaces.at(static_cast<int>(n(0)))));
            }
            if (name == "texture_flush") {
                if (!args.empty() && value_is_string(args[0])) {
                    const auto group = text(0);
                    assets.flush_texture_group(group);
                    loaded_texture_groups.erase(group);
                }
                return std::monostate{};
            }
            if (name == "texturegroup_get_textures")
                return ArrayPtr(new ArrayValue);
            if (name == "sprite_get_texture")
                return std::int32_t{sprite_texture_handle(static_cast<int>(n(0)), n(1))};
            if (name == "sprite_get_uvs" || name == "texture_get_uvs") {
                int sprite_id = -1;
                double frame = 0;
                if (name == "sprite_get_uvs") {
                    sprite_id = number(args[0]);
                    frame = args.size() > 1 ? n(1) : 0;
                } else if (const auto texture = sprite_textures.find(number(args[0]));
                           texture != sprite_textures.end()) {
                    sprite_id = texture->second.first;
                    frame = texture->second.second;
                }
                std::array<double, 8> values{};
                if (sprite_id >= 0) {
                    values = assets.sprite_uvs(sprite_id, frame);
                } else if (name == "texture_get_uvs") {
                    const int texture_handle = number(args[0]);
                    if (const auto surface = texture_surfaces.find(texture_handle);
                        surface != texture_surfaces.end() && surface_exists(surface->second))
                        values = {0, 0, 1, 1, 0, 0, 1, 1};
                }
                ArrayPtr result(new ArrayValue);
                result->row_lengths[0] = static_cast<std::int32_t>(values.size());
                for (std::size_t index = 0; index < values.size(); ++index)
                    result->elements[static_cast<std::int32_t>(index)] = values[index];
                return result;
            }
            if (name == "texture_get_texel_width" || name == "texture_get_texel_height" ||
                name == "texture_get_width" || name == "texture_get_height") {
                const auto texture = sprite_textures.find(number(args[0]));
                double texel_width = 0.0;
                double texel_height = 0.0;
                if (texture != sprite_textures.end()) {
                    assets.texture_texel_size(texture->second.first, texture->second.second, texel_width,
                                              texel_height);
                } else if (const auto surface = texture_surfaces.find(number(args[0]));
                           surface != texture_surfaces.end() && surface_exists(surface->second)) {
                    const auto &canvas = surface->second == 0 ? this->canvas : surfaces.at(surface->second);
                    texel_width = canvas.width() > 0 ? 1.0 / canvas.width() : 0.0;
                    texel_height = canvas.height() > 0 ? 1.0 / canvas.height() : 0.0;
                } else {
                    return 0.0;
                }
                if (name == "texture_get_texel_width")
                    return texel_width;
                if (name == "texture_get_texel_height")
                    return texel_height;
                return name == "texture_get_width" ? (texel_width > 0 ? 1.0 / texel_width : 0.0)
                                                   : (texel_height > 0 ? 1.0 / texel_height : 0.0);
            }
            if (name == "gpu_set_texrepeat" || name == "gpu_set_tex_repeat") {
                gpu_texture_repeat = args.empty() || n(0) != 0;
                return std::monostate{};
            }
            if (name == "gpu_get_texrepeat" || name == "gpu_get_tex_repeat")
                return gpu_texture_repeat;
            return std::nullopt;
        };
        if (auto handled = dispatch_builtin(5, dispatch_builtins_5))
            return std::move(*handled);
        auto dispatch_builtins_6 = [&]() REGML_NOINLINE_LAMBDA -> std::optional<Value> {
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
                    std::erase_if(layer->backgrounds, [element_id](const LayerBackground &background) {
                        return background.id == element_id;
                    });
                return std::monostate{};
            }
            if (name == "layer_background_get_id" || name == "layer_background_get_sprite" ||
                name == "layer_background_get_index" || name == "layer_background_get_blend" ||
                name == "layer_background_get_alpha" || name == "layer_background_get_htiled" ||
                name == "layer_background_get_vtiled" || name == "layer_background_get_stretch" ||
                name == "layer_background_get_xscale" || name == "layer_background_get_yscale" ||
                name == "layer_background_get_visible" || name == "layer_background_get_speed") {
                const auto background = find_layer_background(static_cast<int>(n(0)));
                if (!background.second)
                    return name == "layer_background_get_id" || name == "layer_background_get_sprite"
                               ? Value{std::int32_t{-1}}
                               : Value{0.0};
                const auto &value = *background.second;
                if (name == "layer_background_get_id")
                    return std::int32_t{value.id};
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
                if (name == "layer_background_get_speed")
                    return value.speed;
                return value.visible;
            }
            if (name == "layer_background_change" || name == "layer_background_sprite" ||
                name == "layer_background_blend" || name == "layer_background_alpha" ||
                name == "layer_background_htiled" || name == "layer_background_vtiled" ||
                name == "layer_background_stretch" || name == "layer_background_xscale" ||
                name == "layer_background_yscale" || name == "layer_background_visible" ||
                name == "layer_background_index" || name == "layer_background_speed") {
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
                        value.color =
                            (value.color & 0xff000000U) | (static_cast<unsigned>(setting) & 0xffffffU);
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
                    else if (name == "layer_background_speed")
                        value.speed = setting;
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
                    const auto found =
                        std::find_if(current.layers.begin(), current.layers.end(),
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
                const int layer_id = layer_id_argument(0);
                const int depth = static_cast<int>(n(1));
                if (auto *layer = find_layer(layer_id)) {
                    layer->depth = depth;
                    for (const auto &instance : instances)
                        if (instance->alive && !instance->manual_depth &&
                            number(instance->vars.at("layer")) == layer_id)
                            instance->vars["depth"] = depth;
                    if (current_room >= 0 && static_cast<std::size_t>(current_room) < rooms.size()) {
                        auto &tiles = rooms[static_cast<std::size_t>(current_room)].tiles;
                        for (auto &tile : tiles)
                            if (tile.layer_id == layer_id)
                                tile.depth = depth;
                        std::stable_sort(tiles.begin(), tiles.end(), [](const Tile &left, const Tile &right) {
                            return left.depth > right.depth;
                        });
                    }
                }
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
                    const auto instance = find_alive_instance_by_id(instance_id);
                    if (instance) {
                        event(instance, 1, 0);
                        instance->alive = false;
                    }
                }
                std::erase_if(current.tiles,
                              [layer_id](const Tile &tile) { return tile.layer_id == layer_id; });
                std::erase_if(current.layers,
                              [layer_id](const RoomLayer &layer) { return layer.id == layer_id; });
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
            if (name == "layer_tilemap_get_id") {
                const auto *layer = find_layer(layer_id_argument(0));
                return std::int32_t{layer ? layer->tile_element_id : -1};
            }
            if (name == "layer_tilemap_exists") {
                const auto *layer = find_layer(layer_id_argument(0));
                return layer && layer->tile_element_id == static_cast<int>(n(1));
            }
            if (name == "draw_tile") {
                if (args.size() < 5)
                    throw Error(ErrorKind::vm,
                                "draw_tile requires a tileset, tile data, frame, and position");
                const int tileset = number(args[0]);
                const auto tile_data = static_cast<std::uint32_t>(number(args[1]));
                const auto tile_index = tile_data & 0x7ffffU;
                if (tile_index == 0)
                    return std::int32_t{0};
                const double frame_value = std::floor(value_number(args[2]));
                const auto frame =
                    std::isfinite(frame_value) && frame_value > 0
                        ? static_cast<std::size_t>(std::min(
                              frame_value, static_cast<double>(std::numeric_limits<std::uint32_t>::max())))
                        : std::size_t{0};
                const auto source = assets.background_tile_source(tileset, tile_index, 0, frame);
                if (!source)
                    return std::int32_t{0};
                const auto alpha = static_cast<unsigned>(std::clamp(draw_alpha, 0.0, 1.0) * 255.0 + 0.5);
                const auto color = (draw_color & 0x00ffffffU) | (alpha << 24);
                assets.draw_tile(current_canvas(), tileset, source->x, source->y, source->width,
                                 source->height, value_number(args[3]), value_number(args[4]), 1, 1, color,
                                 tile_data);
                return std::int32_t{0};
            }
            if (name == "draw_tilemap") {
                if (args.size() < 3)
                    throw Error(ErrorKind::vm, "draw_tilemap requires a tilemap ID and position");
                const int tilemap_id = static_cast<int>(n(0));
                const auto *layer = find_tilemap_layer(tilemap_id);
                if (!layer || current_room < 0 || static_cast<std::size_t>(current_room) >= rooms.size())
                    return std::monostate{};

                const double draw_x = n(1), draw_y = n(2);
                const int cell_width = layer->tile_width;
                const int cell_height = layer->tile_height;
                if (cell_width <= 0 || cell_height <= 0)
                    return std::monostate{};

                const auto elapsed_us =
                    static_cast<std::uint64_t>(std::max(0.0, value_number(engine["current_time"])) * 1000.0);
                for (const auto &tile : rooms[static_cast<std::size_t>(current_room)].tiles) {
                    if (tile.element_id != tilemap_id || tile.cell_x < 0 || tile.cell_y < 0)
                        continue;
                    int source_x = tile.source_x;
                    int source_y = tile.source_y;
                    if (tile.modern_tileset) {
                        const auto source =
                            assets.background_tile_source(tile.id, tile.tileset_index, elapsed_us);
                        if (!source)
                            continue;
                        source_x = source->x;
                        source_y = source->y;
                    }
                    assets.draw_tile(current_canvas(), tile.id, source_x, source_y, tile.width, tile.height,
                                     draw_x + static_cast<double>(tile.cell_x) * cell_width,
                                     draw_y + static_cast<double>(tile.cell_y) * cell_height, 1, 1,
                                     tile.color, tile.tile_data);
                }
                return std::monostate{};
            }
            if (name == "tilemap_set" || name == "tilemap_set_at_pixel") {
                if (args.size() < 4)
                    throw Error(ErrorKind::vm, std::string(name) + " requires tile data and coordinates");
                auto *layer = find_tilemap_layer(static_cast<int>(n(0)));
                if (!layer)
                    return false;
                auto &offset = tile_layer_offsets[layer->id];
                const double map_x = layer->x - layer->origin_x + offset.first;
                const double map_y = layer->y - layer->origin_y + offset.second;
                int cell_x, cell_y;
                if (name == "tilemap_set") {
                    cell_x = static_cast<int>(std::floor(n(2)));
                    cell_y = static_cast<int>(std::floor(n(3)));
                } else {
                    if (layer->tile_width <= 0 || layer->tile_height <= 0)
                        return false;
                    cell_x = static_cast<int>(std::floor((n(2) - map_x) / layer->tile_width));
                    cell_y = static_cast<int>(std::floor((n(3) - map_y) / layer->tile_height));
                }
                if (cell_x < 0 || cell_y < 0 || cell_x >= layer->tilemap_width ||
                    cell_y >= layer->tilemap_height)
                    return false;
                auto &tiles = rooms[static_cast<std::size_t>(current_room)].tiles;
                const auto existing = std::find_if(tiles.begin(), tiles.end(), [&](const Tile &tile) {
                    return tile.element_id == layer->tile_element_id && tile.cell_x == cell_x &&
                           tile.cell_y == cell_y;
                });
                const auto data = static_cast<std::uint32_t>(number(args[1]));
                const auto index = data & 0x7ffffU;
                if (index == 0) {
                    if (existing != tiles.end())
                        tiles.erase(existing);
                    return true;
                }
                const auto source = assets.background_tile_source(layer->tile_set, index);
                if (!source)
                    return false;
                Tile tile;
                tile.id = layer->tile_set;
                tile.element_id = layer->tile_element_id;
                tile.cell_x = cell_x;
                tile.cell_y = cell_y;
                tile.tile_data = data;
                // Tile coordinates are layer-local; draw_tile() applies the layer
                // position and origin when it renders the tile.
                tile.x = static_cast<double>(cell_x) * source->width;
                tile.y = static_cast<double>(cell_y) * source->height;
                tile.depth = layer->depth;
                tile.source_x = source->x;
                tile.source_y = source->y;
                tile.width = source->width;
                tile.height = source->height;
                tile.layer_id = layer->id;
                tile.tileset_index = index;
                tile.transform = data & ((1U << 28) | (1U << 29) | (1U << 30));
                tile.modern_tileset = true;
                layer->tile_width = source->width;
                layer->tile_height = source->height;
                if (existing == tiles.end()) {
                    // The room draw loop consumes this vector in descending depth order.
                    // Keep runtime-created cells in that order too, so a platform swap
                    // cannot make the remaining map draw after unrelated foregrounds.
                    const auto insert_at =
                        std::upper_bound(tiles.begin(), tiles.end(), tile.depth,
                                         [](int depth, const Tile &entry) { return depth > entry.depth; });
                    tiles.insert(insert_at, std::move(tile));
                } else {
                    *existing = std::move(tile);
                }
                return true;
            }
            if (name == "tilemap_clear" || name == "tilemap_set_width" || name == "tilemap_set_height" ||
                name == "tilemap_tileset") {
                auto *layer = find_tilemap_layer(static_cast<int>(n(0)));
                if (!layer)
                    return std::monostate{};
                if (name == "tilemap_clear") {
                    auto &tiles = rooms[static_cast<std::size_t>(current_room)].tiles;
                    std::erase_if(
                        tiles, [&](const Tile &tile) { return tile.element_id == layer->tile_element_id; });
                    return std::monostate{};
                }
                if (name == "tilemap_set_width" || name == "tilemap_set_height") {
                    const int size = std::clamp(number(n(1)), 0, 32000);
                    if (name == "tilemap_set_width")
                        layer->tilemap_width = size;
                    else
                        layer->tilemap_height = size;
                    auto &tiles = rooms[static_cast<std::size_t>(current_room)].tiles;
                    std::erase_if(tiles, [&](const Tile &tile) {
                        return tile.element_id == layer->tile_element_id &&
                               (tile.cell_x < 0 || tile.cell_y < 0 || tile.cell_x >= layer->tilemap_width ||
                                tile.cell_y >= layer->tilemap_height);
                    });
                    return std::monostate{};
                }
                const int tile_set = static_cast<int>(n(1));
                layer->tile_set = tile_set;
                auto &tiles = rooms[static_cast<std::size_t>(current_room)].tiles;
                for (auto &tile : tiles) {
                    if (tile.element_id != layer->tile_element_id)
                        continue;
                    const auto source = assets.background_tile_source(tile_set, tile.tileset_index);
                    if (!source)
                        continue;
                    tile.id = tile_set;
                    tile.width = source->width;
                    tile.height = source->height;
                    tile.source_x = source->x;
                    tile.source_y = source->y;
                    tile.x = static_cast<double>(tile.cell_x * source->width);
                    tile.y = static_cast<double>(tile.cell_y * source->height);
                }
                return std::monostate{};
            }
            if (name == "tilemap_get_frame")
                return 0.0;
            if (name == "tilemap_get" || name == "tilemap_get_at_pixel" ||
                name == "tilemap_get_tileset" || name == "tilemap_get_width" ||
                name == "tilemap_get_height" || name == "tilemap_get_tile_width" ||
                name == "tilemap_get_tile_height" || name == "tilemap_get_x" || name == "tilemap_get_y" ||
                name == "tilemap_x" || name == "tilemap_y") {
                auto *layer = find_tilemap_layer(static_cast<int>(n(0)));
                if (!layer)
                    return name == "tilemap_get_tileset" || name == "tilemap_get" ||
                                   name == "tilemap_get_at_pixel"
                               ? Value{std::int32_t{-1}}
                               : Value{0.0};
                auto &offset = tile_layer_offsets[layer->id];
                const double map_x = layer->x - layer->origin_x + offset.first;
                const double map_y = layer->y - layer->origin_y + offset.second;
                if (name == "tilemap_get_tileset")
                    return std::int32_t{layer->tile_set};
                if (name == "tilemap_get_width")
                    return std::int32_t{layer->tilemap_width};
                if (name == "tilemap_get_height")
                    return std::int32_t{layer->tilemap_height};
                if (name == "tilemap_get_tile_width")
                    return std::int32_t{layer->tile_width};
                if (name == "tilemap_get_tile_height")
                    return std::int32_t{layer->tile_height};
                if (name == "tilemap_get_x")
                    return map_x;
                if (name == "tilemap_get_y")
                    return map_y;
                if (name == "tilemap_x" || name == "tilemap_y") {
                    const double value = n(1);
                    if (name == "tilemap_x")
                        offset.first = value - (layer->x - layer->origin_x);
                    else
                        offset.second = value - (layer->y - layer->origin_y);
                    return std::monostate{};
                }
                int cell_x = 0, cell_y = 0;
                if (name == "tilemap_get") {
                    cell_x = static_cast<int>(std::floor(n(1)));
                    cell_y = static_cast<int>(std::floor(n(2)));
                } else {
                    if (layer->tile_width <= 0 || layer->tile_height <= 0)
                        return name == "tilemap_get_at_pixel" ? Value{std::int32_t{-1}}
                                                              : Value{std::int32_t{0}};
                    cell_x = static_cast<int>(std::floor((n(1) - map_x) / layer->tile_width));
                    cell_y = static_cast<int>(std::floor((n(2) - map_y) / layer->tile_height));
                }
                if (cell_x < 0 || cell_y < 0 || cell_x >= layer->tilemap_width ||
                    cell_y >= layer->tilemap_height)
                    return std::int32_t{-1};
                const auto &room = rooms[static_cast<std::size_t>(current_room)];
                const auto tile = std::find_if(room.tiles.begin(), room.tiles.end(), [&](const Tile &entry) {
                    return entry.element_id == layer->tile_element_id && entry.cell_x == cell_x &&
                           entry.cell_y == cell_y;
                });
                return tile == room.tiles.end() ? Value{std::int32_t{0}}
                                                : Value{static_cast<std::int32_t>(tile->tile_data)};
            }
            if (name == "layer_get_all_elements") {
                const auto *layer = find_layer(layer_id_argument(0));
                std::vector<int> ids;
                if (layer) {
                    ids.reserve(layer->instances.size() + layer->sprites.size() + layer->backgrounds.size() +
                                1);
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
                if (current_room >= 0 && static_cast<std::size_t>(current_room) < rooms.size()) {
                    const auto &current = rooms[static_cast<std::size_t>(current_room)];
                    for (const auto &layer : current.layers)
                        if (layer.tile_element_id == id)
                            return std::int32_t{5};
                }
                if (find_layer_tile(id).second)
                    return std::int32_t{3};
                if (current_room >= 0 && static_cast<std::size_t>(current_room) < rooms.size()) {
                    const auto &current = rooms[static_cast<std::size_t>(current_room)];
                    for (const auto &layer : current.layers)
                        if (std::find(layer.instances.begin(), layer.instances.end(), id) !=
                            layer.instances.end())
                            return std::int32_t{2};
                }
                return std::int32_t{-1};
            }
            if (name == "layer_instance_get_instance") {
                const int element_id = number(args[0]);
                const auto instance = find_alive_instance_by_id(element_id);
                return std::int32_t{instance ? instance->id : -1};
            }
            if (name == "layer_get_element_layer") {
                const int element_id = number(args[0]);
                if (const auto sprite = find_layer_sprite(element_id); sprite.first)
                    return std::int32_t{sprite.first->id};
                if (const auto background = find_layer_background(element_id); background.first)
                    return std::int32_t{background.first->id};
                if (const auto tile = find_layer_tile(element_id); tile.first)
                    return std::int32_t{tile.first->id};
                if (current_room >= 0 && static_cast<std::size_t>(current_room) < rooms.size()) {
                    const auto &current = rooms[static_cast<std::size_t>(current_room)];
                    for (const auto &layer : current.layers)
                        if (std::find(layer.instances.begin(), layer.instances.end(), element_id) !=
                            layer.instances.end())
                            return std::int32_t{layer.id};
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
                    const double legacy_offset =
                        offset == tile_layer_offsets.end()
                            ? 0
                            : (name == "layer_tile_x" ? offset->second.first : offset->second.second);
                    const double layer_offset =
                        tile.first ? (name == "layer_tile_x" ? tile.first->x - tile.first->origin_x
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
            if (name == "layer_sprite_get_id") {
                const auto *layer = find_layer(layer_id_argument(0));
                if (!layer)
                    return std::int32_t{-1};
                const auto found =
                    std::find_if(layer->sprites.begin(), layer->sprites.end(),
                                 [&](const LayerSprite &sprite) { return sprite.name == text(1); });
                return std::int32_t{found == layer->sprites.end() ? -1 : found->id};
            }
            if (name == "layer_sprite_exists") {
                const auto sprite = find_layer_sprite(number(args[1]));
                return sprite.first && sprite.second && sprite.first->id == layer_id_argument(0);
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
            if (name == "layer_sprite_change" || name == "layer_sprite_index" ||
                name == "layer_sprite_speed" || name == "layer_sprite_x" || name == "layer_sprite_y" ||
                name == "layer_sprite_xscale" || name == "layer_sprite_yscale" ||
                name == "layer_sprite_angle" || name == "layer_sprite_alpha" ||
                name == "layer_sprite_blend") {
                const auto sprite = find_layer_sprite(static_cast<int>(n(0)));
                if (sprite.second) {
                    if (name == "layer_sprite_change") {
                        const int sprite_id = number(args[1]);
                        if (sprite_id == -1 || assets.sprite_exists(sprite_id)) {
                            sprite.second->sprite = sprite_id;
                            sprite.second->frame = 0;
                        }
                    } else if (name == "layer_sprite_index")
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
                        sprite.second->color =
                            (sprite.second->color & 0x00ffffffU) |
                            (static_cast<unsigned>(std::clamp(n(1), 0.0, 1.0) * 255) << 24);
                    else
                        sprite.second->color =
                            (sprite.second->color & 0xff000000U) | (static_cast<unsigned>(n(1)) & 0xffffffU);
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
            if (name == "sprite_duplicate")
                return std::int32_t{assets.duplicate_sprite(number(args[0]))};
            if (name == "sprite_get_info") {
                const int id = number(args[0]);
                const GameAssets::Sprite *sprite = assets.sprite_exists(id) ? &assets.sprite(id) : nullptr;
                const int sprite_width = sprite ? sprite->width : 0;
                const int sprite_height = sprite ? sprite->height : 0;
                StructPtr info = make_struct();
                info->variables["type"] = std::int32_t{0};
                info->variables["width"] = std::int32_t{sprite_width};
                info->variables["height"] = std::int32_t{sprite_height};
                info->variables["xoffset"] = std::int32_t{sprite ? sprite->ox : 0};
                info->variables["yoffset"] = std::int32_t{sprite ? sprite->oy : 0};
                info->variables["transparent"] = false;
                info->variables["smooth"] = false;
                info->variables["slots"] = ArrayPtr(new ArrayValue);
                info->variables["messages"] = ArrayPtr(new ArrayValue);
                ArrayPtr frames(new ArrayValue);
                // Some games pass the no-sprite sentinel through sprite_get_info
                // before checking the result. Keep frame fields scalar so scripts
                // that inspect a frame can safely draw nothing for that instance.
                const auto frame_count = sprite && !sprite->frames.empty() ? sprite->frames.size() : 1;
                frames->row_lengths[0] = static_cast<std::int32_t>(frame_count);
                frames->elements.reserve_dense(frame_count);
                for (std::size_t frame_index = 0; frame_index < frame_count; ++frame_index) {
                    const auto frame_info =
                        sprite && frame_index < sprite->frames.size()
                            ? assets.sprite_frame_info(id, frame_index)
                                  .value_or(GameAssets::SpriteFrameInfo{0, 0, 0, 0, -1, sprite_width,
                                                                        sprite_height})
                            : GameAssets::SpriteFrameInfo{0, 0, 0, 0, -1, sprite_width, sprite_height};
                    StructPtr frame = make_struct();
                    frame->variables["x"] = std::int32_t{frame_info.x};
                    frame->variables["y"] = std::int32_t{frame_info.y};
                    frame->variables["w"] = std::int32_t{frame_info.width};
                    frame->variables["h"] = std::int32_t{frame_info.height};
                    frame->variables["texture"] = std::int32_t{frame_info.texture};
                    frame->variables["original_width"] = std::int32_t{frame_info.original_width};
                    frame->variables["original_height"] = std::int32_t{frame_info.original_height};
                    frame->variables["crop_width"] = std::int32_t{frame_info.crop_width};
                    frame->variables["crop_height"] = std::int32_t{frame_info.crop_height};
                    frame->variables["x_offset"] = std::int32_t{frame_info.x_offset};
                    frame->variables["y_offset"] = std::int32_t{frame_info.y_offset};
                    frames->elements[static_cast<std::int32_t>(frame_index)] = frame;
                }
                info->variables["frames"] = frames;
                return info;
            }
            if (name == "sprite_get_speed_type")
                return std::int32_t{0};
            if (name == "sprite_exists")
                return assets.sprite_exists(static_cast<int>(n(0)));
            if (name == "sprite_get_name") {
                const int id = static_cast<int>(n(0));
                return assets.sprite_exists(id) ? std::string(assets.sprite_name(id)) : std::string{};
            }
            if (name == "sprite_set_offset") {
                assets.set_sprite_offset(static_cast<int>(n(0)), static_cast<int>(n(1)),
                                         static_cast<int>(n(2)));
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
                return std::int32_t{assets.create_sprite_from_surface(
                    *source, x, y, width, height, remove_background, smooth, origin_x, origin_y)};
            }
            if (name == "sprite_delete")
                return assets.delete_sprite(number(args[0]));
            if (name == "sprite_replace")
                return false;
            if (name == "sprite_collision_mask")
                return std::monostate{};
            if (name == "background_get_width" || name == "background_get_height") {
                const auto [width, height] = assets.background_size(static_cast<int>(n(0)));
                return std::int32_t{name == "background_get_width" ? width : height};
            }
            if (name == "sprite_get_width" || name == "sprite_get_height" || name == "sprite_get_xoffset" ||
                name == "sprite_get_yoffset") {
                const int id = static_cast<int>(n(0));
                if (!assets.sprite_exists(id))
                    return std::int32_t{0};
                const auto &spr = assets.sprite(id);
                if (name == "sprite_get_width")
                    return std::int32_t{spr.width};
                if (name == "sprite_get_height")
                    return std::int32_t{spr.height};
                if (name == "sprite_get_xoffset")
                    return std::int32_t{spr.ox};
                return std::int32_t{spr.oy};
            }
            if (name == "sprite_set_bbox") {
                assets.set_sprite_bbox(number(args[0]), number(args[1]), number(args[2]), number(args[3]),
                                       number(args[4]));
                return std::monostate{};
            }
            if (name == "sprite_set_bbox_mode") {
                assets.set_sprite_bbox_mode(number(args[0]), number(args[1]));
                return std::monostate{};
            }
            if (name == "sprite_get_bbox_mode") {
                const int id = number(args[0]);
                return std::int32_t{assets.sprite_exists(id) ? assets.sprite_bbox_mode(id) : -1};
            }
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
                current_canvas().color(draw_color);
                return std::monostate{};
            }
            if (name == "draw_get_color")
                return static_cast<double>(draw_color);
            if (name == "draw_set_alpha") {
                draw_alpha = n(0);
                current_canvas().alpha(draw_alpha);
                return std::monostate{};
            }
            if (name == "draw_get_alpha")
                return draw_alpha;
            if (name == "font_add_sprite_ext")
                return static_cast<std::int32_t>(assets.add_sprite_font(static_cast<int>(n(0)), text(1),
                                                                        n(2) != 0, static_cast<int>(n(3))));
            if (name == "draw_set_font") {
                draw_font = static_cast<int>(n(0));
                return std::monostate{};
            }
            if (name == "draw_get_font")
                return std::int32_t{draw_font};
            if (name == "draw_set_halign") {
                halign = static_cast<int>(n(0));
                return std::monostate{};
            }
            if (name == "draw_get_halign")
                return std::int32_t{halign};
            if (name == "matrix_build_identity")
                return matrix_value(identity_matrix());
            if (name == "matrix_get" || name == "matrix_set") {
                const int type = number(args[0]);
                if (type < 0 || type >= static_cast<int>(transform_matrices.size()))
                    throw Error(ErrorKind::vm, "matrix type out of range");
                if (name == "matrix_get")
                    return matrix_value(transform_matrices[static_cast<std::size_t>(type)]);
                transform_matrices[static_cast<std::size_t>(type)] = matrix_from_value(args[1]);
                return std::monostate{};
            }
            if (name == "matrix_multiply") {
                Matrix4 result = multiply_matrix(matrix_from_value(args[0]), matrix_from_value(args[1]));
                if (args.size() > 2) {
                    const auto *destination = std::get_if<ArrayPtr>(&args[2]);
                    if (!destination || !*destination)
                        throw Error(ErrorKind::vm, "matrix_multiply result must be an array");
                    for (std::size_t i = 0; i < result.size(); ++i)
                        (*destination)->elements[static_cast<std::int32_t>(i)] = result[i];
                    (*destination)->row_lengths[0] = 16;
                    return *destination;
                }
                return matrix_value(result);
            }
            if (name == "matrix_build_lookat" || name == "matrix_build_projection_ortho" ||
                name == "matrix_build_projection_perspective" ||
                name == "matrix_build_projection_perspective_fov") {
                Matrix4 result{};
                if (name == "matrix_build_lookat") {
                    if (args.size() < 9)
                        throw Error(ErrorKind::vm, "matrix_build_lookat requires nine coordinates");
                    const double ex = n(0), ey = n(1), ez = n(2);
                    double fx = n(3) - ex, fy = n(4) - ey, fz = n(5) - ez;
                    double ux = n(6), uy = n(7), uz = n(8);
                    const double forward_length = std::hypot(std::hypot(fx, fy), fz);
                    if (forward_length > 0) {
                        fx /= forward_length;
                        fy /= forward_length;
                        fz /= forward_length;
                    }
                    double sx = fy * uz - fz * uy, sy = fz * ux - fx * uz, sz = fx * uy - fy * ux;
                    const double side_length = std::hypot(std::hypot(sx, sy), sz);
                    if (side_length > 0) {
                        sx /= side_length;
                        sy /= side_length;
                        sz /= side_length;
                    }
                    ux = sy * fz - sz * fy;
                    uy = sz * fx - sx * fz;
                    uz = sx * fy - sy * fx;
                    result = {sx,
                              ux,
                              -fx,
                              0,
                              sy,
                              uy,
                              -fy,
                              0,
                              sz,
                              uz,
                              -fz,
                              0,
                              -(sx * ex + sy * ey + sz * ez),
                              -(ux * ex + uy * ey + uz * ez),
                              fx * ex + fy * ey + fz * ez,
                              1};
                } else {
                    const double first = n(0), second = n(1), near_plane = n(2), far_plane = n(3);
                    const double distance = far_plane - near_plane;
                    if (distance == 0)
                        throw Error(ErrorKind::vm, "projection near and far planes cannot be equal");
                    if (name == "matrix_build_projection_ortho") {
                        if (first == 0 || second == 0)
                            throw Error(ErrorKind::vm, "orthographic projection dimensions cannot be zero");
                        result = {2 / first,
                                  0,
                                  0,
                                  0,
                                  0,
                                  2 / second,
                                  0,
                                  0,
                                  0,
                                  0,
                                  1 / distance,
                                  0,
                                  -1,
                                  -1,
                                  -near_plane / distance,
                                  1};
                    } else {
                        double scale_x = 0, scale_y = 0;
                        if (name == "matrix_build_projection_perspective_fov") {
                            if (second == 0)
                                throw Error(ErrorKind::vm, "perspective aspect ratio cannot be zero");
                            scale_y = 1.0 / std::tan(first * 3.141592653589793 / 360.0);
                            scale_x = scale_y / second;
                        } else {
                            if (first == 0 || second == 0 || near_plane == 0)
                                throw Error(ErrorKind::vm, "perspective dimensions cannot be zero");
                            scale_x = 2 * near_plane / first;
                            scale_y = 2 * near_plane / second;
                        }
                        result = {scale_x,
                                  0,
                                  0,
                                  0,
                                  0,
                                  scale_y,
                                  0,
                                  0,
                                  0,
                                  0,
                                  far_plane / distance,
                                  1,
                                  0,
                                  0,
                                  -near_plane * far_plane / distance,
                                  0};
                    }
                }
                if (args.size() > (name == "matrix_build_lookat" ? 9U : 4U)) {
                    const auto *destination =
                        std::get_if<ArrayPtr>(&args[name == "matrix_build_lookat" ? 9 : 4]);
                    if (!destination || !*destination)
                        throw Error(ErrorKind::vm, "matrix result must be an array");
                    for (std::size_t i = 0; i < result.size(); ++i)
                        (*destination)->elements[static_cast<std::int32_t>(i)] = result[i];
                    (*destination)->row_lengths[0] = 16;
                    return *destination;
                }
                return matrix_value(result);
            }
            if (name == "camera_create" || name == "camera_create_view") {
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
            if (name == "camera_destroy") {
                const int id = static_cast<int>(n(0));
                cameras.erase(id);
                for (int view = 0; view < 8; ++view)
                    if (number(view_value("view_camera", view, std::int32_t{view})) == id)
                        engine["view_camera" + view_suffix(view)] = std::int32_t{-1};
                if (active_camera_id == id)
                    active_camera_id = -1;
                return std::monostate{};
            }
            if (name == "camera_get_active")
                return std::int32_t{active_camera_id >= 0 ? active_camera_id : camera_for_view(0)};
            if (name == "camera_get_default")
                return std::int32_t{camera_for_view(0)};
            if (name == "camera_set_default") {
                const int id = number(args[0]);
                (void)camera_for_id(id);
                active_camera_id = id;
                engine["view_camera[0]"] = std::int32_t{id};
                sync_camera_to_views(id);
                return std::monostate{};
            }
            if (name == "room_get_camera") {
                if (args.size() < 2)
                    throw Error(ErrorKind::vm, "room_get_camera requires a room and view index");
                const int room_id = number(args[0]), view = std::clamp(number(args[1]), 0, 7);
                if (room_id < 0 || static_cast<std::size_t>(room_id) >= rooms.size())
                    return std::int32_t{-1};
                const auto key = "view_camera[" + std::to_string(view) + "]";
                if (room_id == current_room) {
                    if (const auto value = engine.find(key); value != engine.end())
                        return std::int32_t{number(value->second)};
                }
                if (const auto value = rooms[static_cast<std::size_t>(room_id)].views.find(key);
                    value != rooms[static_cast<std::size_t>(room_id)].views.end())
                    return std::int32_t{number(value->second)};
                return std::int32_t{-1};
            }
            if (name == "camera_set_view_mat" || name == "camera_set_proj_mat") {
                auto &camera = camera_for_id(number(args[0]));
                auto &target = name == "camera_set_view_mat" ? camera.view_matrix : camera.projection_matrix;
                target = matrix_from_value(args[1]);
                return std::monostate{};
            }
            if (name == "camera_get_view_mat" || name == "camera_get_proj_mat") {
                const auto &camera = camera_for_id(number(args[0]));
                return matrix_value(name == "camera_get_view_mat" ? camera.view_matrix
                                                                  : camera.projection_matrix);
            }
            if (name == "camera_apply") {
                active_camera_id = static_cast<int>(n(0));
                (void)camera_for_id(active_camera_id);
                sync_camera_to_views(active_camera_id);
                return true;
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
                const std::string_view field = name == "view_get_visible" ? "view_visible"
                                               : name == "view_get_xport" ? "view_xport"
                                               : name == "view_get_yport" ? "view_yport"
                                               : name == "view_get_wport" ? "view_wport"
                                               : name == "view_get_hport" ? "view_hport"
                                                                          : "view_surface_id";
                return view_value(field, view, std::int32_t{field == "view_surface_id" ? -1 : 0});
            }
            if (name == "view_set_visible" || name == "view_set_xport" || name == "view_set_yport" ||
                name == "view_set_wport" || name == "view_set_hport" || name == "view_set_surface_id") {
                const int view = view_index();
                const std::string_view field = name == "view_set_visible" ? "view_visible"
                                               : name == "view_set_xport" ? "view_xport"
                                               : name == "view_set_yport" ? "view_yport"
                                               : name == "view_set_wport" ? "view_wport"
                                               : name == "view_set_hport" ? "view_hport"
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
                assets.draw_text(current_canvas(), draw_font, n(0), n(1), text_view(2, storage), draw_color,
                                 draw_alpha, halign, valign);
                return std::monostate{};
            }
            if (name == "draw_text_color" || name == "draw_text_colour") {
                const unsigned color = static_cast<unsigned>((n(3) + n(4) + n(5) + n(6)) / 4);
                std::string storage;
                assets.draw_text(current_canvas(), draw_font, n(0), n(1), text_view(2, storage), color, n(7),
                                 halign, valign);
                return std::monostate{};
            }
            if (name == "draw_text_transformed") {
                std::string storage;
                assets.draw_text(current_canvas(), draw_font, n(0), n(1), text_view(2, storage), draw_color,
                                 draw_alpha, halign, valign, n(3), n(4), n(5));
                return std::monostate{};
            }
            if (name == "draw_text_transformed_color") {
                std::string storage;
                assets.draw_text(current_canvas(), draw_font, n(0), n(1), text_view(2, storage),
                                 static_cast<unsigned>(n(6)), draw_alpha, halign, valign, n(3), n(4), n(5));
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
                    assets.draw_text(current_canvas(), draw_font, x, y,
                                     value.substr(line_start, line_end - line_start), draw_color, draw_alpha,
                                     halign, valign, sx, sy, angle);
                    line_start = line_end;
                    line_width = 0;
                    ++line_number;
                };
                for (std::size_t index = 0; index < value.size(); ++index) {
                    const char ch = value[index];
                    const double char_width =
                        ch == '\n' || ch == '#'
                            ? 0
                            : assets.text_character_width(draw_font, static_cast<unsigned char>(ch));
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
            return std::nullopt;
        };
        if (auto handled = dispatch_builtin(6, dispatch_builtins_6))
            return std::move(*handled);
        auto dispatch_builtins_7 = [&]() REGML_NOINLINE_LAMBDA -> std::optional<Value> {
            if (name == "video_open") {
                video_player.close();
                video_start_event_sent = false;
                video_end_event_sent = false;
                if (video_surface >= 0) {
                    forget_surface_texture(video_surface);
                    surfaces.erase(video_surface);
                }
                video_surface = -1;
                const auto requested = std::filesystem::path(text(0));
                std::filesystem::path path = requested;
                std::error_code error;
                if (path.is_relative()) {
                    const auto game_path = game_dir / path;
                    path = std::filesystem::is_regular_file(game_path, error) && !error
                               ? game_path
                               : std::filesystem::current_path(error) / requested;
                }
                if (!video_player.open(path))
                    return std::monostate{};
                try {
                    video_surface = next_surface++;
                    surfaces.emplace(std::piecewise_construct, std::forward_as_tuple(video_surface),
                                     std::forward_as_tuple(video_player.width(), video_player.height()));
                    surfaces.at(video_surface).clear_transparent();
                } catch (...) {
                    video_player.close();
                    video_surface = -1;
                    return std::monostate{};
                }
                video_start_event_sent = true;
                dispatch_video_async_event("video_start");
                return std::monostate{};
            }
            if (name == "video_close") {
                video_player.close();
                video_start_event_sent = false;
                video_end_event_sent = false;
                if (video_surface >= 0) {
                    forget_surface_texture(video_surface);
                    surfaces.erase(video_surface);
                }
                video_surface = -1;
                return std::monostate{};
            }
            if (name == "video_enable_loop") {
                video_player.enable_loop(n(0) != 0);
                return std::monostate{};
            }
            if (name == "video_pause") {
                video_player.pause();
                return std::monostate{};
            }
            if (name == "video_resume") {
                video_player.resume();
                return std::monostate{};
            }
            if (name == "video_set_volume") {
                video_player.set_volume(n(0));
                return std::monostate{};
            }
            if (name == "video_get_volume")
                return video_player.volume();
            if (name == "video_get_duration")
                return video_player.duration_ms();
            if (name == "video_get_position")
                return video_player.position_ms();
            if (name == "video_get_status")
                return std::int32_t{video_player.status()};
            if (name == "video_get_format")
                return std::int32_t{0}; // desktop decode output is rgba
            if (name == "video_draw") {
                auto result = ArrayPtr(new ArrayValue);
                if (!video_player.loaded() || video_surface < 0 || !surfaces.contains(video_surface)) {
                    result->row_lengths[0] = 1;
                    result->elements[0] = std::int32_t{-1};
                    return result;
                }
                video_player.update_frame();
                if (video_player.failed()) {
                    result->row_lengths[0] = 1;
                    result->elements[0] = std::int32_t{-1};
                    return result;
                }
                if (video_player.status() == 0) {
                    if (video_start_event_sent && !video_end_event_sent) {
                        video_end_event_sent = true;
                        dispatch_video_async_event("video_end");
                    }
                    result->row_lengths[0] = 1;
                    result->elements[0] = std::int32_t{-2};
                    return result;
                }
                if (video_player.has_frame()) {
                    auto &surface = surfaces.at(video_surface);
                    if (surface.width() != video_player.width() || surface.height() != video_player.height())
                        surface = Canvas(video_player.width(), video_player.height());
                    const auto frame_pixels = video_player.pixels();
                    if (frame_pixels.size() == surface.pixels().size())
                        std::copy(frame_pixels.begin(), frame_pixels.end(), surface.pixels().begin());
                }
                result->row_lengths[0] = 2;
                result->elements[0] = std::int32_t{0};
                result->elements[1] = std::int32_t{video_surface};
                return result;
            }
            if (name == "surface_depth_disable") {
                surface_depth_disabled = n(0) != 0;
                return std::monostate{};
            }
            if (name == "surface_get_depth_disable")
                return surface_depth_disabled;
            if (name == "surface_has_depth") {
                (void)n(0);
                return false;
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
                configure_blend(surfaces.at(id));
                surfaces.at(id).color_write_enable(color_write_enable[0], color_write_enable[1],
                                                   color_write_enable[2], color_write_enable[3]);
                surfaces.at(id).blend_enable(blend_enabled);
                surfaces.at(id).alpha_test_enable(alpha_test_enabled);
                surfaces.at(id).alpha_test_ref(alpha_test_reference);
                return std::int32_t{id};
            }
            if (name == "surface_resize") {
                const int id = static_cast<int>(n(0));
                const int width = static_cast<int>(std::clamp(n(1), 1.0, 8192.0));
                const int height = static_cast<int>(std::clamp(n(2), 1.0, 8192.0));
                if (static_cast<std::uint64_t>(width) * height > 64ULL * 1024 * 1024)
                    return std::monostate{};
                if (id == 0) {
                    this->canvas = Canvas(width, height, true);
                    configure_blend(this->canvas);
                    this->canvas.color_write_enable(color_write_enable[0], color_write_enable[1],
                                                    color_write_enable[2], color_write_enable[3]);
                    this->canvas.blend_enable(blend_enabled);
                    this->canvas.alpha_test_enable(alpha_test_enabled);
                    this->canvas.alpha_test_ref(alpha_test_reference);
                } else if (surfaces.contains(id)) {
                    surfaces.at(id) = Canvas(width, height);
                    surfaces.at(id).clear_transparent();
                    configure_blend(surfaces.at(id));
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
                return surface_exists(id);
            }
            if (name == "surface_get_texture")
                return std::int32_t{surface_texture_handle(static_cast<int>(n(0)))};
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
                if (id != 0 && !surfaces.contains(id))
                    return false;
                surface_target_stack.push_back(surface_target);
                surface_target = id;
                configure_shader(current_canvas());
                return true;
            }
            if (name == "surface_reset_target") {
                surface_target = surface_target_stack.empty() ? 0 : surface_target_stack.back();
                if (!surface_target_stack.empty())
                    surface_target_stack.pop_back();
                configure_shader(current_canvas());
                return std::monostate{};
            }
            if (name == "surface_free") {
                const int id = static_cast<int>(n(0));
                if (surface_target == id) {
                    surface_target = surface_target_stack.empty() ? 0 : surface_target_stack.back();
                    if (!surface_target_stack.empty())
                        surface_target_stack.pop_back();
                }
                std::erase(surface_target_stack, id);
                forget_surface_texture(id);
                surfaces.erase(id);
                configure_shader(current_canvas());
                return std::monostate{};
            }
            if (name == "draw_surface_part_ext") {
                const int id = static_cast<int>(n(0));
                const Canvas *source = nullptr;
                if (id == 0)
                    source = &this->canvas;
                else if (const auto found = surfaces.find(id); found != surfaces.end())
                    source = &found->second;
                if (!source)
                    return std::monostate{};
                auto &target = current_canvas();
                const auto draw = [&](const Canvas &source_canvas) {
                    const auto [x, y] = assets.transform_point(n(5), n(6));
                    target.draw_surface_part(source_canvas, n(1), n(2), n(3), n(4), x, y,
                                             n(7) * assets.view_scale_x(), n(8) * assets.view_scale_y(),
                                             static_cast<unsigned>(n(9)), n(10));
                };
                if (&target == source) {
                    const Canvas source_copy = *source;
                    draw(source_copy);
                } else
                    draw(*source);
                return std::monostate{};
            }
            if (name == "draw_surface" || name == "draw_surface_ext") {
                const int id = static_cast<int>(n(0));
                const Canvas *source = nullptr;
                if (id == 0)
                    source = &this->canvas;
                else if (const auto found = surfaces.find(id); found != surfaces.end())
                    source = &found->second;
                if (!source)
                    return std::monostate{};
                const bool extended = name == "draw_surface_ext";
                auto &target = current_canvas();
                const auto draw = [&](const Canvas &source_canvas) {
                    const auto [x, y] = assets.transform_point(n(1), n(2));
                    target.draw_surface(source_canvas, x, y,
                                        (extended ? n(3) : 1) * assets.view_scale_x(),
                                        (extended ? n(4) : 1) * assets.view_scale_y(), extended ? n(5) : 0,
                                        extended ? static_cast<unsigned>(n(6)) : 0xffffff,
                                        extended ? n(7) : 1);
                };
                if (&target == source) {
                    const Canvas source_copy = *source;
                    draw(source_copy);
                } else
                    draw(*source);
                return std::monostate{};
            }
            if (name == "draw_surface_tiled_ext") {
                const int id = static_cast<int>(n(0));
                const Canvas *source_ptr = nullptr;
                if (id == 0)
                    source_ptr = &this->canvas;
                else if (const auto found = surfaces.find(id); found != surfaces.end())
                    source_ptr = &found->second;
                if (!source_ptr)
                    return std::monostate{};
                auto &target = current_canvas();
                const double sx = n(3) * assets.view_scale_x(), sy = n(4) * assets.view_scale_y();
                const double tile_width = source_ptr->width() * std::fabs(sx);
                const double tile_height = source_ptr->height() * std::fabs(sy);
                if (!std::isfinite(tile_width + tile_height) || tile_width <= 0 || tile_height <= 0)
                    return std::monostate{};
                const auto [x, y] = assets.transform_point(n(1), n(2));
                const double start_x = x + std::floor(-x / tile_width) * tile_width;
                const double start_y = y + std::floor(-y / tile_height) * tile_height;
                const double columns = std::ceil((target.logical_width() - start_x) / tile_width);
                const double rows = std::ceil((target.logical_height() - start_y) / tile_height);
                if (columns > 0 && rows > 0 && columns * rows > 1'000'000)
                    throw Error(ErrorKind::vm, "tiled surface draw limit exceeded");
                const auto draw_tiles = [&](const Canvas &source) {
                    for (double tile_y = start_y; tile_y < target.logical_height(); tile_y += tile_height)
                        for (double tile_x = start_x; tile_x < target.logical_width(); tile_x += tile_width)
                            target.draw_surface(source, tile_x, tile_y, sx, sy, 0,
                                                static_cast<unsigned>(n(5)), n(6));
                };
                if (&target == source_ptr) {
                    const Canvas source_copy = *source_ptr;
                    draw_tiles(source_copy);
                } else
                    draw_tiles(*source_ptr);
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
                assets.rectangle(current_canvas(), n(0), n(1), n(2), n(3), n(4) != 0);
                return std::monostate{};
            }
            if (name == "ossafe_fill_rectangle") {
                assets.rectangle(current_canvas(), n(0), n(1), n(2), n(3), false);
                return std::monostate{};
            }
            if (name == "draw_circle" || name == "draw_circle_color" || name == "draw_circle_colour") {
                const bool colored = name != "draw_circle";
                if (colored) {
                    draw_color = average_color_arguments(3, 2);
                    current_canvas().color(draw_color);
                }
                assets.draw_circle(current_canvas(), n(0), n(1), n(2), n(colored ? 5 : 3) != 0);
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
                    current_canvas().color(draw_color);
                }
                assets.draw_line(current_canvas(), n(0), n(1), n(2), n(3), thick ? n(4) : 1);
                return std::monostate{};
            }
            if (name == "draw_triangle" || name == "draw_triangle_color" || name == "draw_triangle_colour") {
                const bool colored = name != "draw_triangle";
                const unsigned outline_index = colored ? 9U : 6U;
                if (colored) {
                    draw_color = average_color_arguments(6, 3);
                    current_canvas().color(draw_color);
                }
                assets.draw_triangle(current_canvas(), n(0), n(1), n(2), n(3), n(4), n(5),
                                     n(outline_index) != 0);
                return std::monostate{};
            }
            if (name == "draw_ellipse" || name == "draw_ellipse_color" || name == "draw_ellipse_colour") {
                const bool colored = name != "draw_ellipse";
                if (colored) {
                    draw_color = average_color_arguments(4, 2);
                    current_canvas().color(draw_color);
                }
                assets.draw_ellipse(current_canvas(), n(0), n(1), n(2), n(3), n(colored ? 6 : 4) != 0);
                return std::monostate{};
            }
            if (name == "draw_roundrect" || name == "draw_roundrect_color" ||
                name == "draw_roundrect_colour" || name == "draw_roundrect_ext" ||
                name == "draw_roundrect_color_ext" || name == "draw_roundrect_colour_ext") {
                const bool colored = name == "draw_roundrect_color" || name == "draw_roundrect_colour" ||
                                     name == "draw_roundrect_color_ext" ||
                                     name == "draw_roundrect_colour_ext";
                const bool extended = name == "draw_roundrect_ext" || name == "draw_roundrect_color_ext" ||
                                      name == "draw_roundrect_colour_ext";
                const bool outline = n(extended ? (colored ? 8 : 6) : (colored ? 6 : 4)) != 0;
                if (colored) {
                    draw_color = average_color_arguments(extended ? 6 : 4, 2);
                    current_canvas().color(draw_color);
                }
                const double width = std::fabs(n(2) - n(0)), height = std::fabs(n(3) - n(1));
                const double radius_x = extended ? n(4) : std::min(10.0, width / 4);
                const double radius_y = extended ? n(5) : std::min(10.0, height / 4);
                assets.draw_round_rectangle(current_canvas(), n(0), n(1), n(2), n(3), radius_x, radius_y,
                                            outline);
                return std::monostate{};
            }
            if (name == "draw_rectangle_color" || name == "draw_rectangle_colour") {
                draw_color = average_color_arguments(4, 4);
                current_canvas().color(draw_color);
                assets.rectangle(current_canvas(), n(0), n(1), n(2), n(3), n(8) != 0);
                return std::monostate{};
            }
            if (name == "draw_point" || name == "draw_point_color" || name == "draw_point_colour") {
                if (name != "draw_point") {
                    draw_color = average_color_arguments(2, 2);
                    current_canvas().color(draw_color);
                }
                assets.draw_line(current_canvas(), n(0), n(1), n(0), n(1));
                return std::monostate{};
            }
            if (name == "draw_arrow") {
                const double x1 = n(0), y1 = n(1), x2 = n(2), y2 = n(3);
                const double size = std::max(0.0, n(4));
                assets.draw_line(current_canvas(), x1, y1, x2, y2);
                const double angle = std::atan2(y2 - y1, x2 - x1);
                const double wing = size * 0.55;
                constexpr double spread = 0.55;
                assets.draw_line(current_canvas(), x2, y2, x2 - size * std::cos(angle - spread),
                                 y2 - size * std::sin(angle - spread), wing);
                assets.draw_line(current_canvas(), x2, y2, x2 - size * std::cos(angle + spread),
                                 y2 - size * std::sin(angle + spread), wing);
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
                    assets.draw_line(current_canvas(), x + a.x, y + a.y, x + a.x + (b.x - a.x) * fraction,
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
            if (name == "gpu_get_blendmode_ext" || name == "gpu_get_blendmode_ext_sepalpha") {
                ArrayPtr factors(new ArrayValue);
                const int count = name == "gpu_get_blendmode_ext" ? 2 : 4;
                factors->row_lengths[0] = count;
                for (int i = 0; i < count; ++i)
                    factors->elements[i] = std::int32_t{gpu_blend_factors[static_cast<std::size_t>(i)]};
                return factors;
            }
            if (name == "gpu_get_blendmode")
                return std::int32_t{gpu_blend_mode};
            if (name == "gpu_get_blendequation")
                return std::int32_t{gpu_blend_equations[0]};
            if (name == "gpu_get_blendequation_sepalpha") {
                ArrayPtr equations(new ArrayValue);
                equations->row_lengths[0] = 2;
                equations->elements[0] = std::int32_t{gpu_blend_equations[0]};
                equations->elements[1] = std::int32_t{gpu_blend_equations[1]};
                return equations;
            }
            if (name == "gpu_set_blendmode") {
                gpu_blend_mode = number(args[0]);
                gpu_blend_equations = {0, 0};
                // presets are normal add max substract and reverse-subtract
                if (gpu_blend_mode == 0)
                    gpu_blend_factors = {5, 6, 5, 6};
                else if (gpu_blend_mode == 1)
                    gpu_blend_factors = {5, 2, 5, 2};
                else if (gpu_blend_mode == 2)
                    gpu_blend_factors = {5, 4, 5, 4};
                else if (gpu_blend_mode == 3)
                    gpu_blend_factors = {1, 4, 1, 4};
                else if (gpu_blend_mode == 4) {
                    gpu_blend_factors = {2, 2, 2, 2};
                    gpu_blend_equations = {3, 3};
                } else if (gpu_blend_mode == 5) {
                    gpu_blend_factors = {5, 2, 5, 2};
                    gpu_blend_equations = {2, 2};
                }
                configure_blend();
                return std::monostate{};
            }
            if (name == "gpu_set_blendequation" || name == "gpu_set_blendequation_sepalpha") {
                if (args.empty() || (name == "gpu_set_blendequation_sepalpha" && args.size() < 2))
                    throw Error(ErrorKind::vm, std::string(name) + " requires an equation");
                gpu_blend_equations[0] = number(args[0]);
                gpu_blend_equations[1] =
                    name == "gpu_set_blendequation_sepalpha" ? number(args[1]) : gpu_blend_equations[0];
                configure_blend();
                return std::monostate{};
            }
            if (name == "gpu_set_blendmode_ext" || name == "gpu_set_blendmode_ext_sepalpha") {
                const bool separate_alpha = name == "gpu_set_blendmode_ext_sepalpha";
                if (args.size() == 1) {
                    const auto *values = std::get_if<ArrayPtr>(&args[0]);
                    if (!values || !*values)
                        throw Error(ErrorKind::vm, "extended blend mode requires factors or an array");
                    const int count = separate_alpha ? 4 : 2;
                    for (int i = 0; i < count; ++i) {
                        const auto *factor = (*values)->elements.find(i);
                        if (!factor)
                            throw Error(ErrorKind::vm, "extended blend mode array has too few factors");
                        gpu_blend_factors[static_cast<std::size_t>(i)] = number(*factor);
                    }
                    if (!separate_alpha) {
                        gpu_blend_factors[2] = gpu_blend_factors[0];
                        gpu_blend_factors[3] = gpu_blend_factors[1];
                    }
                } else {
                    const int count = separate_alpha ? 4 : 2;
                    if (args.size() < static_cast<std::size_t>(count))
                        throw Error(ErrorKind::vm, "extended blend mode requires more factors");
                    for (int i = 0; i < count; ++i)
                        gpu_blend_factors[static_cast<std::size_t>(i)] =
                            number(args[static_cast<std::size_t>(i)]);
                    if (!separate_alpha) {
                        gpu_blend_factors[2] = gpu_blend_factors[0];
                        gpu_blend_factors[3] = gpu_blend_factors[1];
                    }
                }
                gpu_blend_mode = -1;
                configure_blend();
                return std::monostate{};
            }
            if (name == "gpu_set_fog" || name == "gpu_set_texfilter" || name == "gpu_set_tex_filter" ||
                name == "gpu_set_ztestenable" || name == "gpu_set_zwriteenable" ||
                name == "gpu_set_cullmode" || name == "draw_set_circle_precision") {
                return std::monostate{};
            }
            if (name == "draw_clear_alpha") {
                // GameMaker drawing operations only take effect from Draw events.
                // Scripts called by an event inherit the event frame, so checking
                // the active frame also handles draw_clear calls made in scripts.
                if (event_stack.empty() || event_stack.back().type != event_draw)
                    return std::monostate{};
                current_canvas().clear(static_cast<unsigned>(n(0)));
                return std::monostate{};
            }
            if (name == "draw_clear") {
                if (event_stack.empty() || event_stack.back().type != event_draw)
                    return std::monostate{};
                current_canvas().clear(static_cast<unsigned>(n(0)));
                return std::monostate{};
            }
            if (name == "draw_background") {
                assets.draw_background(current_canvas(), static_cast<int>(n(0)), n(1), n(2));
                return std::monostate{};
            }
            if (name == "draw_background_stretched") {
                assets.draw_background_stretched(current_canvas(), static_cast<int>(n(0)), n(1), n(2), n(3),
                                                 n(4));
                return std::monostate{};
            }
            if (name == "draw_background_part_ext") {
                assets.draw_background_part_ext(current_canvas(), static_cast<int>(n(0)), n(1), n(2), n(3),
                                                n(4), n(5), n(6), n(7), n(8), static_cast<unsigned>(n(9)),
                                                n(10));
                return std::monostate{};
            }
            if (name == "draw_sprite_tiled_ext") {
                const int sprite = static_cast<int>(n(0));
                const auto &info = assets.sprite(sprite);
                const double frame_index = n(1), x = n(2), y = n(3), sx = n(4), sy = n(5);
                const double tile_width = info.width * std::fabs(sx);
                const double tile_height = info.height * std::fabs(sy);
                if (!std::isfinite(tile_width + tile_height + x + y) || tile_width <= 0 || tile_height <= 0)
                    return std::monostate{};
                const double width = current_canvas().logical_width(),
                             height = current_canvas().logical_height();
                const double start_x = x + std::floor(-x / tile_width) * tile_width;
                const double start_y = y + std::floor(-y / tile_height) * tile_height;
                const double columns = std::ceil((width - start_x) / tile_width);
                const double rows = std::ceil((height - start_y) / tile_height);
                if (columns > 0 && rows > 0 && columns * rows > 1'000'000)
                    throw Error(ErrorKind::vm, "tiled sprite draw limit exceeded");
                for (double tile_y = start_y; tile_y < height; tile_y += tile_height)
                    for (double tile_x = start_x; tile_x < width; tile_x += tile_width)
                        assets.draw_sprite(current_canvas(), sprite, frame_index, tile_x, tile_y, sx, sy, 0,
                                           static_cast<unsigned>(n(6)), n(7));
                return std::monostate{};
            }
            if (name == "draw_sprite") {
                const auto f = n(1) < 0 ? value_number(self->vars.at("image_index")) : n(1);
                assets.draw_sprite(current_canvas(), static_cast<int>(n(0)), f, n(2), n(3), 1, 1, 0, 0xffffff,
                                   draw_alpha);
                return std::monostate{};
            }
            if (name == "draw_sprite_pos") {
                if (args.size() < 11)
                    throw Error(ErrorKind::vm, "draw_sprite_pos requires 11 arguments");
                const double frame = n(1) < 0 ? value_number(self->vars.at("image_index")) : n(1);
                std::array<double, 8> corners{};
                for (std::size_t i = 0; i < corners.size(); ++i)
                    corners[i] = n(i + 2);
                assets.draw_sprite_pos(current_canvas(), static_cast<int>(n(0)), frame, corners, n(10));
                return std::monostate{};
            }
            if (name == "draw_sprite_ext") {
                assets.draw_sprite(current_canvas(), static_cast<int>(n(0)),
                                   n(1) < 0 ? value_number(self->vars.at("image_index")) : n(1), n(2), n(3),
                                   n(4), n(5), n(6), static_cast<unsigned>(n(7)), n(8));
                return std::monostate{};
            }
            if (name == "draw_sprite_stretched") {
                const auto sprite = static_cast<int>(n(0));
                const auto &info = assets.sprite(sprite);
                const double sx = info.width ? n(4) / info.width : 1,
                             sy = info.height ? n(5) / info.height : 1;
                assets.draw_sprite(current_canvas(), sprite,
                                   n(1) < 0 ? value_number(self->vars.at("image_index")) : n(1), n(2), n(3),
                                   sx, sy, 0, 0xffffff, draw_alpha);
                return std::monostate{};
            }
            if (name == "draw_sprite_stretched_ext") {
                const int sprite = static_cast<int>(n(0));
                const auto &info = assets.sprite(sprite);
                const double frame = n(1) < 0 ? value_number(self->vars.at("image_index")) : n(1);
                const double sx = info.width ? n(4) / info.width : 1;
                const double sy = info.height ? n(5) / info.height : 1;
                assets.draw_sprite(current_canvas(), sprite, frame, n(2), n(3), sx, sy, 0,
                                   static_cast<unsigned>(n(6)), n(7));
                return std::monostate{};
            }
            if (name == "draw_self") {
                const int sprite = static_cast<int>(value_number(self->vars.at("sprite_index")));
                if (sprite >= 0)
                    assets.draw_sprite(current_canvas(), sprite, value_number(self->vars.at("image_index")),
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
                if (x < 0 || y < 0 || x >= current_canvas().logical_width() ||
                    y >= current_canvas().logical_height())
                    return std::int32_t{-1};
                const auto pixel =
                    current_canvas().pixel(static_cast<int>(x * current_canvas().render_scale()),
                                           static_cast<int>(y * current_canvas().render_scale()));
                return std::int32_t{
                    static_cast<int>(((pixel >> 16) & 0xff) | (pixel & 0xff00) | ((pixel & 0xff) << 16))};
            }
            if (name == "draw_sprite_part") {
                assets.draw_sprite_part(current_canvas(), static_cast<int>(n(0)),
                                        n(1) < 0 ? value_number(self->vars.at("image_index")) : n(1), n(2),
                                        n(3), n(4), n(5), n(6), n(7), 1, 1, 0xffffff, draw_alpha);
                return std::monostate{};
            }
            if (name == "draw_sprite_part_ext") {
                assets.draw_sprite_part(current_canvas(), static_cast<int>(n(0)),
                                        n(1) < 0 ? value_number(self->vars.at("image_index")) : n(1), n(2),
                                        n(3), n(4), n(5), n(6), n(7), n(8), n(9),
                                        static_cast<unsigned>(n(10)), n(11));
                return std::monostate{};
            }
            if (name == "draw_sprite_general") {
                if (args.size() < 16)
                    throw Error(ErrorKind::vm, "draw_sprite_general requires 16 arguments");
                const double frame = n(1) < 0 ? value_number(self->vars.at("image_index")) : n(1);
                const std::array<unsigned, 4> colors = {
                    static_cast<unsigned>(n(11)), static_cast<unsigned>(n(12)),
                    static_cast<unsigned>(n(13)), static_cast<unsigned>(n(14))};
                assets.draw_sprite_general(current_canvas(), static_cast<int>(n(0)), frame, n(2), n(3), n(4),
                                           n(5), n(6), n(7), n(8), n(9), n(10), colors, n(15));
                return std::monostate{};
            }
            return std::nullopt;
        };
        if (auto handled = dispatch_builtin(7, dispatch_builtins_7))
            return std::move(*handled);
        auto dispatch_builtins_8 = [&]() REGML_NOINLINE_LAMBDA -> std::optional<Value> {
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
            if (name == "place_free") {
                // GameMaker's place_free checks the caller's collision mask against
                // solid instances only. Coordinates are quantized to whole pixels
                // before the collision check, matching the engine's movement API.
                const double x = std::floor(n(0)), y = std::floor(n(1));
                if (collision_sprite(*self) < 0)
                    return true;
                const auto blocked_by = [&](const Instance &instance) {
                    return instance.alive && !instance.deactivated && &instance != self.get() &&
                           value_number(instance.vars.at("solid")) != 0 && collision_sprite(instance) >= 0 &&
                           collides(*self, x, y, instance);
                };
                if (collect_spatial_candidates(bounds(*self, x, y))) {
                    for (const auto *instance : spatial_query_candidates)
                        if (blocked_by(*instance))
                            return false;
                } else {
                    for (const auto &instance : instances)
                        if (blocked_by(*instance))
                            return false;
                }
                return true;
            }
            if (name == "place_meeting" || name == "instance_place") {
                const int target = static_cast<int>(n(2));
                const double x = n(0), y = n(1);
                const auto collided_with = [&](Instance &instance) -> std::optional<Value> {
                    if (!instance.alive || (target != -2 && instance.deactivated) ||
                        &instance == self.get() || !matches(instance, target) ||
                        !collides(*self, x, y, instance))
                        return std::nullopt;
                    return name == "place_meeting" ? Value{true} : Value{std::int32_t{instance.id}};
                };
                if (target != -1 && target != -2 && collect_spatial_candidates(bounds(*self, x, y))) {
                    for (auto *instance : spatial_query_candidates)
                        if (auto result = collided_with(*instance))
                            return std::move(*result);
                } else {
                    for (const auto &instance : targets(target, self, other))
                        if (instance != self)
                            if (collides(*self, x, y, *instance))
                                return name == "place_meeting" ? Value{true}
                                                               : Value{std::int32_t{instance->id}};
                }
                return name == "place_meeting" ? Value{false} : Value{std::int32_t{-4}};
            }
            if (name == "move_bounce_all") {
                const bool advanced = n(0) != 0;
                double hspeed = value_number(self->vars.at("hspeed"));
                double vspeed = value_number(self->vars.at("vspeed"));
                bool repositioned = false;
                const auto own_sprite = collision_sprite(*self);
                if (own_sprite >= 0) {
                    const auto own_bounds = bounds(*self);
                    const auto previous_bounds = advanced
                                                     ? bounds(*self, value_number(self->vars.at("xprevious")),
                                                              value_number(self->vars.at("yprevious")))
                                                     : Bounds{};
                    const auto process_candidate = [&](Instance *candidate) {
                        if (!candidate->alive || candidate == self.get() ||
                            collision_sprite(*candidate) < 0 ||
                            !collides(*self, value_number(self->vars.at("x")),
                                      value_number(self->vars.at("y")), *candidate))
                            return;
                        const auto other_bounds = bounds(*candidate);
                        if (!own_bounds.valid || !other_bounds.valid)
                            return;
                        const double overlap_x = std::min(own_bounds.right, other_bounds.right) -
                                                 std::max(own_bounds.left, other_bounds.left);
                        const double overlap_y = std::min(own_bounds.bottom, other_bounds.bottom) -
                                                 std::max(own_bounds.top, other_bounds.top);
                        if (overlap_x <= 0 || overlap_y <= 0)
                            return;
                        const double center_x =
                            (own_bounds.left + own_bounds.right - other_bounds.left - other_bounds.right) *
                            0.5;
                        const double center_y =
                            (own_bounds.top + own_bounds.bottom - other_bounds.top - other_bounds.bottom) *
                            0.5;
                        if (overlap_x < overlap_y ||
                            (overlap_x == overlap_y && std::fabs(hspeed) >= std::fabs(vspeed)))
                            hspeed = center_x == 0 ? -hspeed : std::copysign(std::fabs(hspeed), center_x);
                        else
                            vspeed = center_y == 0 ? -vspeed : std::copysign(std::fabs(vspeed), center_y);
                        if (advanced) {
                            self->vars["x"] = value_number(self->vars.at("xprevious"));
                            self->vars["y"] = value_number(self->vars.at("yprevious"));
                            repositioned = true;
                        }
                    };
                    const auto *additional_bounds = advanced ? &previous_bounds : nullptr;
                    if (collect_spatial_candidates(own_bounds, additional_bounds)) {
                        for (auto *candidate : spatial_query_candidates)
                            process_candidate(candidate);
                    } else {
                        for (const auto &candidate : instances)
                            process_candidate(candidate.get());
                    }
                }
                if (repositioned) {
                    note_collision_geometry_change();
                    update_spatial_instance(*self);
                }
                self->vars["hspeed"] = hspeed;
                self->vars["vspeed"] = vspeed;
                self->vars["speed"] = std::hypot(hspeed, vspeed);
                if (hspeed != 0 || vspeed != 0)
                    self->vars["direction"] =
                        std::fmod(std::atan2(-vspeed, hspeed) * 180 / 3.141592653589793 + 360, 360);
                return std::monostate{};
            }
            if (name == "keyboard_clear") {
                const auto key = static_cast<int>(n(0));
                if (key >= 0 && key < 256) {
                    const auto k = static_cast<std::size_t>(key);
                    // GameMaker keeps a cleared held key ignored until the real key is
                    // released. Clearing only the per-frame state lets SDL's held state
                    // restore the key on the next tick, so menus can inherit movement
                    // or action input from before they opened.
                    if (physical_held[k])
                        cleared_until_release[k] = true;
                    held[k] = pressed[k] = released[k] = false;
                    previous_held[k] = false;
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
                    self->vars["direction"] =
                        std::fmod(std::atan2(-dy, dx) * 180 / 3.141592653589793 + 360, 360);
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
            if (name == "mp_grid_create") {
                if (args.size() < 6)
                    throw Error(ErrorKind::vm, "mp_grid_create requires six arguments");
                const int columns = number(args[2]), rows = number(args[3]);
                const double cell_width = n(4), cell_height = n(5);
                if (columns <= 0 || rows <= 0 || columns > 10000 || rows > 10000 ||
                    static_cast<std::uint64_t>(columns) * static_cast<std::uint64_t>(rows) > 1'000'000 ||
                    cell_width <= 0 || cell_height <= 0)
                    return std::int32_t{-1};
                MotionPlanningGrid grid;
                grid.x = n(0);
                grid.y = n(1);
                grid.columns = columns;
                grid.rows = rows;
                grid.cell_width = cell_width;
                grid.cell_height = cell_height;
                grid.blocked.resize(static_cast<std::size_t>(columns) * rows);
                const int id = next_mp_grid_id++;
                mp_grids.emplace(id, std::move(grid));
                return std::int32_t{id};
            }
            if (name == "mp_grid_draw") {
                const auto found = mp_grids.find(number(args[0]));
                if (found == mp_grids.end())
                    return false;
                const auto &grid = found->second;
                const double saved_alpha = draw_alpha;
                const unsigned saved_color = draw_color;
                current_canvas().alpha(saved_alpha * 0.35);
                for (int row = 0; row < grid.rows; ++row)
                    for (int column = 0; column < grid.columns; ++column) {
                        const auto index = static_cast<std::size_t>(row) * grid.columns + column;
                        current_canvas().color(grid.blocked[index] ? 0x0000ffU : 0x00ff00U);
                        assets.rectangle(current_canvas(), grid.x + column * grid.cell_width,
                                         grid.y + row * grid.cell_height,
                                         grid.x + (column + 1) * grid.cell_width,
                                         grid.y + (row + 1) * grid.cell_height, false);
                    }
                current_canvas().color(saved_color);
                current_canvas().alpha(saved_alpha);
                return true;
            }
            if (name == "mp_grid_destroy" || name == "mp_grid_clear_all" || name == "mp_grid_add_cell" ||
                name == "mp_grid_clear_cell" || name == "mp_grid_add_rectangle" ||
                name == "mp_grid_clear_rectangle" || name == "mp_grid_add_instances") {
                const int id = number(args[0]);
                auto found = mp_grids.find(id);
                if (name == "mp_grid_destroy") {
                    if (found != mp_grids.end())
                        mp_grids.erase(found);
                    return std::monostate{};
                }
                if (found == mp_grids.end())
                    return name == "mp_grid_add_instances" ? Value{std::int32_t{0}} : Value{std::monostate{}};
                auto &grid = found->second;
                if (name == "mp_grid_clear_all") {
                    std::fill(grid.blocked.begin(), grid.blocked.end(), 0);
                    return std::monostate{};
                }
                auto set_cell = [&](int column, int row, bool blocked) {
                    if (column < 0 || row < 0 || column >= grid.columns || row >= grid.rows)
                        return;
                    grid.blocked[static_cast<std::size_t>(row) * grid.columns + column] = blocked ? 1 : 0;
                };
                auto cell_at = [&](double x, double y) {
                    return std::pair{static_cast<int>(std::floor((x - grid.x) / grid.cell_width)),
                                     static_cast<int>(std::floor((y - grid.y) / grid.cell_height))};
                };
                if (name == "mp_grid_add_cell" || name == "mp_grid_clear_cell") {
                    const auto [column, row] = cell_at(n(1), n(2));
                    set_cell(column, row, name == "mp_grid_add_cell");
                    return std::monostate{};
                }
                auto set_rectangle = [&](double x1, double y1, double x2, double y2, bool blocked) {
                    const auto [left, top] = cell_at(std::min(x1, x2), std::min(y1, y2));
                    const auto [right, bottom] = cell_at(std::max(x1, x2), std::max(y1, y2));
                    for (int row = top; row <= bottom; ++row)
                        for (int column = left; column <= right; ++column)
                            set_cell(column, row, blocked);
                };
                if (name == "mp_grid_add_rectangle" || name == "mp_grid_clear_rectangle") {
                    set_rectangle(n(1), n(2), n(3), n(4), name == "mp_grid_add_rectangle");
                    return std::monostate{};
                }
                const int object = number(args[1]);
                visit_target_candidates(object, [&](const InstancePtr &instance) {
                    if (!instance->alive || instance->deactivated)
                        return false;
                    const auto box = bounds(*instance, value_number(instance->vars.at("x")),
                                            value_number(instance->vars.at("y")));
                    if (!box.valid)
                        return false;
                    set_rectangle(box.left, box.top, box.right, box.bottom, true);
                    return false;
                });
                return std::monostate{};
            }
            if (name == "mp_grid_path") {
                const int grid_id = number(args[0]), path_id = number(args[1]);
                const auto grid_it = mp_grids.find(grid_id);
                if (grid_it == mp_grids.end() || path_id < 0 ||
                    static_cast<std::size_t>(path_id) >= paths.size() ||
                    !paths[static_cast<std::size_t>(path_id)].valid)
                    return false;
                const auto &grid = grid_it->second;
                auto &path = paths[static_cast<std::size_t>(path_id)];
                path.control_points.clear();
                path.points.clear();
                path.length = 0;
                const auto [start_x, start_y] = [&] {
                    const auto column = static_cast<int>(std::floor((n(2) - grid.x) / grid.cell_width));
                    const auto row = static_cast<int>(std::floor((n(3) - grid.y) / grid.cell_height));
                    return std::pair{column, row};
                }();
                const auto [goal_x, goal_y] = [&] {
                    const auto column = static_cast<int>(std::floor((n(4) - grid.x) / grid.cell_width));
                    const auto row = static_cast<int>(std::floor((n(5) - grid.y) / grid.cell_height));
                    return std::pair{column, row};
                }();
                auto cell_index = [&](int column, int row) {
                    return static_cast<std::size_t>(row) * grid.columns + column;
                };
                auto valid = [&](int column, int row) {
                    return column >= 0 && row >= 0 && column < grid.columns && row < grid.rows;
                };
                if (!valid(start_x, start_y) || !valid(goal_x, goal_y))
                    return false;
                const auto start = cell_index(start_x, start_y), goal = cell_index(goal_x, goal_y);
                const bool diagonal = n(6) != 0;
                const auto cell_count = grid.blocked.size();
                const double infinity = std::numeric_limits<double>::infinity();
                std::vector<double> costs(cell_count, infinity);
                std::vector<std::size_t> previous(cell_count, cell_count);
                using SearchNode = std::pair<double, std::size_t>;
                std::priority_queue<SearchNode, std::vector<SearchNode>, std::greater<>> frontier;
                auto heuristic = [&](int x, int y) {
                    const int dx = std::abs(goal_x - x), dy = std::abs(goal_y - y);
                    return diagonal ? static_cast<double>(std::max(dx, dy)) +
                                          (1.4142135623730951 - 1.0) * std::min(dx, dy)
                                    : static_cast<double>(dx + dy);
                };
                costs[start] = 0;
                frontier.emplace(heuristic(start_x, start_y), start);
                static constexpr std::array<std::pair<int, int>, 8> neighbors = {
                    std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1},  std::pair{0, -1},
                    std::pair{1, 1}, std::pair{1, -1}, std::pair{-1, 1}, std::pair{-1, -1}};
                while (!frontier.empty()) {
                    const auto current = frontier.top().second;
                    frontier.pop();
                    if (current == goal)
                        break;
                    const int column = static_cast<int>(current % grid.columns);
                    const int row = static_cast<int>(current / grid.columns);
                    for (std::size_t direction = 0; direction < (diagonal ? 8U : 4U); ++direction) {
                        const auto [dx, dy] = neighbors[direction];
                        const int next_x = column + dx, next_y = row + dy;
                        if (!valid(next_x, next_y))
                            continue;
                        const auto next = cell_index(next_x, next_y);
                        if (grid.blocked[next] && next != goal)
                            continue;
                        if (dx && dy &&
                            (grid.blocked[cell_index(column + dx, row)] ||
                             grid.blocked[cell_index(column, row + dy)]))
                            continue;
                        const double candidate = costs[current] + (dx && dy ? 1.4142135623730951 : 1.0);
                        if (candidate >= costs[next])
                            continue;
                        costs[next] = candidate;
                        previous[next] = current;
                        frontier.emplace(candidate + heuristic(next_x, next_y), next);
                    }
                }
                if (start != goal && previous[goal] == cell_count)
                    return false;
                std::vector<std::size_t> route{goal};
                for (auto cursor = goal; cursor != start;) {
                    cursor = previous[cursor];
                    if (cursor == cell_count)
                        return false;
                    route.push_back(cursor);
                }
                std::reverse(route.begin(), route.end());
                path.smooth = false;
                path.closed = false;
                path.control_points.reserve(route.size());
                for (const auto index : route) {
                    const int column = static_cast<int>(index % grid.columns);
                    const int row = static_cast<int>(index / grid.columns);
                    path.control_points.push_back({grid.x + (column + 0.5) * grid.cell_width,
                                                   grid.y + (row + 0.5) * grid.cell_height, 100, 0});
                }
                rebuild_path(path);
                return true;
            }
            return std::nullopt;
        };
        if (auto handled = dispatch_builtin(8, dispatch_builtins_8))
            return std::move(*handled);
        auto dispatch_builtins_9 = [&]() REGML_NOINLINE_LAMBDA -> std::optional<Value> {
            if (name == "path_add") {
                Path path;
                path.closed = true;
                paths.push_back(std::move(path));
                return static_cast<std::int32_t>(paths.size() - 1);
            }
            if (name == "path_exists" || name == "path_delete" || name == "path_clear_points" ||
                name == "path_add_point" || name == "path_set_closed" || name == "path_set_kind" ||
                name == "path_set_precision" || name == "path_get_x" || name == "path_get_y" ||
                name == "path_get_speed" || name == "path_get_length" || name == "path_get_number" ||
                name == "path_get_point_x" || name == "path_get_point_y") {
                const int id = static_cast<int>(n(0));
                const bool exists = id >= 0 && static_cast<std::size_t>(id) < paths.size() &&
                                    paths[static_cast<std::size_t>(id)].valid;
                if (name == "path_exists")
                    return exists;
                if (!exists)
                    throw Error(ErrorKind::vm, "invalid path ID");
                auto &path = paths[static_cast<std::size_t>(id)];
                if (name == "path_delete") {
                    path.valid = false;
                    path.control_points.clear();
                    path.points.clear();
                    path.length = 0;
                    for (const auto &instance : instances)
                        if (number(instance->vars.at("path_index")) == id)
                            instance->vars["path_index"] = std::int32_t{-1};
                    return std::monostate{};
                }
                if (name == "path_clear_points") {
                    path.control_points.clear();
                    path.points.clear();
                    path.length = 0;
                    return std::monostate{};
                }
                if (name == "path_add_point") {
                    if (args.size() < 3)
                        throw Error(ErrorKind::vm, "path_add_point requires x and y coordinates");
                    if (path.control_points.size() >= 65536)
                        throw Error(ErrorKind::vm, "path point limit exceeded");
                    path.control_points.push_back({n(1), n(2), args.size() > 3 ? n(3) : 100.0, 0});
                    rebuild_path(path);
                    return std::monostate{};
                }
                if (name == "path_set_closed") {
                    path.closed = n(1) != 0;
                    rebuild_path(path);
                    return std::monostate{};
                }
                if (name == "path_set_kind") {
                    path.smooth = n(1) != 0;
                    rebuild_path(path);
                    return std::monostate{};
                }
                if (name == "path_set_precision") {
                    path.precision = static_cast<unsigned>(std::clamp(number(n(1)), 1, 8));
                    rebuild_path(path);
                    return std::monostate{};
                }
                if (name == "path_get_length")
                    return path.length;
                if (name == "path_get_number")
                    return static_cast<std::int32_t>(path.control_points.size());
                const int point = static_cast<int>(n(1));
                if (point < 0 || static_cast<std::size_t>(point) >= path.control_points.size())
                    return 0.0;
                const auto &value = path.control_points[static_cast<std::size_t>(point)];
                if (name == "path_get_x" || name == "path_get_point_x")
                    return value.x;
                if (name == "path_get_y" || name == "path_get_point_y")
                    return value.y;
                return value.speed;
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
            if (name == "motion_set" || name == "motion_add") {
                const double radians = n(0) * 3.141592653589793 / 180.0;
                double hspeed = n(1) * std::cos(radians);
                double vspeed = -n(1) * std::sin(radians);
                if (name == "motion_add") {
                    hspeed += value_number(self->vars.at("hspeed"));
                    vspeed += value_number(self->vars.at("vspeed"));
                }
                self->vars["hspeed"] = hspeed;
                self->vars["vspeed"] = vspeed;
                self->vars["speed"] = std::hypot(hspeed, vspeed);
                if (hspeed != 0 || vspeed != 0)
                    self->vars["direction"] =
                        std::fmod(std::atan2(-vspeed, hspeed) * 180 / 3.141592653589793 + 360, 360);
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
            if (name == "point_distance_3d")
                return std::sqrt((n(3) - n(0)) * (n(3) - n(0)) + (n(4) - n(1)) * (n(4) - n(1)) +
                                 (n(5) - n(2)) * (n(5) - n(2)));
            if (name == "dot_product")
                return n(0) * n(2) + n(1) * n(3);
            if (name == "dot_product_3d")
                return n(0) * n(3) + n(1) * n(4) + n(2) * n(5);
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
            if (name == "arctan2")
                return std::atan2(n(0), n(1));
            if (name == "tan")
                return std::tan(n(0));
            if (name == "log2")
                return std::log2(n(0));
            if (name == "make_color_rgb" || name == "make_colour_rgb") {
                const auto channel = [&](std::size_t i) {
                    return static_cast<unsigned>(std::clamp(n(i), 0.0, 255.0));
                };
                return std::int32_t{static_cast<int>((channel(2) << 16) | (channel(1) << 8) | channel(0))};
            }
            if (name == "make_color_hsv" || name == "make_colour_hsv") {
                const double hue = std::fmod(std::fmod(n(0), 256.0) + 256.0, 256.0) * 6 / 256;
                const double saturation = std::clamp(n(1), 0.0, 255.0) / 255;
                const double value = std::clamp(n(2), 0.0, 255.0) / 255;
                const double chroma = value * saturation,
                             x = chroma * (1 - std::fabs(std::fmod(hue, 2.0) - 1));
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
            if (name == "merge_color" || name == "merge_colour") {
                const auto first = static_cast<unsigned>(n(0)), second = static_cast<unsigned>(n(1));
                const double t = std::clamp(n(2), 0.0, 1.0);
                unsigned result = 0;
                for (unsigned shift : {0U, 8U, 16U}) {
                    const double a = static_cast<double>((first >> shift) & 255), b = (second >> shift) & 255;
                    result |= static_cast<unsigned>(std::lround(a + (b - a) * t)) << shift;
                }
                return std::int32_t{static_cast<int>(result)};
            }
            if (name == "color_get_red" || name == "colour_get_red" || name == "color_get_green" ||
                name == "colour_get_green" || name == "color_get_blue" || name == "colour_get_blue") {
                const auto color = static_cast<std::uint32_t>(number(args[0]));
                const unsigned shift = name.ends_with("red") ? 0U : name.ends_with("green") ? 8U : 16U;
                return std::int32_t{static_cast<std::int32_t>((color >> shift) & 0xffU)};
            }
            if (name == "color_get_hue" || name == "color_get_saturation" || name == "color_get_value") {
                const auto color = static_cast<std::uint32_t>(number(args[0]));
                const double red = static_cast<double>(color & 255U) / 255.0;
                const double green = static_cast<double>((color >> 8) & 255U) / 255.0;
                const double blue = static_cast<double>((color >> 16) & 255U) / 255.0;
                const double maximum = std::max({red, green, blue}), minimum = std::min({red, green, blue});
                const double delta = maximum - minimum;
                if (name == "color_get_value")
                    return maximum * 255.0;
                if (name == "color_get_saturation")
                    return maximum == 0 ? 0.0 : delta / maximum * 255.0;
                double hue = 0;
                if (delta != 0) {
                    if (maximum == red)
                        hue = 60.0 * std::fmod((green - blue) / delta, 6.0);
                    else if (maximum == green)
                        hue = 60.0 * ((blue - red) / delta + 2.0);
                    else
                        hue = 60.0 * ((red - green) / delta + 4.0);
                    if (hue < 0)
                        hue += 360.0;
                }
                return hue * 256.0 / 360.0;
            }
            if (name == "point_in_rectangle") {
                const double left = std::min(n(2), n(4)), right = std::max(n(2), n(4));
                const double top = std::min(n(3), n(5)), bottom = std::max(n(3), n(5));
                return n(0) >= left && n(0) <= right && n(1) >= top && n(1) <= bottom;
            }
            if (name == "rectangle_in_rectangle") {
                const double sl = std::min(n(0), n(2)), sr = std::max(n(0), n(2));
                const double st = std::min(n(1), n(3)), sb = std::max(n(1), n(3));
                const double dl = std::min(n(4), n(6)), dr = std::max(n(4), n(6));
                const double dt = std::min(n(5), n(7)), db = std::max(n(5), n(7));
                if (sr < dl || sl > dr || sb < dt || st > db)
                    return std::int32_t{0};
                return std::int32_t{sl >= dl && sr <= dr && st >= dt && sb <= db ? 1 : 2};
            }
            if (name == "instance_nearest") {
                const double x = n(0), y = n(1);
                InstancePtr nearest;
                double nearest_distance = std::numeric_limits<double>::infinity();
                for (const auto &instance : targets(static_cast<int>(n(2)), self, other)) {
                    const double dx = value_number(instance->vars.at("x")) - x;
                    const double dy = value_number(instance->vars.at("y")) - y;
                    const double distance = dx * dx + dy * dy;
                    if (distance < nearest_distance) {
                        nearest = instance;
                        nearest_distance = distance;
                    }
                }
                return nearest ? Value{std::int32_t{nearest->id}} : Value{std::int32_t{-4}};
            }
            if (name == "position_meeting") {
                const double x = n(0), y = n(1);
                for (const auto &instance : targets(static_cast<int>(n(2)), self, other)) {
                    const auto b = bounds(*instance);
                    if (!b.valid || x < b.left || x >= b.right || y < b.top || y >= b.bottom)
                        continue;
                    const auto mask = collision_mask(*instance, value_number(instance->vars.at("x")),
                                                     value_number(instance->vars.at("y")));
                    if (!mask.sprite || mask.pixels.empty() || mask_contains(mask, x, y))
                        return true;
                }
                return false;
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
            if (name == "instance_place_list") {
                if (args.size() < 5 || !self)
                    throw Error(ErrorKind::vm,
                                "instance_place_list requires x, y, object, list, and ordered");
                const double x = n(0), y = n(1);
                auto &list = lists.at(static_cast<int>(n(3)));
                std::vector<InstancePtr> hits;
                for (const auto &instance : targets(static_cast<int>(n(2)), self, other))
                    if (instance != self && collides(*self, x, y, *instance))
                        hits.push_back(instance);
                if (n(4) != 0) {
                    std::stable_sort(hits.begin(), hits.end(),
                                     [&](const InstancePtr &left, const InstancePtr &right) {
                                         const double lx = value_number(left->vars.at("x")) - x;
                                         const double ly = value_number(left->vars.at("y")) - y;
                                         const double rx = value_number(right->vars.at("x")) - x;
                                         const double ry = value_number(right->vars.at("y")) - y;
                                         return lx * lx + ly * ly < rx * rx + ry * ry;
                                     });
                }
                for (const auto &instance : hits)
                    list.emplace_back(std::int32_t{instance->id});
                return static_cast<std::int32_t>(hits.size());
            }
            if (name == "collision_rectangle_list") {
                if (args.size() < 9)
                    throw Error(
                        ErrorKind::vm,
                        "collision_rectangle_list requires rectangle, object, flags, list, and ordering");
                const double x1 = std::min(n(0), n(2)), x2 = std::max(n(0), n(2));
                const double y1 = std::min(n(1), n(3)), y2 = std::max(n(1), n(3));
                const int object = static_cast<int>(n(4));
                const bool precise = n(5) != 0, not_me = n(6) != 0, ordered = n(8) != 0;
                auto &list = lists.at(static_cast<int>(n(7)));
                std::vector<InstancePtr> hits;
                for (const auto &instance : targets(object, self, other)) {
                    if (not_me && instance == self)
                        continue;
                    const auto b = bounds(*instance);
                    if (!b.valid || x1 > b.right || x2 < b.left || y1 > b.bottom || y2 < b.top)
                        continue;
                    bool hit = true;
                    const int sprite_id = collision_sprite(*instance);
                    if (precise && sprite_id >= 0 && assets.sprite(sprite_id).precise) {
                        const auto mask = collision_mask(*instance, value_number(instance->vars.at("x")),
                                                         value_number(instance->vars.at("y")));
                        hit = false;
                        const int left = static_cast<int>(std::floor(std::max(x1, b.left)));
                        const int right = static_cast<int>(std::ceil(std::min(x2, b.right)));
                        const int top = static_cast<int>(std::floor(std::max(y1, b.top)));
                        const int bottom = static_cast<int>(std::ceil(std::min(y2, b.bottom)));
                        for (int py = top; py < bottom && !hit; ++py)
                            for (int px = left; px < right; ++px)
                                if (mask_contains(mask, px + 0.5, py + 0.5)) {
                                    hit = true;
                                    break;
                                }
                    }
                    if (hit)
                        hits.push_back(instance);
                }
                if (ordered) {
                    const double cx = (x1 + x2) * 0.5, cy = (y1 + y2) * 0.5;
                    std::stable_sort(hits.begin(), hits.end(),
                                     [&](const InstancePtr &left, const InstancePtr &right) {
                                         const double lx = value_number(left->vars.at("x")) - cx;
                                         const double ly = value_number(left->vars.at("y")) - cy;
                                         const double rx = value_number(right->vars.at("x")) - cx;
                                         const double ry = value_number(right->vars.at("y")) - cy;
                                         return lx * lx + ly * ly < rx * rx + ry * ry;
                                     });
                }
                for (const auto &instance : hits)
                    list.emplace_back(std::int32_t{instance->id});
                return static_cast<std::int32_t>(hits.size());
            }
            if (name == "collision_line_list" || name == "collision_circle_list") {
                if (args.size() < (name == "collision_line_list" ? 9U : 8U))
                    throw Error(ErrorKind::vm, std::string(name) + " received too few arguments");
                const bool line = name == "collision_line_list";
                const double x1 = n(0), y1 = n(1);
                const double x2 = line ? n(2) : x1, y2 = line ? n(3) : y1;
                const double radius = line ? 0 : std::max(0.0, n(2));
                const int object = number(args[line ? 4 : 3]);
                const bool not_me = n(line ? 6 : 5) != 0;
                const int list_id = number(args[line ? 7 : 6]);
                const bool ordered = n(line ? 8 : 7) != 0;
                auto &list = lists.at(list_id);
                std::vector<std::pair<InstancePtr, double>> hits;
                for (const auto &instance : targets(object, self, other)) {
                    if (not_me && instance == self)
                        continue;
                    const auto box = bounds(*instance);
                    if (!box.valid)
                        continue;
                    bool hit = false;
                    double order = 0;
                    if (line) {
                        double low = 0, high = 1;
                        auto clip = [&](double origin, double delta, double minimum, double maximum) {
                            if (delta == 0)
                                return origin >= minimum && origin < maximum;
                            double enter = (minimum - origin) / delta, leave = (maximum - origin) / delta;
                            if (enter > leave)
                                std::swap(enter, leave);
                            low = std::max(low, enter);
                            high = std::min(high, leave);
                            return low <= high;
                        };
                        hit =
                            clip(x1, x2 - x1, box.left, box.right) && clip(y1, y2 - y1, box.top, box.bottom);
                        order = low;
                    } else {
                        const double closest_x = std::clamp(x1, box.left, box.right);
                        const double closest_y = std::clamp(y1, box.top, box.bottom);
                        const double dx = closest_x - x1, dy = closest_y - y1;
                        hit = dx * dx + dy * dy <= radius * radius;
                        order = (value_number(instance->vars.at("x")) - x1) *
                                    (value_number(instance->vars.at("x")) - x1) +
                                (value_number(instance->vars.at("y")) - y1) *
                                    (value_number(instance->vars.at("y")) - y1);
                    }
                    if (hit)
                        hits.emplace_back(instance, order);
                }
                if (ordered)
                    std::stable_sort(hits.begin(), hits.end(), [](const auto &left, const auto &right) {
                        return left.second < right.second;
                    });
                for (const auto &[instance, distance] : hits) {
                    (void)distance;
                    list.emplace_back(std::int32_t{instance->id});
                }
                return static_cast<std::int32_t>(hits.size());
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
                            std::hypot(
                                std::max({target.left - source.right, source.left - target.right, 0.0}),
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
            if (name == "collision_ellipse") {
                const double left = std::min(n(0), n(2)), right = std::max(n(0), n(2));
                const double top = std::min(n(1), n(3)), bottom = std::max(n(1), n(3));
                const double center_x = (left + right) * 0.5, center_y = (top + bottom) * 0.5;
                const double radius_x = (right - left) * 0.5, radius_y = (bottom - top) * 0.5;
                if (radius_x <= 0 || radius_y <= 0)
                    return std::int32_t{-4};
                const bool not_me = n(6) != 0;
                for (const auto &instance : targets(number(args[4]), self, other)) {
                    if (not_me && instance == self)
                        continue;
                    const auto box = bounds(*instance);
                    if (!box.valid)
                        continue;
                    const double closest_x = std::clamp(center_x, box.left, box.right);
                    const double closest_y = std::clamp(center_y, box.top, box.bottom);
                    const double x = (closest_x - center_x) / radius_x;
                    const double y = (closest_y - center_y) / radius_y;
                    if (x * x + y * y <= 1.0)
                        return std::int32_t{instance->id};
                }
                return std::int32_t{-4};
            }
            if (name == "move_snap") {
                const double sx = n(0), sy = n(1);
                bool repositioned = false;
                if (sx > 0) {
                    self->vars["x"] = std::round(value_number(self->vars.at("x")) / sx) * sx;
                    note_collision_geometry_change();
                    repositioned = true;
                }
                if (sy > 0) {
                    self->vars["y"] = std::round(value_number(self->vars.at("y")) / sy) * sy;
                    note_collision_geometry_change();
                    repositioned = true;
                }
                if (repositioned)
                    update_spatial_instance(*self);
                return std::monostate{};
            }
            return std::nullopt;
        };
        if (auto handled = dispatch_builtin(9, dispatch_builtins_9))
            return std::move(*handled);
        auto dispatch_builtins_10 = [&]() REGML_NOINLINE_LAMBDA -> std::optional<Value> {
            if (name == "instance_exists") {
                if (args.empty())
                    throw Error(ErrorKind::vm, "instance_exists requires an instance or object id");
                if (const auto *structure = std::get_if<StructPtr>(&args[0])) {
                    if (!*structure)
                        return false;
                    const auto found = gmrt_instance_values.find(structure->get());
                    return found != gmrt_instance_values.end() && found->second && found->second->alive;
                }
                if (std::holds_alternative<CallablePtr>(args[0]) || std::holds_alternative<ArrayPtr>(args[0]))
                    return false;
                return first_target(static_cast<int>(n(0)), self, other) != nullptr;
            }
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
                event(self, type, subtype, -1, other);
                return std::monostate{};
            }
            if (name == "event_user") {
                event(self, 7, 10 + static_cast<int>(n(0)), -1, other);
                return std::monostate{};
            }
            if (name == "action_move_to") {
                self->vars["x"] = n(0);
                self->vars["y"] = n(1);
                note_collision_geometry_change();
                update_spatial_instance(*self);
                return std::monostate{};
            }
            if (name == "instance_number") {
                const int target = static_cast<int>(n(0));
                if (target == -1 || target == -2)
                    return first_target(target, self, other) ? 1.0 : 0.0;
                std::size_t count = 0;
                visit_target_candidates(target, [&](const InstancePtr &instance) {
                    if (instance->alive && !instance->deactivated)
                        ++count;
                    return false;
                });
                return static_cast<double>(count);
            }
            if (name == "instance_find") {
                const int target = static_cast<int>(n(0));
                auto index = static_cast<std::size_t>(n(1));
                if (target == -1 || target == -2) {
                    const auto *found = index == 0 ? first_target(target, self, other) : nullptr;
                    return std::int32_t{found ? found->id : -4};
                }
                int found = -4;
                visit_target_candidates(target, [&](const InstancePtr &instance) {
                    if (!instance->alive || instance->deactivated)
                        return false;
                    if (index == 0) {
                        found = instance->id;
                        return true;
                    }
                    --index;
                    return false;
                });
                return std::int32_t{found};
            }
            if (name == "instance_id_get") {
                int position = number(args[0]);
                if (position < 0)
                    return std::int32_t{-4};
                for (const auto &instance : instances)
                    if (instance->alive && !instance->deactivated) {
                        if (position-- == 0)
                            return std::int32_t{instance->id};
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
            if (name == "alarm_set") {
                const int alarm = number(args[0]);
                if (alarm < 0 || alarm >= 12)
                    throw Error(ErrorKind::vm, "alarm_set index out of range");
                self->vars["alarm[" + std::to_string(alarm) + "]"] = n(1);
                return std::monostate{};
            }
            if (name == "instance_create_layer") {
                Placement p;
                p.x = n(0);
                p.y = n(1);
                p.layer = layer_id_argument(2);
                if (!find_layer(p.layer))
                    throw Error(ErrorKind::vm, "instance_create_layer received an invalid layer");
                p.object = number(args[3]);
                p.id = next_id++;
                const auto i = make(p);
                if (args.size() > 4) {
                    if (const auto *initial = std::get_if<StructPtr>(&args[4]); initial && *initial) {
                        for (const auto &[name, value] : (*initial)->variables)
                            i->vars[std::string(name)] = value;
                        i->manual_depth = (*initial)->variables.contains("depth");
                    } else if (!std::holds_alternative<std::monostate>(args[4]))
                        throw Error(ErrorKind::vm, "instance_create_layer initial values must be a struct");
                }
                update_spatial_instance(*i);
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
                update_spatial_instance(*i);
                event(i, 14, 0);
                event(i, 0, 0);
                return std::int32_t{i->id};
            }
            if (name == "instance_copy") {
                Placement p;
                p.object = self->object;
                p.id = next_id++;
                p.x = value_number(self->vars.at("x"));
                p.y = value_number(self->vars.at("y"));
                p.layer = number(self->vars.at("layer"));
                const auto copy = make(p);
                copy->vars = self->vars;
                copy->manual_depth = self->manual_depth;
                copy->vars["id"] = std::int32_t{copy->id};
                copy->vars["object_index"] = std::int32_t{copy->object};
                update_spatial_instance(*copy);
                if (args.empty() || n(0) != 0) {
                    event(copy, 14, 0);
                    event(copy, 0, 0);
                }
                return std::int32_t{copy->id};
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
                note_collision_geometry_change();
                object_instance_index_dirty = true;
                if (perform) {
                    const auto &replacement = objects[static_cast<std::size_t>(object)];
                    self->vars["sprite_index"] = std::int32_t{replacement.sprite};
                    self->vars["mask_index"] = std::int32_t{replacement.mask};
                    self->vars["visible"] = std::int32_t{replacement.visible};
                    self->vars["depth"] = std::int32_t{replacement.depth};
                    self->manual_depth = false;
                    self->vars["solid"] = std::int32_t{replacement.solid};
                    self->vars["image_index"] = 0.0;
                    self->vars["image_speed"] = 1.0;
                    update_spatial_instance(*self);
                    event(self, 14, 0);
                    event(self, 0, 0);
                }
                return std::monostate{};
            }
            if (name == "instance_destroy" || name == "action_kill_object") {
                const bool execute_destroy_event =
                    name == "action_kill_object" || args.size() < 2 || n(1) != 0;
                std::vector<InstancePtr> targets_to_destroy;
                if (name == "action_kill_object" || args.empty()) {
                    if (self)
                        targets_to_destroy.push_back(self);
                } else {
                    const int target = static_cast<int>(n(0));
                    if (target == -1) {
                        if (self)
                            targets_to_destroy.push_back(self);
                    } else if (target == -2) {
                        if (other)
                            targets_to_destroy.push_back(other);
                    } else if (target == -3) {
                        for (const auto &instance : instances)
                            if (instance->alive && !instance->deactivated)
                                targets_to_destroy.push_back(instance);
                    } else {
                        visit_target_candidates(target, [&](const InstancePtr &instance) {
                            if (instance->alive && !instance->deactivated)
                                targets_to_destroy.push_back(instance);
                            return false;
                        });
                    }
                }
                for (const auto &instance : targets_to_destroy) {
                    if (!instance->alive || instance->deactivated || instance->destroying)
                        continue;
                    instance->destroying = true;
                    if (execute_destroy_event)
                        event(instance, 1, 0);
                    event(instance, 12, 0);
                    instance->alive = false;
                }
                return std::monostate{};
            }
            if (name == "instance_deactivate_layer" || name == "instance_activate_layer") {
                const int layer_id = layer_id_argument(0);
                const bool deactivate = name == "instance_deactivate_layer";
                for (const auto &instance : instances)
                    if (instance->alive && number(instance->vars.at("layer")) == layer_id)
                        instance->deactivated = deactivate;
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
                visit_target_candidates(object, [&](const InstancePtr &instance) {
                    if (instance->alive)
                        instance->deactivated = false;
                    return false;
                });
                return std::monostate{};
            }
            if (name == "instance_deactivate_object") {
                const int object = static_cast<int>(n(0));
                visit_target_candidates(object, [&](const InstancePtr &instance) {
                    if (instance->alive)
                        instance->deactivated = true;
                    return false;
                });
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
            return std::nullopt;
        };
        if (auto handled = dispatch_builtin(10, dispatch_builtins_10))
            return std::move(*handled);
        auto dispatch_builtins_11 = [&]() REGML_NOINLINE_LAMBDA -> std::optional<Value> {
            if (name == "keyboard_check" || name == "keyboard_check_direct" ||
                name == "keyboard_check_pressed" || name == "keyboard_check_released") {
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
            if (name == "gamepad_is_supported")
                return true;
            if (name == "gamepad_get_device_count")
                return std::int32_t{gamepad.connected ? 1 : 0};
            if (name == "gamepad_axis_count")
                return gamepad.connected && n(0) == 0 ? std::int32_t{4} : std::int32_t{0};
            if (name == "gamepad_button_count")
                return gamepad.connected && n(0) == 0 ? std::int32_t{16} : std::int32_t{0};
            if (name == "gamepad_set_axis_deadzone") {
                if (n(0) == 0)
                    gamepad_axis_deadzone = std::clamp(n(1), 0.0, 1.0);
                return std::monostate{};
            }
            if (name == "gamepad_get_axis_deadzone")
                return n(0) == 0 ? gamepad_axis_deadzone : 0.0;
            if (name == "gamepad_get_description")
                return std::string(gamepad.connected && n(0) == 0 ? "PlayStation 4 controller" : "");
            if (name == "gamepad_get_guid")
                return std::string{"none"};
            if (name == "gamepad_test_mapping")
                return gamepad.connected && n(0) == 0;
            if (name == "gamepad_is_connected")
                return gamepad.connected && n(0) == 0;
            if (name == "joystick_exists")
                return false;
            if (name == "gamepad_button_check" || name == "gamepad_button_check_pressed" ||
                name == "gamepad_button_check_released") {
                const int button = static_cast<int>(n(1)) - 32769;
                if (!gamepad.connected || n(0) != 0 || button < 0 || button >= 16)
                    return false;
                return name == "gamepad_button_check_pressed"    ? gamepad.pressed[button]
                       : name == "gamepad_button_check_released" ? gamepad_released[button]
                                                                 : gamepad.buttons[button];
            }
            if (name == "gamepad_button_value") {
                const int raw_button = number(args[1]);
                const int button = raw_button >= 32769 ? raw_button - 32769 : raw_button;
                return gamepad.connected && number(args[0]) == 0 && button >= 0 && button < 16
                           ? (gamepad.buttons[static_cast<std::size_t>(button)] ? 1.0 : 0.0)
                           : 0.0;
            }
            if (name == "gamepad_axis_value") {
                const int axis = static_cast<int>(n(1)) - 32785;
                if (!gamepad.connected || n(0) != 0 || axis < 0 || axis >= 4)
                    return 0.0;
                const double value = std::clamp(gamepad.axes[axis], -1.0, 1.0);
                const double magnitude = std::abs(value);
                if (magnitude <= gamepad_axis_deadzone || gamepad_axis_deadzone >= 1.0)
                    return 0.0;
                return std::copysign((magnitude - gamepad_axis_deadzone) / (1.0 - gamepad_axis_deadzone),
                                     value);
            }
            if (name == "joystick_check_button" || name == "joystick_buttons" || name == "joystick_has_pov")
                return false;
            if (name == "joystick_pov" || name == "joystick_direction" || name == "joystick_xpos" ||
                name == "joystick_ypos")
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
            if (name == "vertex_format_add_textcoord" || name == "vertex_format_add_texcoord") {
                if (vertex_format_building)
                    vertex_format_attributes.push_back(5);
                return std::monostate{};
            }
            if (name == "vertex_format_end") {
                vertex_format_building = false;
                const int format = next_vertex_format++;
                vertex_formats.emplace(format, std::move(vertex_format_attributes));
                vertex_format_attributes.clear();
                return std::int32_t{format};
            }
            if (name == "vertex_create_buffer") {
                const int buffer = next_vertex_buffer++;
                vertex_buffers.try_emplace(buffer);
                return std::int32_t{buffer};
            }
            if (name == "vertex_begin") {
                const int buffer_id = number(args[0]), format_id = number(args[1]);
                const auto buffer = vertex_buffers.find(buffer_id);
                if (buffer == vertex_buffers.end())
                    throw Error(ErrorKind::vm, "vertex_begin received an invalid buffer");
                auto &target = buffer->second;
                target.vertices.clear();
                target.format = format_id;
                target.next_attribute = 0;
                target.current = {};
                target.building = true;
                if (const auto format = vertex_formats.find(format_id); format != vertex_formats.end())
                    target.attributes = format->second;
                else if (format_id == 0)
                    target.attributes = {2, 4, 5};
                else
                    throw Error(ErrorKind::vm, "vertex_begin received an invalid format");
                active_vertex_buffer = buffer_id;
                return std::monostate{};
            }
            if (name == "vertex_position" || name == "vertex_position_3d" || name == "vertex_normal" ||
                name == "vertex_colour" || name == "vertex_color" || name == "vertex_texcoord") {
                const int buffer_id = number(args[0]);
                const auto buffer = vertex_buffers.find(buffer_id);
                if (buffer == vertex_buffers.end() || !buffer->second.building)
                    throw Error(ErrorKind::vm, std::string(name) + " called outside vertex_begin");
                auto &target = buffer->second;
                if (name == "vertex_position") {
                    target.current.x = n(1);
                    target.current.y = n(2);
                } else if (name == "vertex_position_3d") {
                    target.current.x = n(1);
                    target.current.y = n(2);
                    target.current.z = n(3);
                } else if (name == "vertex_normal") {
                    target.current.nx = n(1);
                    target.current.ny = n(2);
                    target.current.nz = n(3);
                } else if (name == "vertex_colour" || name == "vertex_color") {
                    target.current.color = static_cast<unsigned>(number(args[1])) & 0xffffffU;
                    target.current.alpha = args.size() > 2 ? std::clamp(n(2), 0.0, 1.0) : 1.0;
                } else {
                    target.current.u = n(1);
                    target.current.v = n(2);
                }
                if (++target.next_attribute >= std::max<std::size_t>(target.attributes.size(), 1)) {
                    if (target.vertices.size() >= 1'000'000)
                        throw Error(ErrorKind::vm, "vertex buffer vertex limit exceeded");
                    target.vertices.push_back(target.current);
                    target.current = {};
                    target.next_attribute = 0;
                }
                return std::monostate{};
            }
            if (name == "vertex_end") {
                const int buffer_id = args.empty() ? active_vertex_buffer : number(args[0]);
                const auto buffer = vertex_buffers.find(buffer_id);
                if (buffer != vertex_buffers.end())
                    buffer->second.building = false;
                if (active_vertex_buffer == buffer_id)
                    active_vertex_buffer = -1;
                return std::monostate{};
            }
            if (name == "vertex_delete_buffer") {
                const int buffer_id = number(args[0]);
                vertex_buffers.erase(buffer_id);
                if (active_vertex_buffer == buffer_id)
                    active_vertex_buffer = -1;
                return std::monostate{};
            }
            if (name == "vertex_submit" || name == "vertex_submit_ext") {
                const int buffer_id = number(args[0]), primitive = number(args[1]);
                const auto buffer = vertex_buffers.find(buffer_id);
                if (buffer == vertex_buffers.end())
                    throw Error(ErrorKind::vm, "vertex_submit received an invalid buffer");
                const auto &vertices = buffer->second.vertices;
                const std::size_t begin = name == "vertex_submit_ext" && args.size() > 3
                                              ? std::min<std::size_t>(number(args[3]), vertices.size())
                                              : 0;
                const std::size_t count =
                    name == "vertex_submit_ext" && args.size() > 4
                        ? std::min<std::size_t>(number(args[4]), vertices.size() - begin)
                        : vertices.size() - begin;
                std::optional<std::pair<int, int>> texture;
                if (args.size() > 2) {
                    if (const auto found = sprite_textures.find(number(args[2]));
                        found != sprite_textures.end())
                        texture = found->second;
                }
                auto triangle = [&](const VertexPoint &a, const VertexPoint &b, const VertexPoint &c) {
                    const auto point_a = assets.transform_point(a.x, a.y);
                    const auto point_b = assets.transform_point(b.x, b.y);
                    const auto point_c = assets.transform_point(c.x, c.y);
                    if (current_canvas().gpu_target()) {
                        const auto channel = [&](unsigned shift) {
                            return (((a.color >> shift) & 255U) + ((b.color >> shift) & 255U) +
                                    ((c.color >> shift) & 255U)) /
                                   3U;
                        };
                        const unsigned tint = channel(0) | (channel(8) << 8) | (channel(16) << 16);
                        current_canvas().color(tint);
                        current_canvas().alpha((a.alpha + b.alpha + c.alpha) / 3.0);
                        current_canvas().triangle(point_a[0], point_a[1], point_b[0], point_b[1], point_c[0],
                                                  point_c[1], false);
                        current_canvas().color(draw_color);
                        current_canvas().alpha(draw_alpha);
                        return;
                    }
                    const double scale = current_canvas().render_scale();
                    const double ax = point_a[0] * scale, ay = point_a[1] * scale;
                    const double bx = point_b[0] * scale, by = point_b[1] * scale;
                    const double cx = point_c[0] * scale, cy = point_c[1] * scale;
                    const double area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
                    if (std::fabs(area) < 1e-12)
                        return;
                    const int left =
                        static_cast<int>(std::clamp(std::floor(std::min({ax, bx, cx})), 0.0,
                                                    static_cast<double>(current_canvas().width())));
                    const int right =
                        static_cast<int>(std::clamp(std::ceil(std::max({ax, bx, cx})), 0.0,
                                                    static_cast<double>(current_canvas().width())));
                    const int top =
                        static_cast<int>(std::clamp(std::floor(std::min({ay, by, cy})), 0.0,
                                                    static_cast<double>(current_canvas().height())));
                    const int bottom =
                        static_cast<int>(std::clamp(std::ceil(std::max({ay, by, cy})), 0.0,
                                                    static_cast<double>(current_canvas().height())));
                    std::array<double, 8> uvs{};
                    if (texture)
                        uvs = assets.sprite_uvs(texture->first, texture->second);
                    for (int py = top; py < bottom; ++py)
                        for (int px = left; px < right; ++px) {
                            const double x = px + 0.5, y = py + 0.5;
                            const double wa = ((bx - x) * (cy - y) - (by - y) * (cx - x)) / area;
                            const double wb = ((cx - x) * (ay - y) - (cy - y) * (ax - x)) / area;
                            const double wc = 1.0 - wa - wb;
                            if (wa < 0 || wb < 0 || wc < 0)
                                continue;
                            const auto channel = [&](unsigned shift) {
                                return static_cast<unsigned>(
                                    std::clamp(std::lround(wa * ((a.color >> shift) & 255U) +
                                                           wb * ((b.color >> shift) & 255U) +
                                                           wc * ((c.color >> shift) & 255U)),
                                               0L, 255L));
                            };
                            const unsigned tint = channel(0) | (channel(8) << 8) | (channel(16) << 16);
                            const double alpha =
                                std::clamp(wa * a.alpha + wb * b.alpha + wc * c.alpha, 0.0, 1.0);
                            std::uint32_t sample = 0xffffffffU;
                            if (texture) {
                                const double u = wa * a.u + wb * b.u + wc * c.u;
                                const double v = wa * a.v + wb * b.v + wc * c.v;
                                const double tw = uvs[2] - uvs[0], th = uvs[3] - uvs[1];
                                const double local_u =
                                    tw != 0 ? (uvs[4] + (u - uvs[0]) / tw *
                                                            (assets.sprite(texture->first).width * uvs[6])) /
                                                  assets.sprite(texture->first).width
                                            : 0;
                                const double local_v =
                                    th != 0 ? (uvs[5] + (v - uvs[1]) / th *
                                                            (assets.sprite(texture->first).height * uvs[7])) /
                                                  assets.sprite(texture->first).height
                                            : 0;
                                sample = assets.sample_sprite(texture->first, texture->second, local_u,
                                                              local_v, false);
                            }
                            current_canvas().rgba_pixel(px, py, sample, tint, alpha);
                        }
                };
                if (primitive == 1) {
                    for (std::size_t index = begin; index < begin + count; ++index) {
                        current_canvas().color(vertices[index].color);
                        current_canvas().alpha(vertices[index].alpha);
                        assets.rectangle(current_canvas(), vertices[index].x, vertices[index].y,
                                         vertices[index].x + 1, vertices[index].y + 1, false);
                    }
                    current_canvas().color(draw_color);
                    current_canvas().alpha(draw_alpha);
                } else if (primitive == 2 || primitive == 3) {
                    const std::size_t stride = primitive == 2 ? 2 : 1;
                    for (std::size_t index = begin; index + 1 < begin + count; index += stride) {
                        current_canvas().color(vertices[index].color);
                        current_canvas().alpha(vertices[index].alpha);
                        assets.draw_line(current_canvas(), vertices[index].x, vertices[index].y,
                                         vertices[index + 1].x, vertices[index + 1].y);
                    }
                    current_canvas().color(draw_color);
                    current_canvas().alpha(draw_alpha);
                } else if (primitive == 4) {
                    for (std::size_t index = begin; index + 2 < begin + count; index += 3)
                        triangle(vertices[index], vertices[index + 1], vertices[index + 2]);
                } else if (primitive == 5) {
                    for (std::size_t index = begin; index + 2 < begin + count; ++index)
                        triangle(vertices[index], vertices[index + 1], vertices[index + 2]);
                } else if (primitive == 6 && count > 2) {
                    for (std::size_t index = begin + 1; index + 1 < begin + count; ++index)
                        triangle(vertices[begin], vertices[index], vertices[index + 1]);
                }
                return std::monostate{};
            }
            return std::nullopt;
        };
        if (auto handled = dispatch_builtin(11, dispatch_builtins_11))
            return std::move(*handled);
        auto dispatch_builtins_12 = [&]() REGML_NOINLINE_LAMBDA -> std::optional<Value> {
            if (name == "switch_accounts_is_user_open")
                return false;
            if (name == "switch_controller_support_get_selected_id")
                return std::int32_t{-1};
            if (name == "switch_language_get_desired_language")
                return std::int32_t{0};
            if (name == "switch_save_data_mount")
                return true;
            if (name == "switch_accounts_open_user" || name == "switch_accounts_select_account" ||
                name == "switch_controller_support_show" ||
                name == "switch_controller_set_supported_styles" ||
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
            if (name == "log10")
                return std::log10(n(0));
            if (name == "logn")
                return std::log(n(1)) / std::log(n(0));
            if (name == "abs")
                return std::fabs(n(0));
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
            if (name == "mean" || name == "median") {
                if (args.empty())
                    throw Error(ErrorKind::vm, std::string(name) + " requires at least one value");
                std::vector<double> values;
                values.reserve(args.size());
                for (std::size_t i = 0; i < args.size(); ++i)
                    values.push_back(n(i));
                if (name == "mean") {
                    double sum = 0;
                    for (const double value : values)
                        sum += value;
                    return sum / static_cast<double>(values.size());
                }
                const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
                std::nth_element(values.begin(), middle, values.end());
                return *middle;
            }
            if (name == "frac")
                return n(0) - std::floor(n(0));
            if (name == "arcsin")
                return std::asin(n(0)) * 180.0 / 3.141592653589793;
            if (name == "arccos")
                return std::acos(n(0)) * 180.0 / 3.141592653589793;
            if (name == "radtodeg")
                return n(0) * 180.0 / 3.141592653589793;
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
            if (name == "random_range") {
                const double low = std::min(n(0), n(1)), high = std::max(n(0), n(1));
                if (low == high)
                    return low;
                return std::uniform_real_distribution<double>(low, high)(random);
            }
            if (name == "irandom_range") {
                const int first = static_cast<int>(std::floor(n(0)));
                const int second = static_cast<int>(std::floor(n(1)));
                return std::int32_t{std::uniform_int_distribution<int>(std::min(first, second),
                                                                       std::max(first, second))(random)};
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
                const double fps =
                    value_number(engine.contains("room_speed") ? engine.at("room_speed") : Value{60.0});
                return args.empty() || n(0) == 0 ? Value{fps} : Value{fps > 0 ? 1000000.0 / fps : 0.0};
            }
            if (name == "display_set_gui_size") {
                const double width = n(0), height = n(1);
                if (width == -1 && height == -1) {
                    engine["gui_width"] = static_cast<double>(render_width);
                    engine["gui_height"] = static_cast<double>(render_height);
                } else {
                    engine["gui_width"] = width;
                    engine["gui_height"] = height;
                }
                if (gui_draw_active)
                    update_gui_transform();
                return std::monostate{};
            }
            if (name == "display_get_width")
                return static_cast<double>(render_width);
            if (name == "display_get_height")
                return static_cast<double>(render_height);
            if (name == "display_get_gui_width")
                return engine.contains("gui_width") ? engine.at("gui_width")
                                                    : Value{static_cast<double>(render_width)};
            if (name == "display_get_gui_height")
                return engine.contains("gui_height") ? engine.at("gui_height")
                                                     : Value{static_cast<double>(render_height)};
            if (name == "window_get_width")
                return engine.contains("window_width") ? engine.at("window_width")
                                                       : Value{static_cast<double>(render_width)};
            if (name == "window_get_height")
                return engine.contains("window_height") ? engine.at("window_height")
                                                        : Value{static_cast<double>(render_height)};
            if (name == "window_get_x")
                return engine.contains("window_x") ? engine.at("window_x") : Value{0.0};
            if (name == "window_get_y")
                return engine.contains("window_y") ? engine.at("window_y") : Value{0.0};
            if (name == "window_get_fullscreen")
                return engine.contains("window_fullscreen") &&
                       value_number(engine.at("window_fullscreen")) != 0;
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
            if (name == "get_timer")
                return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                               std::chrono::steady_clock::now() - start_time)
                                               .count());
            if (name == "get_string")
                return args.size() > 1 ? Value{text(1)} : Value{std::string{}};
            if (name == "get_integer")
                return args.size() > 1 ? Value{n(1)} : Value{0.0};
            if (name == "show_question")
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
                configure_shader(current_canvas());
                return std::monostate{};
            }
            if (name == "shaders_are_supported")
                return shoujo_shader >= 0 && !this->canvas.gpu_target();
            if (name == "shader_is_compiled")
                return shader_is_supported(static_cast<int>(n(0)));
            if (name == "shader_get_uniform" || name == "shader_get_sampler_index")
                return std::int32_t{shader_uniform_handle(static_cast<int>(n(0)), text(1),
                                                          name == "shader_get_sampler_index")};
            if (name == "shader_current")
                return std::int32_t{active_shader};
            if (name == "shader_set") {
                active_shader = static_cast<int>(n(0));
                shoujo_texture_failed.fill(false);
                configure_shader(current_canvas());
                return std::monostate{};
            }
            if (name == "shader_reset") {
                active_shader = -1;
                configure_shader(current_canvas());
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
                        (sprite_textures.contains(texture) || (texture_surfaces.contains(texture) &&
                                                               surface_exists(texture_surfaces.at(texture)))))
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
            return std::nullopt;
        };
        if (auto handled = dispatch_builtin(12, dispatch_builtins_12))
            return std::move(*handled);
        throw Error(ErrorKind::unsupported, "engine built-in pending: " + std::string(name));
    }
    void tick(const KeyboardState &input, const KeyboardState &edges) {
        using ProfileClock = std::chrono::steady_clock;
        const auto instance_motion_paused = [](const Instance &instance) {
            const auto paused = instance.vars.find("paused");
            if (paused == instance.vars.end())
                return false;
            const auto &value = paused->second;
            if (const auto *integer = std::get_if<std::int32_t>(&value))
                return *integer != 0;
            if (const auto *real = std::get_if<double>(&value))
                return *real != 0;
            if (const auto *boolean = std::get_if<bool>(&value))
                return *boolean;
            return false;
        };
        const auto profile_start = profile_tick_timing ? ProfileClock::now() : ProfileClock::time_point{};
        auto draw_start = ProfileClock::time_point{};
        last_logic_ms = last_draw_ms = 0;
        last_draw_phases_ms.fill(0);
        last_draw_depth_visual_ms = last_draw_instance_body_ms = 0;
        last_draw_object_hotspots.clear();
        last_draw_visual_kind_ms.fill(0);
        last_draw_visual_kind_calls.fill(0);
        last_draw_layer_hotspots.clear();
        last_draw_event_ms = 0;
        last_draw_builtin_ms = 0;
        last_draw_builtin_categories.fill(0);
        max_draw_builtin_call_ms = 0;
        slowest_draw_builtin = {};
        slowest_draw_builtin_details.clear();
        draw_event_depth = 0;
        const char *allocation_stage = "audio service";
        try {
            audio.service();
            static constexpr std::array<std::string_view, 12> alarm_names = {
                "alarm[0]", "alarm[1]", "alarm[2]", "alarm[3]", "alarm[4]",  "alarm[5]",
                "alarm[6]", "alarm[7]", "alarm[8]", "alarm[9]", "alarm[10]", "alarm[11]"};
            physical_held = input;
            held = input;
            pressed = edges;
            for (std::size_t k = 0; k < 256; ++k) {
                if (!physical_held[k])
                    cleared_until_release[k] = false;
                if (cleared_until_release[k]) {
                    held[k] = false;
                    pressed[k] = false;
                    released[k] = false;
                } else {
                    pressed[k] = pressed[k] || (held[k] && !previous_held[k]);
                    released[k] = !held[k] && previous_held[k];
                }
            }
            previous_held = held;
            for (std::size_t k = 0; k < gamepad.buttons.size(); ++k) {
                gamepad.pressed[k] = gamepad.pressed[k] || (gamepad.buttons[k] && !gamepad_previous[k]);
                gamepad_released[k] = !gamepad.buttons[k] && gamepad_previous[k];
            }
            gamepad_previous = gamepad.buttons;
            event_budget = 0;
            allocation_stage = "audio recording events";
            service_audio_recorders();
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
                if (!i->alive || i->deactivated || instance_motion_paused(*i))
                    continue;
                event(i, 3, 1);
                if (i->alive) {
                    i->vars["xprevious"] = i->vars["x"];
                    i->vars["yprevious"] = i->vars["y"];
                }
            }
            for (auto &i : snapshot)
                if (i->alive && !i->deactivated && !instance_motion_paused(*i))
                    for (int a = 0; a < 12; ++a) {
                        auto &v = i->vars.at(alarm_names[static_cast<std::size_t>(a)]);
                        const auto timer = number(v);
                        if (timer > 0) {
                            v = std::int32_t{timer - 1};
                            if (timer == 1)
                                event(i, 2, a);
                        }
                    }
            for (auto &i : snapshot) {
                if (i->deactivated)
                    continue;
                if (!i->alive || instance_motion_paused(*i))
                    continue;
                event(i, 3, 0);
            }
            for (auto &i : snapshot)
                if (i->alive && !i->deactivated) {
                    if (instance_motion_paused(*i))
                        continue;
                    const double spatial_x_before = value_number(i->vars.at("x"));
                    const double spatial_y_before = value_number(i->vars.at("y"));
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
                                        std::max(std::fabs(scale), 0.000001);
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
                    if (value_number(i->vars.at("x")) != spatial_x_before ||
                        value_number(i->vars.at("y")) != spatial_y_before)
                        update_spatial_instance(*i);
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
            // Instances created during Begin Step / Step must be eligible for
            // this frame's collision pass. The per-frame event snapshot above
            // intentionally excludes them from Step and alarm processing, but
            // GameMaker checks collisions after Step and movement. In particular,
            // one-step attack hitboxes created during Step can be destroyed by
            // their alarm on the following frame.
            collision_instances.assign(instances.begin(), instances.end());
            if (tick_collision_routes.size() != objects.size()) {
                tick_collision_routes.resize(objects.size());
                tick_collision_route_stamps.assign(objects.size(), 0);
                tick_collision_route_generation = 0;
            }
            collision_grid_dirty = true;
            for (const auto &i : collision_instances) {
                if (!i->alive || i->object < 0 || static_cast<std::size_t>(i->object) >= objects.size() ||
                    !objects[static_cast<std::size_t>(i->object)].has_collision_events)
                    continue;
                if (collision_grid_dirty)
                    rebuild_collision_grid();
                if (collision_target_instance_count == 0)
                    continue;
                if (++tick_collision_route_generation == 0) {
                    std::fill(tick_collision_route_stamps.begin(), tick_collision_route_stamps.end(), 0);
                    ++tick_collision_route_generation;
                }
                const auto route_generation = tick_collision_route_generation;
                const auto process_pair = [&](std::size_t target_index,
                                              bool use_route_cache) -> std::pair<bool, bool> {
                    const auto &j = collision_instances[target_index];
                    if (!j->alive || i == j || j->object < 0 ||
                        static_cast<std::size_t>(j->object) >= objects.size())
                        return {false, false};

                    CollisionRoute route;
                    const auto target_object = static_cast<std::size_t>(j->object);
                    if (use_route_cache) {
                        if (tick_collision_route_stamps[target_object] != route_generation) {
                            tick_collision_routes[target_object] = collision_event(i->object, j->object);
                            tick_collision_route_stamps[target_object] = route_generation;
                        }
                        route = tick_collision_routes[target_object];
                    } else {
                        route = collision_event(i->object, j->object);
                    }
                    if (route.source < 0 ||
                        !collides(*i, value_number(i->vars.at("x")), value_number(i->vars.at("y")), *j))
                        return {false, false};

                    const double other_x = value_number(j->vars.at("x"));
                    const double other_y = value_number(j->vars.at("y"));
                    const double other_step_x = other_x - value_number(j->vars.at("xprevious"));
                    const double other_step_y = other_y - value_number(j->vars.at("yprevious"));
                    const auto geometry_before = collision_geometry_generation;
                    {
                        struct CollisionPassGuard {
                            bool &active;
                            bool previous;
                            explicit CollisionPassGuard(bool &active_value)
                                : active(active_value), previous(active_value) {
                                active = true;
                            }
                            ~CollisionPassGuard() { active = previous; }
                        } collision_pass_guard{collision_pass_active};
                        event(i, 4, route.target, route.source, j);
                    }
                    bool geometry_changed =
                        gmrt_project.has_value() || collision_geometry_generation != geometry_before;
                    bool target_repositioned = false;
                    if (j->alive) {
                        // Collision code can move `other` by the same amount it already moved
                        // this step. The collision runs after Step, so don't count that
                        // movement twice.
                        const double pushed_x = value_number(j->vars.at("x")) - other_x;
                        const double pushed_y = value_number(j->vars.at("y")) - other_y;
                        if (other_step_x != 0 && std::fabs(pushed_x - other_step_x) < 1e-9) {
                            j->vars["x"] = value_number(j->vars.at("x")) - other_step_x;
                            geometry_changed = true;
                            target_repositioned = true;
                        }
                        if (other_step_y != 0 && std::fabs(pushed_y - other_step_y) < 1e-9) {
                            j->vars["y"] = value_number(j->vars.at("y")) - other_step_y;
                            geometry_changed = true;
                            target_repositioned = true;
                        }
                    }
                    if (target_repositioned)
                        update_spatial_instance(*j);
                    if (geometry_changed)
                        collision_grid_dirty = true;
                    return {true, geometry_changed};
                };

                const auto source_bounds = bounds(*i);
                if (!collect_collision_candidates(source_bounds)) {
                    for (std::size_t target_index = 0; target_index < collision_instances.size();
                         ++target_index)
                        process_pair(target_index, false);
                    continue;
                }

                for (const auto target_index : collision_query_indices) {
                    const auto [collided, geometry_changed] = process_pair(target_index, true);
                    if (!collided)
                        continue;
                    if (geometry_changed) {
                        // An event changed collision geometry. Recheck the untouched suffix
                        // in original instance order, since the spatial snapshot is stale.
                        for (std::size_t remaining = target_index + 1; remaining < collision_instances.size();
                             ++remaining)
                            process_pair(remaining, false);
                        break;
                    }
                }
            }
            allocation_stage = "end-step events";
            for (const auto &i : snapshot) {
                if (i->deactivated)
                    continue;
                if (!i->alive || instance_motion_paused(*i))
                    continue;
                event(i, 3, 2);
            }
            allocation_stage = "particle updates";
            std::vector<std::array<int, 3>> active_particle_streams;
            active_particle_streams.reserve(particle_emitters.size());
            for (const auto &[id, emitter] : particle_emitters)
                if (emitter.stream_type >= 0 && emitter.stream_count != 0)
                    active_particle_streams.push_back({emitter.system, id, emitter.stream_type});
            for (const auto &stream : active_particle_streams) {
                const auto emitter = particle_emitters.find(stream[1]);
                if (emitter != particle_emitters.end())
                    emit_particles(stream[0], stream[1], stream[2], emitter->second.stream_count);
            }
            std::vector<int> automatic_particle_systems;
            automatic_particle_systems.reserve(particle_systems.size());
            for (const auto &[id, system] : particle_systems)
                if (system.automatic_update)
                    automatic_particle_systems.push_back(id);
            for (const int id : automatic_particle_systems) {
                const std::array<Value, 1> update_args{std::int32_t{id}};
                call("part_system_update", update_args, {}, {});
            }
            allocation_stage = "delayed callbacks";
            const std::size_t completed_frame = frame + 1;
            std::vector<int> due_callbacks;
            for (const auto &timer : delayed_calls)
                if (timer.due_frame <= completed_frame)
                    due_callbacks.push_back(timer.id);
            for (const int timer_id : due_callbacks) {
                const auto found =
                    std::find_if(delayed_calls.begin(), delayed_calls.end(),
                                 [timer_id](const DelayedCall &timer) { return timer.id == timer_id; });
                if (found == delayed_calls.end())
                    continue;
                const auto timer = *found;
                if (found->loop)
                    found->due_frame += found->period_frames;
                else
                    delayed_calls.erase(found);
                if (const auto *function = std::get_if<FunctionReference>(&timer.callback))
                    invoke_function(function->symbol_id, std::span<const Value>{}, timer.self, timer.other,
                                    timer.struct_self);
                else if (const auto *callable = std::get_if<CallablePtr>(&timer.callback);
                         callable && *callable && (*callable)->invoke)
                    (*callable)->invoke(std::span<const Value>{},
                                        timer.self ? std::optional<int>{timer.self->id} : std::nullopt);
                else
                    throw Error(ErrorKind::vm, "call_later callback is no longer callable");
            }
            if (number(engine.at("view_enabled")) != 0 && number(engine.at("view_visible[0]")) != 0) {
                const int target = number(engine.at("view_object[0]"));
                const double vw = value_number(engine.at("view_wview[0]")),
                             vh = value_number(engine.at("view_hview[0]"));
                if (target >= 0 && vw > 0 && vh > 0) {
                    auto tracked = find_alive_instance_by_id(target);
                    if (!tracked)
                        tracked = find_first_alive_exact_object(target);
                    if (tracked) {
                        const auto &room = rooms.at(static_cast<std::size_t>(current_room));
                        auto follow = [&](const char *view_key, const char *speed_key, const char *coordinate,
                                          double extent, double border, double room_extent) {
                            double origin = value_number(engine.at(view_key));
                            const double pos = value_number(tracked->vars.at(coordinate));
                            double desired = origin;
                            // Keep a non-empty dead zone. Borders larger than half the
                            // view make the left and right follow conditions overlap;
                            // with a full-view border they alternately snap the camera
                            // one viewport up and down each frame.
                            const double safe_border = std::clamp(border, 0.0, extent * 0.5);
                            if (pos < origin + safe_border)
                                desired = pos - safe_border;
                            else if (pos > origin + extent - safe_border)
                                desired = pos - extent + safe_border;
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
                if (port_x == 0 && port_y == 0 && std::fabs(port_width - screen_width) < 0.5 &&
                    port_height < screen_height && std::fabs(w / h - screen_width / screen_height) < 0.001) {
                    port_height = screen_height;
                    engine["view_hport[0]"] = port_height;
                }
                const double surface_scale_x = port_width > 0 ? canvas.logical_width() / port_width : 1.0;
                const double surface_scale_y = port_height > 0 ? canvas.logical_height() / port_height : 1.0;
                assets.view(value_number(engine.at("view_xview[0]")),
                            value_number(engine.at("view_yview[0]")), port_width / w * surface_scale_x,
                            port_height / h * surface_scale_y, port_x * surface_scale_x,
                            port_y * surface_scale_y);
            } else {
                const auto &active_room = rooms.at(static_cast<std::size_t>(current_room));
                const double scale_x =
                    static_cast<double>(canvas.logical_width()) / std::max(1, active_room.width);
                const double scale_y =
                    static_cast<double>(canvas.logical_height()) / std::max(1, active_room.height);
                assets.view(0, 0, scale_x, scale_y, 0, 0);
            }
            auto draw_phase_start = profile_tick_timing ? ProfileClock::now() : ProfileClock::time_point{};
            if (profile_tick_timing)
                last_draw_phases_ms[0] =
                    std::chrono::duration<double, std::milli>(draw_phase_start - draw_start).count();
            auto &room = rooms.at(static_cast<std::size_t>(current_room));
            allocation_stage = "pre-draw events";
            for (const auto &i : snapshot)
                if (i->alive && !i->deactivated && number(i->vars.at("visible")))
                    event(i, event_draw, event_draw_pre);
            allocation_stage = "draw begin events";
            for (const auto &i : snapshot)
                if (i->alive && !i->deactivated && number(i->vars.at("visible")))
                    event(i, event_draw, event_draw_begin);
            for (auto &layer : room.layers) {
                layer.x += layer.hspeed;
                layer.y += layer.vspeed;
            }
            if (profile_tick_timing) {
                const auto now = ProfileClock::now();
                last_draw_phases_ms[1] =
                    std::chrono::duration<double, std::milli>(now - draw_phase_start).count();
                draw_phase_start = now;
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
                if (const auto found = room.layer_indices.find(t.layer_id);
                    found != room.layer_indices.end()) {
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
                if (t.sprite_tile) {
                    assets.draw_sprite_part(canvas, t.id, 0, source_x, source_y, t.width, t.height,
                                            t.x + ox + layer_x, t.y + oy + layer_y, t.sx, t.sy,
                                            t.color & 0xffffffU, static_cast<double>(t.color >> 24) / 255.0);
                } else {
                    assets.draw_tile(canvas, t.id, source_x, source_y, t.width, t.height, t.x + ox + layer_x,
                                     t.y + oy + layer_y, t.sx, t.sy, t.color, t.transform);
                }
            };
            std::size_t layer_visual_count = 0;
            for (const auto &layer : room.layers)
                if (layer.visible)
                    layer_visual_count += layer.sprites.size() + layer.backgrounds.size();
            layer_visuals.clear();
            layer_visuals.reserve(layer_visual_count);
            std::size_t layer_visual_order = 0;
            for (std::size_t layer_index = 0; layer_index < room.layers.size(); ++layer_index) {
                const auto &layer = room.layers[layer_index];
                if (!layer.visible)
                    continue;
                for (std::size_t element_index = 0; element_index < layer.sprites.size(); ++element_index) {
                    const auto &sprite = layer.sprites[element_index];
                    layer_visuals.push_back({static_cast<double>(layer.depth), layer_index, element_index,
                                             layer_visual_order++, layer.id, sprite.id, false});
                }
                for (std::size_t element_index = 0; element_index < layer.backgrounds.size();
                     ++element_index) {
                    const auto &background = layer.backgrounds[element_index];
                    layer_visuals.push_back({background.foreground ? std::numeric_limits<double>::lowest()
                                                                   : static_cast<double>(layer.depth),
                                             layer_index, element_index, layer_visual_order++, layer.id,
                                             background.id, true});
                }
            }
            std::sort(layer_visuals.begin(), layer_visuals.end(), [](const auto &left, const auto &right) {
                return left.depth != right.depth ? left.depth > right.depth : left.order < right.order;
            });
            particle_visuals.clear();
            particle_visuals.reserve(particle_systems.size());
            std::size_t particle_visual_order = 0;
            for (const auto &[id, system] : particle_systems)
                if (system.automatic_draw)
                    particle_visuals.push_back(
                        {static_cast<double>(system.depth), id, particle_visual_order++});
            std::sort(
                particle_visuals.begin(), particle_visuals.end(), [](const auto &left, const auto &right) {
                    return left.depth != right.depth ? left.depth > right.depth : left.order < right.order;
                });
            if (profile_tick_timing) {
                const auto now = ProfileClock::now();
                last_draw_phases_ms[2] =
                    std::chrono::duration<double, std::milli>(now - draw_phase_start).count();
                draw_phase_start = now;
            }
            auto draw_layer_visual = [&](const LayerVisualEntry &entry) {
                std::size_t layer_index = entry.layer_index;
                if (layer_index >= room.layers.size() || room.layers[layer_index].id != entry.layer_id) {
                    const auto found = room.layer_indices.find(entry.layer_id);
                    if (found == room.layer_indices.end())
                        return;
                    layer_index = found->second;
                }
                auto &layer = room.layers[layer_index];
                if (!layer.visible)
                    return;
                const double ox = layer.x - layer.origin_x, oy = layer.y - layer.origin_y;
                if (entry.background) {
                    LayerBackground *background = nullptr;
                    if (entry.element_index < layer.backgrounds.size() &&
                        layer.backgrounds[entry.element_index].id == entry.element_id) {
                        background = &layer.backgrounds[entry.element_index];
                    } else {
                        const auto found = std::find_if(
                            layer.backgrounds.begin(), layer.backgrounds.end(),
                            [&](const LayerBackground &item) { return item.id == entry.element_id; });
                        if (found != layer.backgrounds.end())
                            background = &*found;
                    }
                    if (!background || !background->visible || !assets.sprite_exists(background->sprite))
                        return;
                    const auto &sprite = assets.sprite(background->sprite);
                    if (sprite.width <= 0 || sprite.height <= 0)
                        return;
                    double sx = background->sx, sy = background->sy;
                    if (background->stretch) {
                        sx *= static_cast<double>(room.width) / sprite.width;
                        sy *= static_cast<double>(room.height) / sprite.height;
                    }
                    const double step_x = sprite.width * sx, step_y = sprite.height * sy;
                    if (!std::isfinite(step_x) || !std::isfinite(step_y) || step_x == 0 || step_y == 0)
                        return;
                    const double stride_x = std::fabs(step_x), stride_y = std::fabs(step_y);
                    double x = ox, y = oy;
                    if (background->htiled)
                        x -= std::ceil(std::max(x, 0.0) / stride_x) * stride_x;
                    if (background->vtiled)
                        y -= std::ceil(std::max(y, 0.0) / stride_y) * stride_y;
                    const auto tile_count = [](double extent, double origin, double stride) {
                        const double count = std::ceil(std::max(0.0, extent - origin) / stride);
                        if (!std::isfinite(count) || count >= 256)
                            return 256;
                        return std::clamp(static_cast<int>(count), 1, 256);
                    };
                    const int columns = background->htiled ? tile_count(room.width, x, stride_x) : 1;
                    const int rows = background->vtiled ? tile_count(room.height, y, stride_y) : 1;
                    const auto tint = background->color & 0xffffffU;
                    const double alpha = static_cast<double>(background->color >> 24) / 255.0;
                    for (int row = 0; row < rows && row * columns < 4096; ++row)
                        for (int column = 0; column < columns && row * columns + column < 4096; ++column)
                            assets.draw_sprite(canvas, background->sprite, background->frame,
                                               x + column * stride_x, y + row * stride_y, sx, sy, 0, tint,
                                               alpha);
                    const double frame_count = static_cast<double>(sprite.frames.size());
                    if (frame_count > 0) {
                        const double fps = value_number(engine.at("room_speed"));
                        const double advance =
                            background->speed_type == 0 ? background->speed / fps : background->speed;
                        background->frame = std::fmod(
                            std::fmod(background->frame + advance, frame_count) + frame_count, frame_count);
                    }
                    return;
                }
                LayerSprite *sprite_entry = nullptr;
                if (entry.element_index < layer.sprites.size() &&
                    layer.sprites[entry.element_index].id == entry.element_id) {
                    sprite_entry = &layer.sprites[entry.element_index];
                } else {
                    const auto found =
                        std::find_if(layer.sprites.begin(), layer.sprites.end(),
                                     [&](const LayerSprite &item) { return item.id == entry.element_id; });
                    if (found != layer.sprites.end())
                        sprite_entry = &*found;
                }
                if (!sprite_entry || !assets.sprite_exists(sprite_entry->sprite))
                    return;
                auto &sprite = *sprite_entry;
                assets.draw_sprite(canvas, sprite.sprite, sprite.frame, sprite.x + ox, sprite.y + oy,
                                   sprite.sx, sprite.sy, sprite.rotation, sprite.color & 0xffffffU,
                                   static_cast<double>(sprite.color >> 24) / 255.0);
                const double frame_count = static_cast<double>(assets.sprite(sprite.sprite).frames.size());
                if (frame_count > 0 && sprite.speed != 0) {
                    const double fps = value_number(engine.at("room_speed"));
                    const double advance = sprite.speed_type == 0 ? sprite.speed / fps : sprite.speed;
                    sprite.frame =
                        std::fmod(std::fmod(sprite.frame + advance, frame_count) + frame_count, frame_count);
                }
            };
            auto run_layer_script = [&](const Value &script, const InstancePtr &context) {
                if (std::holds_alternative<std::monostate>(script))
                    return;
                const auto old_event_type = engine.find("event_type");
                const auto old_event_number = engine.find("event_number");
                const std::optional<Value> saved_event_type =
                    old_event_type == engine.end() ? std::nullopt
                                                   : std::optional<Value>{old_event_type->second};
                const std::optional<Value> saved_event_number =
                    old_event_number == engine.end() ? std::nullopt
                                                     : std::optional<Value>{old_event_number->second};
                engine["event_type"] = std::int32_t{8};
                engine["event_number"] = std::int32_t{0};
                if (const auto *callable = std::get_if<CallablePtr>(&script);
                    callable && *callable && (*callable)->invoke)
                    (*callable)->invoke(std::span<const Value>{},
                                        context ? std::optional<int>{context->id} : std::nullopt);
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
            std::size_t particle_visual = 0;
            allocation_stage = "draw-list allocation";
            auto instance_draw_depth = [&](const InstancePtr &instance) {
                if (!instance->manual_depth) {
                    const int layer_id = number(instance->vars.at("layer"));
                    if (const auto layer = room.layer_indices.find(layer_id);
                        layer != room.layer_indices.end())
                        return static_cast<double>(room.layers[layer->second].depth);
                }
                return value_number(instance->vars.at("depth"));
            };
            bool rebuild_draw_order = draw_entries.size() != snapshot.size();
            bool resort_draw_order = false;
            if (!rebuild_draw_order) {
                for (auto &entry : draw_entries) {
                    if (entry.order >= snapshot.size() || entry.instance != snapshot[entry.order].get()) {
                        rebuild_draw_order = true;
                        break;
                    }
                    const double depth = instance_draw_depth(snapshot[entry.order]);
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
                    draw_entries.push_back({instance_draw_depth(instance), index, instance.get()});
                }
                resort_draw_order = true;
            }
            if (resort_draw_order)
                std::sort(draw_entries.begin(), draw_entries.end(), [](const auto &a, const auto &b) {
                    return a.depth != b.depth ? a.depth > b.depth : a.order < b.order;
                });
            if (profile_tick_timing) {
                const auto now = ProfileClock::now();
                last_draw_phases_ms[3] =
                    std::chrono::duration<double, std::milli>(now - draw_phase_start).count();
                draw_phase_start = now;
            }
            allocation_stage = "instance draw calls";
            auto profile_visual_call = [&](std::size_t category, auto &&draw) {
                if (!profile_tick_timing) {
                    draw();
                    return;
                }
                const auto start = ProfileClock::now();
                draw();
                last_draw_visual_kind_ms[category] +=
                    std::chrono::duration<double, std::milli>(ProfileClock::now() - start).count();
                ++last_draw_visual_kind_calls[category];
            };
            std::vector<std::pair<LayerVisualEntry, double>> layer_visual_times;
            if (profile_tick_timing)
                layer_visual_times.reserve(layer_visuals.size());
            auto profile_layer_visual = [&](const LayerVisualEntry &entry) {
                if (!profile_tick_timing) {
                    draw_layer_visual(entry);
                    return;
                }
                const auto start = ProfileClock::now();
                draw_layer_visual(entry);
                const double elapsed =
                    std::chrono::duration<double, std::milli>(ProfileClock::now() - start).count();
                last_draw_visual_kind_ms[1] += elapsed;
                ++last_draw_visual_kind_calls[1];
                layer_visual_times.emplace_back(entry, elapsed);
            };
            auto draw_world_before = [&](double depth) {
                const auto visual_start =
                    profile_tick_timing ? ProfileClock::now() : ProfileClock::time_point{};
                while ((tile < room.tiles.size() && room.tiles[tile].depth >= depth) ||
                       (layer_visual < layer_visuals.size() && layer_visuals[layer_visual].depth >= depth) ||
                       (particle_visual < particle_visuals.size() &&
                        particle_visuals[particle_visual].depth >= depth)) {
                    if (tile < room.tiles.size() && room.tiles[tile].depth >= depth &&
                        (layer_visual >= layer_visuals.size() ||
                         room.tiles[tile].depth >= layer_visuals[layer_visual].depth) &&
                        (particle_visual >= particle_visuals.size() ||
                         room.tiles[tile].depth >= particle_visuals[particle_visual].depth))
                        profile_visual_call(0, [&] { draw_tile(room.tiles[tile++]); });
                    else if (layer_visual < layer_visuals.size() &&
                             layer_visuals[layer_visual].depth >= depth &&
                             (particle_visual >= particle_visuals.size() ||
                              layer_visuals[layer_visual].depth >= particle_visuals[particle_visual].depth))
                        profile_layer_visual(layer_visuals[layer_visual++]);
                    else {
                        const int system_id = particle_visuals[particle_visual++].system;
                        if (particle_systems.contains(system_id)) {
                            const std::array<Value, 1> draw_args{std::int32_t{system_id}};
                            profile_visual_call(2, [&] { call("part_system_drawit", draw_args, {}, {}); });
                        }
                    }
                }
                if (profile_tick_timing)
                    last_draw_depth_visual_ms +=
                        std::chrono::duration<double, std::milli>(ProfileClock::now() - visual_start).count();
            };
            std::unordered_map<int, double> draw_object_times;
            if (profile_tick_timing)
                draw_object_times.reserve(draw_entries.size());
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
                    draw_world_before(entry.depth);
                    const auto object_draw_start =
                        profile_tick_timing ? ProfileClock::now() : ProfileClock::time_point{};
                    if (objects[static_cast<std::size_t>(i->object)].draw_owner >= 0)
                        event(i, event_draw, event_draw_normal);
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
                    if (profile_tick_timing) {
                        const double elapsed =
                            std::chrono::duration<double, std::milli>(ProfileClock::now() - object_draw_start)
                                .count();
                        last_draw_instance_body_ms += elapsed;
                        draw_object_times[i->object] += elapsed;
                    }
                }
            }
            auto trailing_visual_start =
                profile_tick_timing ? ProfileClock::now() : ProfileClock::time_point{};
            draw_world_before(std::numeric_limits<double>::lowest());
            while (layer_visual < layer_visuals.size())
                profile_layer_visual(layer_visuals[layer_visual++]);
            for (const auto &b : room.backgrounds)
                if (b.foreground)
                    assets.draw_background(canvas, b.id, b.x, b.y);
            if (profile_tick_timing)
                last_draw_depth_visual_ms +=
                    std::chrono::duration<double, std::milli>(ProfileClock::now() - trailing_visual_start)
                        .count();
            if (profile_tick_timing && !draw_object_times.empty()) {
                std::vector<std::pair<int, double>> hotspots(draw_object_times.begin(),
                                                             draw_object_times.end());
                const auto hotspot_count = std::min<std::size_t>(10, hotspots.size());
                std::partial_sort(
                    hotspots.begin(), hotspots.begin() + hotspot_count, hotspots.end(),
                    [](const auto &left, const auto &right) { return left.second > right.second; });
                std::ostringstream details;
                details << std::fixed << std::setprecision(3);
                for (std::size_t index = 0; index < hotspot_count; ++index) {
                    if (index)
                        details << ';';
                    const int object_id = hotspots[index].first;
                    const std::string_view object_name =
                        object_id >= 0 && static_cast<std::size_t>(object_id) < objects.size()
                            ? std::string_view(objects[static_cast<std::size_t>(object_id)].name)
                            : std::string_view("<invalid>");
                    details << object_name << '=' << hotspots[index].second << "ms";
                }
                last_draw_object_hotspots = details.str();
            }
            if (profile_tick_timing && !layer_visual_times.empty()) {
                std::sort(layer_visual_times.begin(), layer_visual_times.end(),
                          [](const auto &left, const auto &right) { return left.second > right.second; });
                std::ostringstream details;
                details << std::fixed << std::setprecision(3);
                for (std::size_t index = 0; index < layer_visual_times.size(); ++index) {
                    if (index)
                        details << ';';
                    const auto &[entry, elapsed] = layer_visual_times[index];
                    const auto &layer = room.layers[entry.layer_index];
                    int sprite_id = -1;
                    if (entry.background) {
                        const LayerBackground *background = nullptr;
                        if (entry.element_index < layer.backgrounds.size() &&
                            layer.backgrounds[entry.element_index].id == entry.element_id)
                            background = &layer.backgrounds[entry.element_index];
                        else if (const auto found = std::find_if(
                                     layer.backgrounds.begin(), layer.backgrounds.end(),
                                     [&](const auto &item) { return item.id == entry.element_id; });
                                 found != layer.backgrounds.end())
                            background = &*found;
                        if (background)
                            sprite_id = background->sprite;
                    } else {
                        const LayerSprite *sprite = nullptr;
                        if (entry.element_index < layer.sprites.size() &&
                            layer.sprites[entry.element_index].id == entry.element_id)
                            sprite = &layer.sprites[entry.element_index];
                        else if (const auto found = std::find_if(
                                     layer.sprites.begin(), layer.sprites.end(),
                                     [&](const auto &item) { return item.id == entry.element_id; });
                                 found != layer.sprites.end())
                            sprite = &*found;
                        if (sprite)
                            sprite_id = sprite->sprite;
                    }
                    details << "layer" << entry.layer_id << ':'
                            << (entry.background ? "background" : "sprite") << entry.element_id;
                    if (sprite_id >= 0 && assets.sprite_exists(sprite_id)) {
                        const auto &sprite = assets.sprite(sprite_id);
                        details << "[" << sprite.name << ':' << sprite.width << 'x' << sprite.height << ']';
                    }
                    details << '=' << elapsed << "ms";
                }
                last_draw_layer_hotspots = details.str();
            }
            if (profile_tick_timing) {
                const auto now = ProfileClock::now();
                last_draw_phases_ms[4] =
                    std::chrono::duration<double, std::milli>(now - draw_phase_start).count();
                draw_phase_start = now;
            }
            allocation_stage = "draw end events";
            for (const auto &i : snapshot)
                if (i->alive && !i->deactivated && number(i->vars.at("visible")))
                    event(i, event_draw, event_draw_end);
            assets.view(0, 0, 1, 1, 0, 0);
            allocation_stage = "post-draw events";
            for (const auto &i : snapshot)
                if (i->alive && !i->deactivated && number(i->vars.at("visible")))
                    event(i, event_draw, event_draw_post);
            allocation_stage = "GUI draw begin events";
            gui_draw_active = true;
            update_gui_transform();
            for (const auto &i : snapshot)
                if (i->alive && !i->deactivated && number(i->vars.at("visible")))
                    event(i, event_draw, event_draw_gui_begin);
            allocation_stage = "GUI draw events";
            for (const auto &i : snapshot)
                if (i->alive && !i->deactivated && number(i->vars.at("visible")))
                    event(i, event_draw, event_draw_gui);
            allocation_stage = "GUI draw end events";
            for (const auto &i : snapshot) {
                if (i->alive && !i->deactivated && number(i->vars.at("visible")))
                    event(i, event_draw, event_draw_gui_end);
            }
            gui_draw_active = false;
            assets.view(0, 0, 1, 1, 0, 0);
            if (profile_tick_timing) {
                const auto now = ProfileClock::now();
                last_draw_phases_ms[5] =
                    std::chrono::duration<double, std::milli>(now - draw_phase_start).count();
                last_draw_ms = std::chrono::duration<double, std::milli>(now - draw_start).count();
            }
            const auto instance_count_before_cleanup = instances.size();
            std::erase_if(instances, [&](const auto &i) {
                if (!i->alive) {
                    unregister_instance_store(i);
                    return true;
                }
                return false;
            });
            if (instances.size() != instance_count_before_cleanup) {
                object_instance_index_dirty = true;
                spatial_grid_built = false;
            }
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
                         const std::filesystem::path &save_directory,
                         const GamepadState &gamepad)
    : impl_(std::make_unique<Impl>(path, seed, save_directory, gamepad)) {}
GameSession::~GameSession() = default;
void GameSession::set_gamepad_state(const GamepadState &state) {
    impl_->gamepad = state;
}
void GameSession::tick(const KeyboardState &h, const KeyboardState &p) {
    impl_->tick(h, p);
    if (impl_->pending_game_change) {
        const auto path = std::move(*impl_->pending_game_change);
        const auto seed = impl_->fixed_seed;
        const auto save_directory = impl_->save_dir;
        const auto profile = impl_->profile_tick_timing;
        auto next = std::make_unique<Impl>(path, seed, save_directory, impl_->gamepad);
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
    profile.draw_setup_ms = impl_->last_draw_phases_ms[0];
    profile.draw_pre_event_ms = impl_->last_draw_phases_ms[1];
    profile.draw_world_setup_ms = impl_->last_draw_phases_ms[2];
    profile.draw_list_ms = impl_->last_draw_phases_ms[3];
    profile.draw_world_ms = impl_->last_draw_phases_ms[4];
    profile.draw_finish_ms = impl_->last_draw_phases_ms[5];
    profile.draw_depth_visual_ms = impl_->last_draw_depth_visual_ms;
    profile.draw_instance_body_ms = impl_->last_draw_instance_body_ms;
    profile.draw_object_hotspots = impl_->last_draw_object_hotspots;
    profile.draw_tile_ms = impl_->last_draw_visual_kind_ms[0];
    profile.draw_layer_visual_ms = impl_->last_draw_visual_kind_ms[1];
    profile.draw_particle_ms = impl_->last_draw_visual_kind_ms[2];
    profile.draw_tile_calls = impl_->last_draw_visual_kind_calls[0];
    profile.draw_layer_visual_calls = impl_->last_draw_visual_kind_calls[1];
    profile.draw_particle_calls = impl_->last_draw_visual_kind_calls[2];
    profile.draw_layer_hotspots = impl_->last_draw_layer_hotspots;
    profile.draw_event_ms = impl_->last_draw_event_ms;
    profile.draw_builtin_ms = impl_->last_draw_builtin_ms;
    profile.draw_text_builtin_ms = impl_->last_draw_builtin_categories[0];
    profile.draw_image_builtin_ms = impl_->last_draw_builtin_categories[1];
    profile.draw_primitive_builtin_ms = impl_->last_draw_builtin_categories[2];
    profile.draw_other_builtin_ms = impl_->last_draw_builtin_categories[3];
    profile.max_draw_builtin_call_ms = impl_->max_draw_builtin_call_ms;
    profile.slowest_draw_builtin = impl_->slowest_draw_builtin;
    profile.slowest_draw_builtin_details = impl_->slowest_draw_builtin_details;
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
    impl_->video_player.mix_audio(output, playback_rate);
    const auto master_gain = static_cast<float>(impl_->audio_master_gain.load(std::memory_order_relaxed));
    for (auto &sample : output)
        sample = std::clamp(sample * master_gain, -1.0f, 1.0f);
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
