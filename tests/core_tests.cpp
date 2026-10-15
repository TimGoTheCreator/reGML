#include "demo/demo.hpp"
#include "game.hpp"
#include "instance_ptr.hpp"
#include "interpreter.hpp"
#include "linker.hpp"
#include "package.hpp"
#include <bit>
#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

namespace {
using Bytes = std::vector<std::uint8_t>;
void check(bool result, std::string_view message) {
    if (!result)
        throw std::runtime_error(std::string(message));
}
void error_is(regml::ErrorKind kind, const std::function<void()> &action,
              std::optional<std::size_t> offset = std::nullopt) {
    try {
        action();
    } catch (const regml::Error &error) {
        check(error.kind() == kind, "wrong error category");
        if (offset)
            check(error.offset() == offset, "wrong absolute error offset");
        return;
    }
    throw std::runtime_error("expected a structured error");
}
void u32(Bytes &bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}
void patch(Bytes &bytes, std::size_t at, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.at(at + shift / 8) = static_cast<std::uint8_t>(value >> shift);
}
Bytes words(std::initializer_list<std::uint32_t> input) {
    Bytes bytes;
    for (auto word : input)
        u32(bytes, word);
    return bytes;
}
std::size_t chunk(Bytes &bytes, std::string_view tag, std::size_t length) {
    for (auto ch : tag)
        bytes.push_back(static_cast<std::uint8_t>(ch));
    u32(bytes, static_cast<std::uint32_t>(length));
    return bytes.size();
}
void finish(Bytes &bytes) {
    patch(bytes, 4, static_cast<std::uint32_t>(bytes.size() - 8));
}
struct Fixture {
    Bytes bytes;
    std::size_t gen = 0, strg = 0, code = 0, record = 0, blob = 0, string_record = 0;
};
Fixture fixture(std::uint8_t version = 16) {
    Fixture f;
    f.bytes = {'F', 'O', 'R', 'M', 0, 0, 0, 0};
    f.gen = chunk(f.bytes, "GEN8", 104);
    f.bytes.resize(f.gen + 104);
    f.bytes[f.gen] = 1;
    f.bytes[f.gen + 1] = version;
    patch(f.bytes, f.gen + 20, 1337);
    patch(f.bytes, f.gen + 44, 1);
    patch(f.bytes, f.gen + 48, 4);
    patch(f.bytes, f.gen + 52, 1804);
    patch(f.bytes, f.gen + 60, 640);
    patch(f.bytes, f.gen + 64, 480);
    const auto demo = regml::demo_bytecode();
    const bool old = version == 14;
    f.code = chunk(f.bytes, "CODE", 0);
    u32(f.bytes, 1);
    u32(f.bytes, 0);
    if (old) {
        f.record = f.bytes.size();
        u32(f.bytes, 0);
        u32(f.bytes, static_cast<std::uint32_t>(demo.size()));
        f.blob = f.bytes.size();
        f.bytes.insert(f.bytes.end(), demo.begin(), demo.end());
    } else {
        f.blob = f.bytes.size();
        f.bytes.insert(f.bytes.end(), demo.begin(), demo.end());
        f.record = f.bytes.size();
        u32(f.bytes, 0);
        u32(f.bytes, static_cast<std::uint32_t>(demo.size()));
        u32(f.bytes, 0x80000002); // locals=2, args=0, high-bit locals flag
        const auto displacement =
            static_cast<std::int32_t>(static_cast<std::int64_t>(f.blob) - static_cast<std::int64_t>(f.bytes.size()));
        u32(f.bytes, std::bit_cast<std::uint32_t>(displacement));
        u32(f.bytes, 0);
    }
    patch(f.bytes, f.code + 4, static_cast<std::uint32_t>(f.record));
    patch(f.bytes, f.code - 4, static_cast<std::uint32_t>(f.bytes.size() - f.code));
    f.strg = chunk(f.bytes, "STRG", 0);
    u32(f.bytes, 2);
    u32(f.bytes, 0);
    u32(f.bytes, 0);
    const std::string names[] = {"reGML fixture", "gml_Script_test"};
    std::size_t pointers[2]{};
    for (std::size_t i = 0; i < 2; ++i) {
        pointers[i] = f.bytes.size();
        u32(f.bytes, static_cast<std::uint32_t>(names[i].size()));
        f.bytes.insert(f.bytes.end(), names[i].begin(), names[i].end());
        f.bytes.push_back(0);
        patch(f.bytes, f.strg + 4 + 4 * i, static_cast<std::uint32_t>(pointers[i]));
    }
    f.string_record = pointers[0];
    patch(f.bytes, f.gen + 40, static_cast<std::uint32_t>(pointers[0] + 4));
    patch(f.bytes, f.gen + 100, static_cast<std::uint32_t>(pointers[0] + 4));
    patch(f.bytes, f.record, static_cast<std::uint32_t>(pointers[1] + 4));
    patch(f.bytes, f.strg - 4, static_cast<std::uint32_t>(f.bytes.size() - f.strg));
    finish(f.bytes);
    return f;
}
regml::VmResult run(const Bytes &bytes, regml::VmOptions options = {}) {
    return regml::execute_numeric(regml::decode(regml::BinaryView(bytes), 16), options);
}
std::int32_t integer_result(const regml::VmResult &result) {
    check(result.returned.has_value(), "missing return value");
    return std::get<std::int32_t>(*result.returned);
}
} // namespace
int main(int argc, char **argv) {
    if (argc == 5 && std::string_view(argv[1]) == "--flowey-regression") {
        try {
            std::ifstream input(argv[3]);
            check(static_cast<bool>(input), "replay cannot be opened");
            std::map<std::size_t, regml::KeyboardState> replay;
            std::string line;
            while (std::getline(input, line)) {
                if (line.empty() || line[0] == '#')
                    continue;
                std::istringstream row(line);
                std::size_t frame;
                check(static_cast<bool>(row >> frame), "invalid replay frame");
                regml::KeyboardState keys{};
                int key;
                while (row >> key) {
                    check(key >= 0 && key < 256, "invalid replay key");
                    keys[static_cast<std::size_t>(key)] = true;
                }
                replay[frame] = keys;
            }
            for (bool dodge : {true, false}) {
                std::filesystem::create_directories(argv[4]);
                regml::GameSession game(argv[2], 1, std::filesystem::path(argv[4]) / (dodge ? "dodge" : "hit"));
                regml::KeyboardState held{};
                bool launched = false, advanced = false;
                std::size_t launch_frame = 0, max_attacking = 0;
                double last_conversation = -1;
                for (std::size_t frame = 0; frame < 7000; ++frame) {
                    if (frame < 3500) {
                        if (const auto it = replay.find(frame); it != replay.end())
                            held = it->second;
                    } else {
                        held = {};
                        if (!launched && frame % 80 == 0)
                            held[90] = true;
                        if (dodge && launched && frame < launch_frame + 20) {
                            held[37] = true;
                            held[40] = true;
                        }
                    }
                    game.tick(held, {});
                    if (frame == 0)
                        check(!game.vm_code_name().empty() && game.vm_address() != 0,
                              "game session exposes the current VM code and bytecode address");
                    if (game.room_name() != "room_floweybattle")
                        continue;
                    const auto states = game.instance_states();
                    double conversation = -1;
                    std::size_t attacking = 0;
                    for (const auto &state : states) {
                        if (state.object_name == "obj_floweybattle1")
                            conversation = regml::value_number(state.variables.at("conversation"));
                        if (state.object_name == "obj_friendlypellet" &&
                            regml::value_number(state.variables.at("attackyou")) == 2)
                            ++attacking;
                    }
                    max_attacking = std::max(max_attacking, attacking);
                    if (!launched && attacking > 0) {
                        launched = true;
                        launch_frame = frame;
                    }
                    if (conversation != last_conversation) {
                        std::cout << (dodge ? "DODGE" : "HIT") << " frame " << frame << " conversation " << conversation
                                  << " attacking pellets " << attacking << '\n';
                        last_conversation = conversation;
                    }
                    if (launched && (dodge ? conversation == 5 : conversation == 10)) {
                        advanced = true;
                        break;
                    }
                    if (launched && dodge && conversation >= 10)
                        throw std::runtime_error("dodge replay was hit before the miss branch");
                }
                check(max_attacking == 5, "all five pellets must launch together");
                check(advanced, dodge ? "dodging must advance Flowey conversation"
                                      : "taking a hit must advance Flowey conversation");
                for (int frame = 0; frame < 120; ++frame) {
                    regml::KeyboardState confirm{};
                    if (frame == 0)
                        confirm[90] = true;
                    game.tick(confirm, {});
                }
                bool followup = false;
                for (const auto &state : game.instance_states())
                    if (state.object_name == "obj_floweybattle1") {
                        const auto conversation = regml::value_number(state.variables.at("conversation"));
                        std::cout << (dodge ? "DODGE" : "HIT") << " follow-up conversation " << conversation << '\n';
                        followup = dodge ? conversation == 7 : conversation == 12;
                    }
                check(followup, "Flowey must start the follow-up dialogue after the attack");
                game.canvas().save_bmp(std::filesystem::path(argv[4]) / (dodge ? "dodge.bmp" : "hit.bmp"));
            }
            regml::GameSession rescue(argv[2], 1, std::filesystem::path(argv[4]) / "toriel");
            regml::KeyboardState held{};
            int player_id = -1;
            bool visited_battle = false, returned = false;
            double restored_x = 0, restored_y = 0;
            for (std::size_t frame = 0; frame < 10000; ++frame) {
                if (frame < 3500) {
                    if (const auto it = replay.find(frame); it != replay.end())
                        held = it->second;
                } else {
                    held = {};
                    if (frame % 100 == 0)
                        held[90] = true;
                }
                rescue.tick(held, {});
                if (rescue.room_name() == "room_floweybattle")
                    visited_battle = true;
                if (rescue.room_name() == "room_area1_2")
                    for (const auto &state : rescue.instance_states())
                        if (state.object_name == "obj_mainchara") {
                            if (!visited_battle)
                                player_id = state.id;
                            else {
                                check(state.id == player_id,
                                      "persistent room must restore the original player instance");
                                returned = true;
                                restored_x = regml::value_number(state.variables.at("x"));
                                restored_y = regml::value_number(state.variables.at("y"));
                            }
                        }
            }
            check(visited_battle && returned, "Toriel rescue must return to the overworld");
            bool moved = false;
            regml::KeyboardState up{};
            up[38] = true;
            for (int frame = 0; frame < 120; ++frame) {
                rescue.tick(up, {});
                for (const auto &state : rescue.instance_states())
                    if (state.object_name == "obj_mainchara" && state.id == player_id)
                        moved = moved || regml::value_number(state.variables.at("x")) != restored_x ||
                                regml::value_number(state.variables.at("y")) != restored_y;
            }
            check(moved, "player movement must unlock after Toriel leaves");
            check(rescue.room_name() == "room_ruins1", "player must be able to enter the Ruins after Toriel leaves");
            bool toriel_faced_path = false;
            regml::KeyboardState idle{};
            for (int frame = 0; frame < 180; ++frame) {
                rescue.tick(idle, {});
                for (const auto &state : rescue.instance_states())
                    if (state.object_name == "obj_toroverworld2" &&
                        regml::value_number(state.variables.at("path_index")) >= 0 &&
                        regml::value_number(state.variables.at("myinteract")) == 0) {
                        const auto direction = regml::value_number(state.variables.at("direction"));
                        if (direction >= 45 && direction <= 135 &&
                            regml::value_number(state.variables.at("sprite_index")) ==
                                regml::value_number(state.variables.at("usprite")))
                            toriel_faced_path = true;
                    }
            }
            check(toriel_faced_path, "Toriel must use her upward-facing sprite while walking up her Ruins path");
            rescue.canvas().save_bmp(std::filesystem::path(argv[4]) / "toriel-return.bmp");
            std::cout << "PASS Flowey five-pellet attack, dodge/hit progression, Toriel room restoration and movement "
                         "into the Ruins\n";
            return 0;
        } catch (const std::exception &error) {
            std::cerr << "FAIL Flowey regression: " << error.what() << '\n';
            return 1;
        }
    }
    if (argc > 1) {
        if (argc != 3 || std::string_view(argv[1]) != "--emit-fixture") {
            std::cerr << "Usage: regml_tests [--emit-fixture <output-path>]\n"
                         "       regml_tests --flowey-regression <data.win> <replay> <artifact-directory>\n";
            return 2;
        }
        const auto bytes = fixture().bytes;
        std::ofstream file(argv[2], std::ios::binary | std::ios::trunc);
        if (!file) {
            std::cerr << "Cannot write synthetic fixture\n";
            return 1;
        }
        file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        file.close();
        if (!file) {
            std::cerr << "Synthetic fixture write failed\n";
            return 1;
        }
        std::cout << "Wrote synthetic package: " << argv[2] << '\n';
        return 0;
    }
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"numeric comparisons tolerate fade rounding",
         [] {
             double alpha = 1;
             for (int step = 0; step < 10; ++step)
                 alpha -= 0.1;
             check(alpha != 0 && regml::compare_numbers(alpha, 0, 3), "fade remainder compares equal to zero");
             check(!regml::compare_numbers(alpha, 0, 4), "inequality agrees with equality");
             check(!regml::compare_numbers(alpha, 0, 6), "near-zero is not greater than zero");
             check(regml::compare_numbers(alpha, 0, 2) && regml::compare_numbers(alpha, 0, 5),
                   "inclusive comparisons accept near-zero");
             check(!regml::compare_numbers(-alpha, 0, 1), "negative rounding residue is not less than zero");
             check(!regml::compare_numbers(0.001, 0, 3), "distinct values remain unequal");
             check(!regml::compare_numbers(alpha, 0, 3, 0), "zero epsilon restores exact comparison");
             check(regml::compare_numbers(0.001, 0, 3, 0.01), "custom epsilon is applied");
             Bytes code;
             for (double number : {alpha, 0.0}) {
                 u32(code, 0xc0000000);
                 const auto bits = std::bit_cast<std::uint64_t>(number);
                 u32(code, static_cast<std::uint32_t>(bits));
                 u32(code, static_cast<std::uint32_t>(bits >> 32));
             }
             u32(code, 0x15000300);
             u32(code, 0x9c040000);
             const auto result = regml::execute_numeric(regml::decode(regml::BinaryView(code), 16));
             check(std::get<bool>(*result.returned), "bytecode comparison uses epsilon");
         }},
        {"array values retain nested data across copies and moves",
         [] {
             regml::Value original;
             {
                 regml::ArrayPtr child(new regml::ArrayValue);
                 child->elements[0] = std::string("retained");
                 child->row_lengths[0] = 1;
                 regml::ArrayPtr parent(new regml::ArrayValue);
                 parent->elements[32000] = child;
                 parent->row_lengths[1] = 1;
                 original = parent;
             }
             regml::Value copied = original;
             regml::Value moved = std::move(copied);
             original = std::monostate{};
             auto &parent = std::get<regml::ArrayPtr>(moved);
             auto &child = std::get<regml::ArrayPtr>(parent->elements.at(32000));
             check(regml::value_text(child->elements.at(0)) == "retained", "nested array survives owner release");
             regml::ArrayPtr detached(new regml::ArrayValue(*parent));
             detached->elements[32000] = std::int32_t{9};
             check(regml::value_text(child->elements.at(0)) == "retained", "detached copy preserves source");
             check(detached->row_lengths.at(1) == 1, "detached copy retains sparse row lengths");
             check(parent.use_count() == 1 && detached.use_count() == 1, "clones have independent ownership");
             moved = std::monostate{};
             check(regml::value_number(detached->elements.at(32000)) == 9, "clone survives source release");
         }},
        {"package variable chains and corruption",
         [] {
             auto f = fixture();
             const auto program = words({0x840f0007, 0x4522ffff, 0xa0000008, 0xc005ffff, 0xa0000001, 0x9d000000});
             std::copy(program.begin(), program.end(), f.bytes.begin() + static_cast<std::ptrdiff_t>(f.blob));
             const auto name = regml::BinaryView(f.bytes).u32(f.record);
             const auto vari = chunk(f.bytes, "VARI", 32);
             for (unsigned i = 0; i < 3; ++i)
                 u32(f.bytes, 0);
             u32(f.bytes, name);
             u32(f.bytes, 0xffffffffU);
             u32(f.bytes, 0);
             u32(f.bytes, 2);
             u32(f.bytes, static_cast<std::uint32_t>(f.blob + 4));
             finish(f.bytes);
             auto linked = regml::link_legacy_package(regml::Package::from_bytes(f.bytes));
             check(linked.variables.size() == 1 && linked.variable_at.size() == 2, "package chain linkage");
             check(linked.variable_at.at(f.blob + 12) == 0, "second occurrence");
             auto lazy_linked = regml::link_legacy_package(regml::Package::from_bytes(f.bytes), {}, false, false, true);
             check(lazy_linked.programs[0].instructions.empty(), "lazy linker releases decoded code after validation");
             check(lazy_linked.reference_symbols == std::vector<std::uint32_t>{0, 0} &&
                       lazy_linked.program_reference_ranges.size() == 1 &&
                       lazy_linked.program_reference_ranges[0].offset == 0 &&
                       lazy_linked.program_reference_ranges[0].count == 2,
                   "lazy linker stores compact per-program symbol bindings");
             const auto package = regml::Package::from_bytes(f.bytes);
             const auto &lazy_program = lazy_linked.program(package, 0);
             for (std::size_t i = 0; i < linked.programs[0].instructions.size(); ++i)
                 check(lazy_program.instructions[i].symbol_id == linked.programs[0].instructions[i].symbol_id,
                       "lazy decoding restores resolved symbols");
             lazy_linked.release_programs();
             check(lazy_linked.program(package, 0).instructions.size() == linked.programs[0].instructions.size(),
                   "lazy code reloads after room cache release");
             const auto runtime_linked =
                 regml::link_legacy_package(regml::Package::from_bytes(f.bytes), {}, false, false);
             check(runtime_linked.variable_at.empty() && runtime_linked.function_at.empty(),
                   "runtime omits audit maps");
             for (std::size_t i = 0; i < linked.programs[0].instructions.size(); ++i) {
                 check(runtime_linked.programs[0].instructions[i].symbol_id ==
                           linked.programs[0].instructions[i].symbol_id,
                       "runtime retains resolved symbols");
                 const auto offset = linked.programs[0].instructions[i].offset;
                 check(runtime_linked.programs[0].instruction_at(offset) == linked.programs[0].instruction_at(offset),
                       "compact program resolves instruction boundaries");
             }
             patch(f.bytes, f.blob + 8, 0xa0000000);
             error_is(regml::ErrorKind::malformed,
                      [&] { (void)regml::link_legacy_package(regml::Package::from_bytes(f.bytes)); });
             patch(f.bytes, f.blob + 8, 0xa0000008);
             patch(f.bytes, f.blob + 16, 0xa0000000);
             error_is(regml::ErrorKind::malformed,
                      [&] { (void)regml::link_legacy_package(regml::Package::from_bytes(f.bytes)); });
             patch(f.bytes, f.blob + 16, 0xa0000001);
             patch(f.bytes, vari + 28, static_cast<std::uint32_t>(f.blob + 8));
             error_is(regml::ErrorKind::malformed,
                      [&] { (void)regml::link_legacy_package(regml::Package::from_bytes(f.bytes)); });
         }},
        {"package function chains and missing references",
         [] {
             auto f = fixture();
             const auto program = words({0xd9050000, 1, 0x9e050000, 0x840f0007, 0x9c020000, 0x9d000000});
             std::copy(program.begin(), program.end(), f.bytes.begin() + static_cast<std::ptrdiff_t>(f.blob));
             error_is(regml::ErrorKind::malformed,
                      [&] { (void)regml::link_legacy_package(regml::Package::from_bytes(f.bytes)); });
             const auto name = regml::BinaryView(f.bytes).u32(f.record);
             (void)chunk(f.bytes, "FUNC", 20);
             u32(f.bytes, 1);
             u32(f.bytes, name);
             u32(f.bytes, 1);
             u32(f.bytes, static_cast<std::uint32_t>(f.blob));
             u32(f.bytes, 0);
             finish(f.bytes);
             const auto linked = regml::link_legacy_package(regml::Package::from_bytes(f.bytes));
             check(linked.functions.size() == 1 && linked.function_at.at(f.blob) == 0, "function linking");
             auto lazy_linked = regml::link_legacy_package(regml::Package::from_bytes(f.bytes), {}, false, false, true);
             check(lazy_linked.programs[0].instructions.empty(), "lazy function linker releases decoded code");
             check(lazy_linked.reference_symbols == std::vector<std::uint32_t>{0} &&
                       lazy_linked.program_reference_ranges[0].count == 1,
                   "lazy function linker stores its symbol in the compact table");
             const auto package = regml::Package::from_bytes(f.bytes);
             check(lazy_linked.program(package, 0).instructions[0].symbol_id == 0,
                   "lazy decode restores resolved function symbol");
             patch(f.bytes, f.blob + 4, 2);
             error_is(regml::ErrorKind::malformed,
                      [&] { (void)regml::link_legacy_package(regml::Package::from_bytes(f.bytes)); });
         }},
#if REGML_WII || REGML_LOW_MEMORY
        {"low-memory bytecode cache stays under its retained limit",
         [] {
             regml::LinkedPackage linked;
             linked.lazy_programs = true;
             regml::Program small;
             small.instructions.resize(100);
             linked.decoded_program_bytes = small.instructions.capacity() * sizeof(regml::Instruction);
             linked.programs.push_back(std::move(small));
             linked.release_programs();
             check(linked.programs[0].instructions.size() == 100, "small bytecode cache should remain hot");

             regml::Program large;
             large.instructions.resize(50000);
             linked.decoded_program_bytes += large.instructions.capacity() * sizeof(regml::Instruction);
             linked.programs.push_back(std::move(large));
             linked.release_programs();
             check(linked.programs[0].instructions.empty() && linked.programs[1].instructions.empty() &&
                       linked.decoded_program_bytes == 0,
                   "large bytecode cache should release all decoded programs");
         }},
#endif
        {"binary reads and absolute bounds",
         [] {
             const auto bytes = words({0x78563412, 0x3ff00000});
             const regml::BinaryView data(bytes, 100);
             check(data.u16(0) == 0x3412 && data.u32(0) == 0x78563412, "endianness");
             error_is(regml::ErrorKind::malformed, [&] { (void)data.slice(4, 4).u32(1); }, 105);
             error_is(regml::ErrorKind::malformed, [&] { (void)data.u64(std::numeric_limits<std::size_t>::max()); });
             check(data.slice(8, 0).text(0, 0).empty(), "empty end slice");
         }},
        {"v16 resource pointers and relative code address",
         [] {
             auto f = fixture();
             const auto package = regml::Package::from_bytes(std::move(f.bytes));
             check(package.chunks().size() == 3 && package.strings().size() == 2 && package.code().size() == 1,
                   "resource counts");
             check(package.string_at(0) == "reGML fixture" && package.code_name(0) == "gml_Script_test",
                   "string pointer convention");
             check(package.code()[0].bytecode_offset == f.blob && package.code()[0].locals == 2 &&
                       package.code()[0].locals_flag,
                   "code header");
             check(package.general_info()->width == 640, "GEN8 metadata");
             const auto code = package.code_bytes(0);
             check(integer_result(regml::execute_numeric(
                       regml::decode(regml::BinaryView(code, package.code()[0].bytecode_offset), 16))) == 42,
                   "loaded code execution");
             const auto copy = package;
             auto moved = copy;
             auto moved_again = std::move(moved);
             check(moved_again.code_name(0) == "gml_Script_test", "package ownership after copy/move");
         }},
        {"STRG pointer lookup handles unsorted records",
         [] {
             auto f = fixture();
             const auto first = regml::BinaryView(f.bytes).u32(f.strg + 4);
             const auto second = regml::BinaryView(f.bytes).u32(f.strg + 8);
             patch(f.bytes, f.strg + 4, second);
             patch(f.bytes, f.strg + 8, first);
             const auto package = regml::Package::from_bytes(std::move(f.bytes));
             check(package.string_pointer(first + 4) == "reGML fixture" &&
                       package.string_pointer(second + 4) == "gml_Script_test",
                   "sorted offset index preserves pointer resolution");
         }},
        {"v14 inventory is separate from instruction support",
         [] {
             const auto package = regml::Package::from_bytes(fixture(14).bytes);
             check(package.code().size() == 1, "v14 inline CODE inventory");
             error_is(regml::ErrorKind::unsupported, [&] {
                 const auto code = package.code_bytes(0);
                 (void)regml::decode(regml::BinaryView(code, package.code()[0].bytecode_offset), 14);
             });
         }},
        {"file-backed CODE headers parse without loading the whole chunk",
         [] {
             const auto f = fixture();
             const auto path = std::filesystem::temp_directory_path() / "regml-file-backed-test.data.win";
             {
                 std::ofstream output(path, std::ios::binary | std::ios::trunc);
                 output.write(reinterpret_cast<const char *>(f.bytes.data()),
                              static_cast<std::streamsize>(f.bytes.size()));
                 check(static_cast<bool>(output), "write file-backed package fixture");
             }
             {
                 const auto package = regml::Package::load(path);
                 check(package.code().size() == 1, "file-backed CODE inventory");
                 check(package.code_bytes(0).size() == regml::demo_bytecode().size(),
                       "file-backed code byte range");
             }
             std::filesystem::remove(path);
         }},
        {"truncated containers and trailing bytes",
         [] {
             const auto full = fixture().bytes;
             for (std::size_t size = 0; size < full.size(); ++size) {
                 Bytes truncated(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(size));
                 error_is(regml::ErrorKind::malformed, [&] { (void)regml::Package::from_bytes(truncated); });
             }
             auto extra = full;
             extra.push_back(0);
             error_is(regml::ErrorKind::malformed, [&] { (void)regml::Package::from_bytes(extra); }, 4);
         }},
        {"bad signature and oversized chunk",
         [] {
             auto f = fixture();
             f.bytes[0] = 'X';
             error_is(regml::ErrorKind::malformed, [&] { (void)regml::Package::from_bytes(f.bytes); }, 0);
             f = fixture();
             patch(f.bytes, f.code - 4, 0xffffffff);
             error_is(regml::ErrorKind::malformed, [&] { (void)regml::Package::from_bytes(f.bytes); });
         }},
        {"unknown chunks preserved and duplicates explicit",
         [] {
             auto f = fixture();
             (void)chunk(f.bytes, "TEST", 1);
             f.bytes.push_back(0x42);
             finish(f.bytes);
             auto package = regml::Package::from_bytes(f.bytes);
             check(package.find_chunk("TEST")->size == 1, "unknown chunk retention");
             (void)chunk(f.bytes, "TEST", 0);
             finish(f.bytes);
             error_is(regml::ErrorKind::unsupported, [&] { (void)regml::Package::from_bytes(f.bytes); });
         }},
        {"string lengths, terminators and pointers checked",
         [] {
             auto f = fixture();
             patch(f.bytes, f.string_record, 0xffffffff);
             error_is(regml::ErrorKind::malformed, [&] { (void)regml::Package::from_bytes(f.bytes); });
             f = fixture();
             f.bytes[f.string_record + 4 + 13] = 'x';
             error_is(regml::ErrorKind::malformed, [&] { (void)regml::Package::from_bytes(f.bytes); });
             f = fixture();
             patch(f.bytes, f.strg + 4, static_cast<std::uint32_t>(f.strg));
             error_is(regml::ErrorKind::malformed, [&] { (void)regml::Package::from_bytes(f.bytes); });
             f = fixture();
             patch(f.bytes, f.gen + 40, 1);
             error_is(regml::ErrorKind::malformed, [&] { (void)regml::Package::from_bytes(f.bytes); });
         }},
        {"code references and offsets checked",
         [] {
             auto f = fixture();
             patch(f.bytes, f.record + 12, 0x80000000);
             error_is(regml::ErrorKind::malformed, [&] { (void)regml::Package::from_bytes(f.bytes); });
             f = fixture();
             patch(f.bytes, f.record + 4, 23);
             error_is(regml::ErrorKind::malformed, [&] { (void)regml::Package::from_bytes(f.bytes); });
             f = fixture();
             patch(f.bytes, f.record + 16, 28);
             error_is(regml::ErrorKind::malformed, [&] { (void)regml::Package::from_bytes(f.bytes); });
         }},
        {"allocation limits enforced",
         [] {
             const auto f = fixture();
             regml::LoadLimits limits;
             limits.max_file_bytes = 8;
             error_is(regml::ErrorKind::unsupported, [&] { (void)regml::Package::from_bytes(f.bytes, limits); });
             limits = {};
             limits.max_chunks = 1;
             error_is(regml::ErrorKind::unsupported, [&] { (void)regml::Package::from_bytes(f.bytes, limits); });
             limits = {};
             limits.max_entries = 1;
             error_is(regml::ErrorKind::unsupported, [&] { (void)regml::Package::from_bytes(f.bytes, limits); });
             auto corrupt = f;
             patch(corrupt.bytes, corrupt.strg, 100);
             error_is(regml::ErrorKind::malformed, [&] { (void)regml::Package::from_bytes(corrupt.bytes); });
         }},
        {"missing metadata and unsupported version stay inventory only",
         [] {
             auto group = words({0x4d524f46, 8, 0x4f445541, 0});
             const auto package = regml::Package::from_bytes(group);
             check(!package.general_info(), "audio-only container");
             auto f = fixture();
             f.bytes[f.gen + 1] = 99;
             const auto newer = regml::Package::from_bytes(f.bytes);
             check(!newer.general_info()->has_known_prefix && newer.strings().empty(), "unknown profile isolation");
         }},
        {"decoder variable widths and signed immediates",
         [] {
             const auto bytes = words({0x840ffffe, 0xc0020000, 0x7fffffff, 0xc0000000, 0, 0x3ff00000, 0xc0030000,
                                       0xffffffff, 0xffffffff, 0x9d000000});
             const auto program = regml::decode(regml::BinaryView(bytes, 200), 16);
             check(program.instructions.size() == 5, "variable instruction widths");
             check(program.instructions[0].immediate_i32() == -2, "signed pushi");
             check(program.instructions[2].immediate_real() == 1.0, "double payload");
             check(program.instructions[3].immediate_i64() == -1, "int64 payload");
             check(program.instructions[4].offset == 36, "instruction boundary");
             const auto sentinel = regml::decode(regml::BinaryView(words({0xc0060000, 0xffffffff, 0x9d000000})), 16);
             check(sentinel.instructions[0].has_reference() &&
                       sentinel.instructions[0].reference_value() == 0xffffffffU,
                   "all-ones reference is still present");
         }},
        {"v17 float and break instructions decode and link",
         [] {
             const auto bytes = words({0xc0010000, 0x3fc00000, 0xff020000, 0xfffffffb, 0x9d000000});
             const auto decoded = regml::decode(regml::BinaryView(bytes), 17);
             check(decoded.instructions.size() == 3, "v17 instruction widths");
             check(decoded.instructions[0].type1 == regml::DataType::float32 &&
                       decoded.instructions[0].immediate_kind == regml::Instruction::ImmediateKind::real32 &&
                       decoded.instructions[0].immediate_real() == 1.5,
                   "v17 float immediate");
             check(decoded.instructions[1].opcode() == regml::Opcode::extended &&
                       decoded.instructions[1].immediate_i32() == -5,
                   "v17 break sub-opcode and payload");
             const auto float_program = regml::decode(
                 regml::BinaryView(words({0xc0010000, 0x3fc00000, 0xc0010000, 0x3e800000, 0x0c110000,
                                         0x9c010000})),
                 17);
             const auto float_result = regml::execute_numeric(float_program);
             check(float_result.returned && std::get<double>(*float_result.returned) == 1.75,
                   "numeric VM executes bytecode 17 float32 arithmetic");
             auto f = fixture(17);
             patch(f.bytes, f.gen + 44, 2);
             patch(f.bytes, f.gen + 48, 3);
             const auto package = regml::Package::from_bytes(f.bytes);
             const auto linked = regml::link_legacy_package(package);
             check(linked.programs[0].bytecode_version == 17, "v17 package symbol/link stage");
         }},
        {"truncated payload and unsupported opcode",
         [] {
             auto bytes = words({0xc0000000});
             error_is(regml::ErrorKind::malformed, [&] { (void)regml::decode(regml::BinaryView(bytes), 16); });
             bytes = words({0xfe000000});
             error_is(regml::ErrorKind::unsupported, [&] { (void)regml::decode(regml::BinaryView(bytes), 16); }, 0);
             error_is(regml::ErrorKind::unsupported, [&] { (void)regml::decode(regml::BinaryView(bytes), 99); });
             bytes = words({0x840f0001, 0x9c020000});
             error_is(
                 regml::ErrorKind::unsupported, [&] { (void)regml::decode(regml::BinaryView(bytes), 16, {1}); }, 4);
         }},
        {"branches validate ranges and immediate boundaries",
         [] {
             auto bytes = words({0xc0020000, 0, 0xb67fffff, 0x9d000000});
             error_is(regml::ErrorKind::malformed, [&] { (void)regml::decode(regml::BinaryView(bytes), 16); }, 8);
             bytes = words({0xb67fffff});
             error_is(regml::ErrorKind::malformed, [&] { (void)regml::decode(regml::BinaryView(bytes), 16); }, 0);
             bytes = words({0xb6000002});
             error_is(regml::ErrorKind::malformed, [&] { (void)regml::decode(regml::BinaryView(bytes), 16); });
             bytes = words({0xbbf00000});
             check(regml::decode(regml::BinaryView(bytes), 16).instructions[0].environment_exit(),
                   "popenv exit sentinel");
         }},
        {"numeric result and operand order",
         [] {
             auto result = run(regml::demo_bytecode());
             check(integer_result(result) == 42 && result.instructions_executed == 6 && result.peak_stack_values == 2,
                   "demo result");
             check(integer_result(run(words({0x840f0002, 0x840f0007, 0x0d220000, 0x9c020000}))) == -5,
                   "subtraction order");
             const auto real =
                 run(words({0xc0000000, 0, 0x40240000, 0xc0000000, 0, 0x40000000, 0x09000000, 0x9c000000}));
             check(std::get<double>(*real.returned) == 5.0, "real division order");
         }},
        {"backward loop and conditional branch",
         [] {
             const auto result = run(words({0x840f0003, 0x86020000, 0x840f0000, 0x15220600, 0xb8000004, 0x840f0001,
                                            0x0d220000, 0xb67ffffa, 0x9c020000}));
             check(integer_result(result) == 0 && result.instructions_executed == 27, "loop control flow");
         }},
        {"VM errors, unsupported behavior and execution limits",
         [] {
             error_is(regml::ErrorKind::vm, [&] { (void)run(words({0x9c020000})); }, 0);
             error_is(regml::ErrorKind::unsupported,
                      [&] { (void)run(words({0xc0020000, 0x7fffffff, 0x840f0001, 0x0c220000, 0x9c020000})); });
             error_is(regml::ErrorKind::unsupported, [&] { (void)run(words({0xc105fff9, 0})); });
             regml::VmOptions budget;
             budget.instruction_budget = 8;
             error_is(regml::ErrorKind::vm, [&] { (void)run(words({0xb6000000}), budget); }, 0);
             regml::VmOptions stack;
             stack.max_stack_values = 1;
             error_is(regml::ErrorKind::vm, [&] { (void)run(regml::demo_bytecode(), stack); }, 4);
         }},
        {"trace and independent execution state",
         [] {
             std::vector<std::size_t> depths;
             regml::VmOptions options;
             options.trace = [&](const regml::Instruction &, std::span<const regml::Value> stack) {
                 depths.push_back(stack.size());
             };
             (void)run(regml::demo_bytecode(), options);
             check(depths == std::vector<std::size_t>({0, 1, 2, 1, 2, 1}), "trace before instruction");
             check(integer_result(run(regml::demo_bytecode())) == 42, "fresh state");
         }},
        {"conversion, discard and boolean return",
         [] {
             const auto real = run(words({0x840f0007, 0x07020000, 0x9c000000}));
             check(std::get<double>(*real.returned) == 7.0, "int32 to double");
             const auto boolean = run(words({0x840f0002, 0x840f0003, 0x15220100, 0x9c040000}));
             check(std::get<bool>(*boolean.returned), "less-than comparison");
             check(integer_result(run(words({0x840f0001, 0x9e020000, 0x840f0009, 0x9c020000}))) == 9, "discard");
             const auto taken = run(words({0x840f0001, 0xb7000002, 0x840f0009, 0x840f0004, 0x9c020000}));
             check(integer_result(taken) == 4, "branch true");
         }},
        {"empty strings and zero-length code",
         [] {
             auto f = fixture();
             patch(f.bytes, f.string_record, 0);
             f.bytes[f.string_record + 4] = 0;
             auto package = regml::Package::from_bytes(f.bytes);
             check(package.string_at(0).empty(), "empty STRG record");
             f = fixture();
             patch(f.bytes, f.record + 4, 0);
             package = regml::Package::from_bytes(f.bytes);
             const auto code = package.code_bytes(0);
             const auto empty = regml::decode(regml::BinaryView(code, package.code()[0].bytecode_offset), 16);
             check(empty.instructions.empty() && !regml::execute_numeric(empty).returned, "zero-length code");
         }},
        {"resolved variable scopes and persistent state",
         [] {
             const auto bytes = words({0x840f000d, 0x4555fff9, 0, 0xc105fff9, 0, 0x9c050000});
             const auto program = regml::decode(regml::BinaryView(bytes), 16);
             regml::VmBindings bindings;
             bindings.variables.emplace(4, regml::VariableBinding{regml::VariableScope::local, "value"});
             bindings.variables.emplace(12, regml::VariableBinding{regml::VariableScope::local, "value"});
             regml::ExecutionContext context;
             context.bindings = &bindings;
             check(integer_result(regml::execute(program, context)) == 13, "local storage and ret.v");
             bindings.variables[4].scope = regml::VariableScope::self;
             bindings.variables[12].scope = regml::VariableScope::self;
             regml::VariableStore state;
             context.self = &state;
             check(integer_result(regml::execute(program, context)) == 13 &&
                       std::get<std::int32_t>(state.at("value")) == 13,
                   "persistent self storage");
             context.self = nullptr;
             error_is(regml::ErrorKind::vm, [&] { (void)regml::execute(program, context); }, 4);
             context.self = &state;
             bindings.variables[12].name = "absent";
             error_is(regml::ErrorKind::vm, [&] { (void)regml::execute(program, context); }, 12);
         }},
        {"variable stores share immutable key storage",
         [] {
             regml::VariableStore first, second;
             first["shared_variable_key"] = std::int32_t{1};
             second[std::string("shared_variable_key")] = std::int32_t{2};
             check(first.begin()->first.data() == second.begin()->first.data(),
                   "matching variable names should use the same interned storage");
             check(std::get<std::int32_t>(first.at("shared_variable_key")) == 1 &&
                       std::get<std::int32_t>(second.at("shared_variable_key")) == 2,
                   "interning must not merge values between stores");
         }},
        {"intrusive instance pointers retain and release without a control block",
         [] {
             struct Tracked {
                 std::size_t references = 0;
                 int *destroyed = nullptr;
                 ~Tracked() { ++*destroyed; }
             };
             int destroyed = 0;
             check(sizeof(regml::IntrusivePtr<Tracked>) == sizeof(Tracked *) &&
                       sizeof(regml::IntrusivePtr<Tracked>) < sizeof(std::shared_ptr<Tracked>),
                   "intrusive pointers are one word without shared ownership metadata");
             auto first = regml::IntrusivePtr<Tracked>(new Tracked{0, &destroyed});
             check(first->references == 1 && destroyed == 0, "initial intrusive ownership");
             {
                 auto second = first;
                 check(second->references == 2 && first == second, "copy increments one shared instance count");
                 auto moved = std::move(second);
                 check(!second && moved->references == 2, "move transfers an intrusive reference");
                 moved.reset();
                 check(first->references == 1 && destroyed == 0, "reset releases only its own reference");
             }
             first.reset();
             check(!first && destroyed == 1, "last intrusive reference destroys the instance");
         }},
        {"built-in calls preserve argument order and diagnostics",
         [] {
             const auto bytes = words({0x840f0007, 0x840f0002, 0xd9050002, 0, 0x9c050000});
             const auto program = regml::decode(regml::BinaryView(bytes, 100), 16);
             regml::VmBindings bindings;
             bindings.functions.emplace(8, "subtract");
             regml::ExecutionContext context;
             context.bindings = &bindings;
             context.call_builtin = [](std::string_view name, std::span<const regml::Value> args) -> regml::Value {
                 check(name == "subtract" && args.size() == 2, "call binding/arity");
                 return std::get<std::int32_t>(args[0]) - std::get<std::int32_t>(args[1]);
             };
             check(integer_result(regml::execute(program, context)) == -5, "first argument popped first");
             context.call_builtin = [](std::string_view, std::span<const regml::Value>) -> regml::Value {
                 throw regml::Error(regml::ErrorKind::unsupported, "not implemented");
             };
             error_is(regml::ErrorKind::unsupported, [&] { (void)regml::execute(program, context); }, 108);
         }},
        {"string constants and dynamic scalar addition",
         [] {
             const auto bytes = words({0xc0060000, 0, 0xc0060000, 1, 0x0c550000, 0x9c050000});
             const auto program = regml::decode(regml::BinaryView(bytes), 16);
             regml::VmBindings bindings;
             bindings.strings.emplace(0, "hello ");
             bindings.strings.emplace(1, "world");
             regml::ExecutionContext context;
             context.bindings = &bindings;
             check(std::get<std::string>(*regml::execute(program, context).returned) == "hello world",
                   "dynamic strings");
             check(regml::value_text(std::monostate{}) == "undefined", "undefined return display");
             const auto number =
                 run(words({0x840f0002, 0x07520000, 0xc0000000, 0, 0x3ff80000, 0x0c550000, 0x9c050000}));
             check(std::get<double>(*number.returned) == 3.5, "dynamic numeric operands");
         }},
        {"framebuffer clipping, alpha and glyph pixels",
         [] {
             regml::Canvas canvas(16, 16);
             canvas.clear(0);
             canvas.color(0xffffff);
             canvas.rectangle(1, 1, 3, 3, false);
             check(canvas.pixel(1, 1) == 0xffffffff && canvas.pixel(3, 3) == 0xff000000, "half-open rectangle");
             canvas.clear(0);
             canvas.rectangle(-100, -100, 100, 100, true);
             check(canvas.pixel(0, 0) == 0xff000000, "clipping does not invent outline edges");
             canvas.clear(0xff0000);
             canvas.color(0x0000ff);
             canvas.alpha(0.5);
             canvas.rectangle(0, 0, 1, 1, false);
             check(canvas.pixel(0, 0) == 0xff80007f, "source alpha and GML BGR color");
             canvas.clear(0);
             canvas.color(0xffffff);
             canvas.alpha(1);
             canvas.text(0, 0, "A");
             check(canvas.pixel(1, 0) == 0xffffffff && canvas.pixel(0, 0) == 0xff000000, "bitmap text");
             regml::Canvas debug_canvas(80, 48);
             debug_canvas.clear(0);
             const std::array<std::string_view, 1> debug_lines{"FPS 60"};
             debug_canvas.debug_text_overlay(debug_lines);
             check(debug_canvas.pixel(6, 6) == 0xffffffff, "debug text overlay draws over the framebuffer");
             canvas.clear(0);
             canvas.circle(8, 8, 3, false);
             check(canvas.pixel(8, 8) == 0xffffffff && canvas.pixel(12, 8) == 0xff000000, "circle coverage");
             regml::Canvas surface(2, 2), composite(4, 4);
             surface.clear_transparent();
             surface.rgba_pixel(0, 0, 0x80ffffff, 0xffffff, 1);
             surface.rgba_pixel(1, 0, 0xffff0000, 0xffffff, 1);
             composite.clear_transparent();
             composite.draw_surface(surface, 1, 1);
             check(composite.pixel(1, 1) == 0x80ffffff && composite.pixel(2, 1) == 0xffff0000 &&
                       composite.pixel(1, 2) == 0,
                   "surface keeps transparency while compositing");
             composite.clear(0);
             composite.draw_surface(surface, -1, 1);
             check(composite.pixel(0, 1) == 0xffff0000, "fast surface path clips and blends correctly");
             const auto unchanged = composite.hash();
             composite.draw_surface(surface, 1, 1, -4, 4, 37, 0x3366cc, 0);
             composite.draw_surface(surface, 1, 1, 2, -2, 0, 0xffffff, -0.2);
             check(composite.hash() == unchanged, "invisible surface draws preserve the destination");
             error_is(regml::ErrorKind::vm, [&] { canvas.alpha(std::numeric_limits<double>::quiet_NaN()); });
             error_is(regml::ErrorKind::unsupported, [&] { regml::Canvas invalid(0, 16); });
         }},
        {"rectangle spans preserve clipped edges and alpha",
         [] {
             const std::array<std::array<double, 4>, 9> rectangles = {{{0, 0, 24, 19},
                                                                       {-5, -4, 12, 8},
                                                                       {5.3, 4.8, 20.2, 16.1},
                                                                       {18, 15, 3, 2},
                                                                       {-100, -100, 100, 100},
                                                                       {4, 3, 4, 12},
                                                                       {4, 3, 5, 4},
                                                                       {4, 3, 6, 5},
                                                                       {-1e100, 2, 1e100, 18}}};
             for (const auto &rectangle : rectangles)
                 for (const bool outline : {false, true})
                     for (const bool transparent : {false, true})
                         for (const double opacity : {0.0, 0.37, 1.0}) {
                             regml::Canvas fast(24, 19), reference(24, 19);
                             if (transparent) {
                                 fast.clear_transparent();
                                 reference.clear_transparent();
                             } else {
                                 fast.clear(0x342516);
                                 reference.clear(0x342516);
                             }
                             fast.color(0x397ac5);
                             fast.alpha(opacity);
                             fast.rectangle(rectangle[0], rectangle[1], rectangle[2], rectangle[3], outline);
                             const double left = std::floor(std::min(rectangle[0], rectangle[2])),
                                          right = std::ceil(std::max(rectangle[0], rectangle[2])),
                                          top = std::floor(std::min(rectangle[1], rectangle[3])),
                                          bottom = std::ceil(std::max(rectangle[1], rectangle[3]));
                             const unsigned alpha = static_cast<unsigned>(std::lround(opacity * 255));
                             for (int y = 0; y < 19; ++y)
                                 for (int x = 0; x < 24; ++x) {
                                     if (x < left || x >= right || y < top || y >= bottom ||
                                         (outline && x != left && x != right - 1 && y != top && y != bottom - 1))
                                         continue;
                                     auto &pixel = reference.pixels()[static_cast<std::size_t>(y) * 24 + x];
                                     unsigned result = 0xff000000;
                                     for (const unsigned shift : {0U, 8U, 16U})
                                         result |= (((0xc57a39U >> shift & 255) * alpha +
                                                     (pixel >> shift & 255) * (255 - alpha) + 127) /
                                                    255)
                                                   << shift;
                                     pixel = result;
                                 }
                             check(fast.hash() == reference.hash(), "rectangle span changed coverage or blending");
                         }
         }},
        {"pixel spans preserve clipping and source-over compositing",
         [] {
             for (const auto pixel : {0x00112233U, 0x017f2345U, 0x7fabcd12U, 0xff123456U})
                 for (unsigned tint : {0xffffffU, 0x3366ccU})
                     for (double opacity : {0.0, 0.37, 1.0, 2.0})
                         for (int x : {-7, -1, 0, 4, 7}) {
                             regml::Canvas actual(7, 2), expected(7, 2);
                             actual.clear_transparent();
                             expected.clear_transparent();
                             for (int dx = 0; dx < 7; ++dx) {
                                 const unsigned alpha = dx % 2 ? 255 : 96;
                                 actual.rgba_pixel_tinted(dx, 1, (alpha << 24) | 0xabc123, 0xffffff, 1);
                                 expected.rgba_pixel_tinted(dx, 1, (alpha << 24) | 0xabc123, 0xffffff, 1);
                             }
                             actual.rgba_span(x, 1, 6, pixel, tint, opacity);
                             for (int dx = x; dx < x + 6; ++dx)
                                 expected.rgba_pixel_tinted(dx, 1, pixel, tint, opacity);
                             check(actual.hash() == expected.hash(), "span changed pixel compositing or clipping");
                         }
         }},
        {"sprite row blit matches per-pixel compositing",
         [] {
             const std::array<std::uint32_t, 12> source = {0x00000000, 0x01abcdef, 0x7f123456, 0xffabcdef,
                                                           0x80112233, 0xff010203, 0x4000ff00, 0x00ffffff,
                                                           0xff804020, 0x7fffffff, 0xff000000, 0x20f0e0d0};
             for (const auto [tint, opacity] :
                  {std::pair{0xffffffU, 1.0}, std::pair{0x3366ccU, 1.0}, std::pair{0xffffffU, 0.37},
                   std::pair{0x3366ccU, 0.0}, std::pair{0xffffffU, -0.2}}) {
                 regml::Canvas fast(5, 4), reference(5, 4);
                 fast.clear(0x342516);
                 reference.clear(0x342516);
                 fast.blit_rgba(source, 4, 1, 0, 0, 1, 3, 3, tint, opacity);
                 for (int y = 0; y < 3; ++y)
                     for (int x = 0; x < 3; ++x)
                         reference.rgba_pixel_tinted(x, y + 1, source[static_cast<std::size_t>(y) * 4 + x + 1], tint,
                                                     opacity);
                 check(fast.hash() == reference.hash(), "specialized blit must preserve pixel compositing");
             }
             const std::array<std::uint32_t, 6> opaque = {0xff123456, 0xffabcdef, 0xff000000,
                                                          0xfffedcba, 0xff654321, 0xffffffff};
             regml::Canvas fast(3, 2), reference(3, 2);
             fast.clear_transparent();
             reference.clear_transparent();
             fast.blit_rgba(opaque, 3, 0, 0, 0, 0, 3, 2, 0xffffff, 1, 2);
             for (int y = 0; y < 2; ++y)
                 for (int x = 0; x < 3; ++x)
                     reference.rgba_pixel_tinted(x, y, opaque[static_cast<std::size_t>(y) * 3 + x], 0xffffff, 1);
             check(fast.hash() == reference.hash(), "opaque row copy must match alpha compositing");
             const std::array<std::uint32_t, 8> binary = {0x00000000, 0xff102030, 0xff405060, 0x00000000,
                                                          0xff708090, 0x00000000, 0x00000000, 0xffa0b0c0};
             regml::Canvas binary_fast(4, 2), binary_reference(4, 2);
             binary_fast.clear(0x203040);
             binary_reference.clear(0x203040);
             binary_fast.blit_rgba(binary, 4, 0, 0, 0, 0, 4, 2, 0xffffff, 1, 1);
             for (int y = 0; y < 2; ++y)
                 for (int x = 0; x < 4; ++x)
                     binary_reference.rgba_pixel_tinted(x, y, binary[static_cast<std::size_t>(y) * 4 + x], 0xffffff, 1);
             check(binary_fast.hash() == binary_reference.hash(), "binary-alpha runs must match compositing");
         }},
        {"playable VM fixture controls, bounds and scoring",
         [] {
             regml::DemoSession demo;
             regml::KeyboardState held{};
             const auto initial_hash = demo.canvas().hash();
             held[39] = true;
             demo.tick(held);
             demo.draw();
             check(regml::value_number(demo.state().at("x")) == 324, "right input changes VM variable");
             held[32] = true;
             demo.tick(held);
             check(regml::value_number(demo.state().at("x")) == 326, "slow input");
             held = {};
             held[37] = true;
             for (int i = 0; i < 100; ++i)
                 demo.tick(held);
             check(regml::value_number(demo.state().at("x")) == 48, "left arena bound");
             demo.draw();
             check(demo.canvas().hash() != initial_hash && demo.vm_instructions() > 1000, "VM draw output");
             regml::DemoSession collect;
             held = {};
             held[65] = true;
             for (int i = 0; i < 50; ++i)
                 collect.tick(held);
             held = {};
             held[87] = true;
             for (int i = 0; i < 50; ++i)
                 collect.tick(held);
             check(regml::value_number(collect.state().at("score")) >= 1, "bytecode pickup and score");
         }},
        {"main screen render scale preserves logical coordinates",
         [] {
             regml::Canvas screen(640, 480, true);
             regml::Canvas surface(8, 8);
             const int divisor = REGML_240P ? 2 : 1;
             check(screen.width() == 640 / divisor && screen.height() == 480 / divisor, "main render dimensions");
             check(screen.logical_width() == 640 && screen.logical_height() == 480, "logical screen dimensions");
             check(surface.width() == 8 && surface.height() == 8, "surfaces retain requested resolution");
             screen.clear(0);
             screen.color(0xffffff);
             screen.rectangle(100, 100, 120, 120, false);
             check(screen.pixel(100 / divisor, 100 / divisor) == 0xffffffff, "primitive coordinates scale once");
             check(screen.pixel(120 / divisor, 100 / divisor) == 0xff000000, "scaled rectangle far edge");
             surface.clear(0);
             surface.color(0xffffff);
             surface.rectangle(0, 0, 8, 8, false);
             screen.draw_surface(surface, 200, 200);
             check(screen.pixel(200 / divisor, 200 / divisor) == 0xffffffff, "surface placement scales once");
             check(screen.pixel(208 / divisor, 200 / divisor) == 0xff000000, "surface draw retains logical size");
             surface.clear_transparent();
             for (int y = 0; y < 8; ++y)
                 surface.rgba_span(0, y, 8, 0x80ffffff, 0xffffff, 1);
             screen.draw_surface(surface, 240, 200);
             check(screen.pixel(240 / divisor, 200 / divisor) == 0xff808080,
                   "downscaled surfaces sample once instead of blending overlapping source pixels");
         }},
        {"fixture collision, game over and pressed-edge restart", [] {
             regml::DemoSession demo;
             regml::KeyboardState held{};
             held[38] = true;
             for (int i = 0; i < 32; ++i)
                 demo.tick(held);
             held = {};
             for (int i = 0; i < 700; ++i)
                 demo.tick(held);
             check(regml::value_number(demo.state().at("lives")) == 0 &&
                       regml::value_number(demo.state().at("game_over")) == 1,
                   "collision and game-over progression");
             regml::KeyboardState pressed{};
             pressed[82] = true;
             demo.tick(held, pressed);
             check(regml::value_number(demo.state().at("lives")) == 3 &&
                       regml::value_number(demo.state().at("y")) == 380,
                   "short key tap restarts");
             held[82] = true;
             demo.tick(held);
             demo.tick(held);
             check(regml::value_number(demo.state().at("timer")) == 1, "held restart does not repeat");
         }}};
    int failed = 0;
    for (const auto &[name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception &error) {
            ++failed;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() - static_cast<std::size_t>(failed) << '/' << tests.size() << " test groups passed\n";
    return failed == 0 ? 0 : 1;
}
