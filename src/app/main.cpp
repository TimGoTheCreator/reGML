#include "regml/bytecode/linker.hpp"
#include "regml/formats/package.hpp"
#include "regml/platform/play.hpp"
#include "regml/vm/interpreter.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace {
std::string escaped(std::string_view text, std::size_t limit = 160) {
    std::ostringstream out;
    out << '"';
    const auto length = std::min(text.size(), limit);
    for (std::size_t i = 0; i < length; ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (byte == '"' || byte == '\\')
            out << '\\' << static_cast<char>(byte);
        else if (byte < 32 || byte >= 127)
            out << "\\x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte) << std::dec;
        else
            out << static_cast<char>(byte);
    }
    if (text.size() > limit)
        out << "...";
    out << '"';
    return out.str();
}
void help() {
    std::cout << "reGML 0.2.0 - GameMaker VM runner foundation\n"
                 "Usage:\n"
                 "  regml inspect <data.win> [--strings] [--code]\n"
                 "  regml disasm <data.win> <code-index> [--linked]\n"
                 "  regml audit <data.win>\n"
                 "  regml vm-demo [--trace]\n"
                 "  regml play-demo [--frames N] [--headless | --hidden] [--autoplay]\n"
                 "  regml run <data.win> [--frames N] [--headless | --hidden]\n"
                 "                  [--screenshot output.bmp] [--input-script file]\n"
                 "                  [--seed N] [--save-dir directory] [--speed Nx]\n"
                 "                  Speed affects real-time playback; hidden/headless runs are unthrottled.\n"
                 "  regml --help | --version\n\n"
                 "Inspection is read-only. Resource tables: v14-v17 prefix/layout.\n"
                 "Instruction decoding: v15/v16. VM demo: numeric scalar subset.\n"
                 "Native legacy-game execution is experimental. Undertale intro,\n"
                 "naming and first-room movement have been exercised; full progression\n"
                 "is not verified. F10 closes the native game window.\n";
}
void print_instruction(const regml::Instruction &ins, std::size_t origin, bool resolved = false) {
    std::cout << "  0x" << std::hex << std::setw(8) << std::setfill('0') << origin + ins.offset << "  " << std::setw(8)
              << ins.word << std::setfill(' ') << std::dec << "  " << regml::opcode_name(ins.opcode);
    if (ins.branch_target)
        std::cout << " -> 0x" << std::hex << origin + *ins.branch_target << std::dec;
    else if (ins.environment_exit)
        std::cout << " <environment-exit>";
    else {
        std::cout << '.' << regml::type_name(ins.type1);
        if (ins.opcode >= regml::Opcode::conv && ins.opcode <= regml::Opcode::cmp)
            std::cout << '.' << regml::type_name(ins.type2);
        if (const auto *integer = std::get_if<std::int32_t>(&ins.immediate))
            std::cout << ' ' << *integer;
        if (const auto *integer = std::get_if<std::int64_t>(&ins.immediate))
            std::cout << ' ' << *integer;
        if (const auto *real = std::get_if<double>(&ins.immediate))
            std::cout << ' ' << regml::value_text(*real);
        if (ins.reference)
            std::cout << " reference=0x" << std::hex << *ins.reference << std::dec << (resolved ? "" : " (unresolved)");
        if (ins.opcode == regml::Opcode::cmp)
            std::cout << " comparison=" << static_cast<unsigned>(ins.comparison);
        if (ins.opcode == regml::Opcode::extended)
            std::cout << " subopcode=" << std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(ins.word));
    }
    std::cout << '\n';
}
void inspect(const regml::Package &package, bool strings, bool code) {
    std::cout << "reGML package inspection\nSize: " << package.size() << " bytes\n";
    if (const auto &info = package.general_info()) {
        std::cout << "Reported bytecode version: " << static_cast<unsigned>(info->bytecode_version) << '\n';
        if (info->has_known_prefix) {
            const auto &v = info->reported_version;
            std::cout << "Reported engine version: " << v[0] << '.' << v[1] << '.' << v[2] << '.' << v[3]
                      << " (not a complete format profile)\n"
                      << "Name: " << escaped(package.string_pointer(info->name_pointer)) << '\n'
                      << "Display name: " << escaped(package.string_pointer(info->display_name_pointer)) << '\n'
                      << "Window: " << info->width << 'x' << info->height << '\n';
        } else
            std::cout << "Metadata/resource layout: unsupported; chunk inventory only\n";
    } else
        std::cout << "GEN8: absent; chunk inventory only (may be an audio group)\n";
    std::cout << "Chunks: " << package.chunks().size() << '\n';
    for (const auto &chunk : package.chunks())
        std::cout << "  " << chunk.name() << "  offset=0x" << std::hex << chunk.data_offset << std::dec
                  << "  bytes=" << chunk.size << '\n';
    std::cout << "Parsed strings: " << package.strings().size() << "\nParsed code entries: " << package.code().size()
              << '\n';
    if (!package.find_chunk("CODE"))
        std::cout << "CODE: absent; executable VM content is not established\n";
    constexpr std::size_t listing_limit = 100;
    if (strings) {
        for (std::size_t i = 0; i < std::min(package.strings().size(), listing_limit); ++i)
            std::cout << "  string[" << i << "] " << escaped(package.string_at(i)) << '\n';
        if (package.strings().size() > listing_limit)
            std::cout << "  (string listing limited to 100 entries)\n";
    }
    if (code) {
        for (std::size_t i = 0; i < std::min(package.code().size(), listing_limit); ++i) {
            const auto &entry = package.code()[i];
            std::cout << "  code[" << i << "] " << escaped(package.code_name(i)) << " bytes=" << entry.bytecode_length
                      << " locals=" << entry.locals << " args=" << entry.arguments
                      << " entry-offset=" << entry.entry_offset << '\n';
        }
        if (package.code().size() > listing_limit)
            std::cout << "  (code listing limited to 100 entries)\n";
    }
    std::cout << "Inspection does not execute the game; use run for experimental execution.\n";
}
std::size_t parse_index(std::string_view input) {
    std::size_t result = 0;
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), result);
    if (input.empty() || parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size())
        throw regml::Error(regml::ErrorKind::usage, "code index must be a nonnegative decimal integer");
    return result;
}
double parse_speed(std::string_view input) {
    if (!input.empty() && (input.back() == 'x' || input.back() == 'X'))
        input.remove_suffix(1);
    double result = 0.0;
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), result);
    if (input.empty() || parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size() ||
        !std::isfinite(result) || result < 0.01 || result > 1000.0)
        throw regml::Error(regml::ErrorKind::usage, "--speed must be a number from 0.01x to 1000x");
    return result;
}
int command(int argc, char **argv) {
    if (argc == 1) {
        help();
        return 0;
    }
    const std::string_view name = argv[1];
    if (name == "--help" || name == "--version") {
        if (argc != 2)
            throw regml::Error(regml::ErrorKind::usage, "unexpected arguments");
        if (name == "--help")
            help();
        else
            std::cout << "reGML 0.2.0\n";
        return 0;
    }
    if (name == "inspect") {
        if (argc < 3)
            throw regml::Error(regml::ErrorKind::usage, "inspect requires a package path");
        bool strings = false, code = false;
        for (int i = 3; i < argc; ++i) {
            const std::string_view flag = argv[i];
            if (flag == "--strings")
                strings = true;
            else if (flag == "--code")
                code = true;
            else
                throw regml::Error(regml::ErrorKind::usage, "unknown inspect option: " + std::string(flag));
        }
        inspect(regml::Package::load(argv[2]), strings, code);
        return 0;
    }
    if (name == "audit") {
        if (argc != 3)
            throw regml::Error(regml::ErrorKind::usage, "audit requires a package path");
        const auto package = regml::Package::load(argv[2]);
        const auto linked = regml::link_legacy_package(package);
        std::cout << "Legacy package decoding and symbol audit\nCode entries: " << linked.programs.size()
                  << "\nInstructions: " << linked.instruction_count << "\nVariables: " << linked.variables.size()
                  << "\nFunctions: " << linked.functions.size()
                  << "\nLinked variable occurrences: " << linked.variable_at.size()
                  << "\nLinked function occurrences: " << linked.function_at.size()
                  << "\nReference chains verified. Runtime compatibility is not established.\n";
        return 0;
    }
    if (name == "disasm") {
        const bool resolve = argc == 5 && std::string_view(argv[4]) == "--linked";
        if (argc != 4 && !resolve)
            throw regml::Error(regml::ErrorKind::usage,
                               "disasm requires a package path and code index; optional --linked");
        const auto index = parse_index(argv[3]);
        const auto package = regml::Package::load(argv[2]);
        if (!package.general_info() || !package.general_info()->has_known_prefix)
            throw regml::Error(regml::ErrorKind::unsupported, "no supported metadata profile for disassembly");
        const auto program = regml::decode(package.code_bytes(index), package.general_info()->bytecode_version);
        std::optional<regml::LinkedPackage> linked;
        if (resolve)
            linked = regml::link_legacy_package(package);
        std::cout << "Code " << index << ": " << escaped(package.code_name(index)) << "\nExecution is not implied.\n";
        for (const auto &ins : program.instructions) {
            print_instruction(ins, program.origin, resolve);
            if (linked) {
                const auto address = program.origin + ins.offset;
                if (const auto it = linked->variable_at.find(address); it != linked->variable_at.end())
                    std::cout << "    variable: " << escaped(linked->variables[it->second].name) << " reference-kind=0x"
                              << std::hex << ((*ins.reference >> 24) & 0xf8) << std::dec << '\n';
                if (const auto it = linked->function_at.find(address); it != linked->function_at.end())
                    std::cout << "    function: " << escaped(linked->functions[it->second].name) << '\n';
            }
        }
        return 0;
    }
    if (name == "vm-demo") {
        const bool trace = argc == 3 && std::string_view(argv[2]) == "--trace";
        if (argc > 3 || (argc == 3 && !trace))
            throw regml::Error(regml::ErrorKind::usage, "vm-demo accepts only --trace");
        const auto bytes = regml::demo_bytecode();
        const auto program = regml::decode(regml::BinaryView(bytes), 16);
        regml::VmOptions options;
        if (trace)
            options.trace = [&](const regml::Instruction &ins, std::span<const regml::Value> stack) {
                print_instruction(ins, program.origin);
                std::cout << "    stack before:";
                for (const auto &value : stack)
                    std::cout << ' ' << regml::value_text(value);
                std::cout << '\n';
            };
        const auto result = regml::execute_numeric(program, options);
        std::cout << "reGML numeric VM demo: (7 - 5) * 21\nResult: "
                  << (result.returned ? regml::value_text(*result.returned) : "undefined")
                  << "\nInstructions: " << result.instructions_executed << "\nPeak stack: " << result.peak_stack_values
                  << '\n';
        return 0;
    }
    if (name == "play-demo" || name == "run") {
        if (name == "run" && argc < 3)
            throw regml::Error(regml::ErrorKind::usage, "run requires a data.win path");
        regml::PlayOptions options;
        for (int i = name == "run" ? 3 : 2; i < argc; ++i) {
            const std::string_view flag = argv[i];
            if (flag == "--headless")
                options.headless = true;
            else if (flag == "--hidden")
                options.hidden = true;
            else if (flag == "--autoplay")
                options.autoplay = true;
            else if (flag == "--frames" || flag == "--screenshot" || flag == "--input-script" || flag == "--seed" ||
                     flag == "--save-dir" || flag == "--speed") {
                if (++i >= argc)
                    throw regml::Error(regml::ErrorKind::usage, "missing value for " + std::string(flag));
                if (flag == "--frames") {
                    options.frame_limit = parse_index(argv[i]);
                    if (options.frame_limit == 0)
                        throw regml::Error(regml::ErrorKind::usage, "--frames must be positive");
                } else if (flag == "--screenshot")
                    options.screenshot = argv[i];
                else if (flag == "--save-dir")
                    options.save_directory = argv[i];
                else if (flag == "--seed") {
                    const auto seed = parse_index(argv[i]);
                    if (seed > UINT32_MAX)
                        throw regml::Error(regml::ErrorKind::usage, "seed exceeds uint32 range");
                    options.seed = static_cast<std::uint32_t>(seed);
                } else if (flag == "--speed")
                    options.speed_multiplier = parse_speed(argv[i]);
                else
                    options.input_script = argv[i];
            } else
                throw regml::Error(regml::ErrorKind::usage, "unknown run option: " + std::string(flag));
        }
        if (options.headless && options.hidden)
            throw regml::Error(regml::ErrorKind::usage, "choose either --headless or --hidden");
        const auto report = name == "run" ? regml::play_game(argv[2], options) : regml::play_demo(options);
        std::cout << (name == "run" ? "reGML package execution" : "reGML VM Arcade") << "\nFrames: " << report.frames
                  << "\nVM instructions: " << report.instructions;
        if (name == "play-demo")
            std::cout << "\nPlayer: " << report.x << ", " << report.y << "\nScore: " << report.score
                      << "\nLives: " << report.lives;
        if (name == "run")
            std::cout << "\nRoom: " << report.room;
        std::cout << "\nFrame hash: " << std::hex << report.frame_hash << std::dec << '\n';
        if (!options.screenshot.empty())
            std::cout << "Screenshot: " << options.screenshot.string() << '\n';
        return 0;
    }
    throw regml::Error(regml::ErrorKind::usage, "unknown command: " + std::string(name));
}
} // namespace
int main(int argc, char **argv) {
#if REGML_WIIU
    // Wii U homebrew launches don't have a useful shell for passing CLI args.
    // Keep the package on the SD card beside this fixed homebrew path.
    const std::filesystem::path package = argc > 1 ? argv[1] : "/vol/external01/reGML/data.win";
    regml::PlayOptions options;
    options.save_directory = "/vol/external01/wiiu/apps/reGML/saves";
    const auto game_directory = package.parent_path();
    if (std::filesystem::exists(game_directory / "regml-input.txt"))
        options.input_script = game_directory / "regml-input.txt";
    if (std::filesystem::exists(game_directory / "regml-trace.enabled"))
        options.trace_log = game_directory / "regml-trace.log";
    try {
        const auto report = regml::play_game(package, options);
        std::cout << "reGML Wii U\nFrames: " << report.frames << "\nVM instructions: " << report.instructions
                  << "\nRoom: " << report.room << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "reGML Wii U error: " << error.what() << '\n';
        return 1;
    }
#else
    try {
        return command(argc, argv);
    } catch (const regml::Error &error) {
        std::cerr << "reGML " << regml::error_kind_name(error.kind()) << " error";
        if (error.offset())
            std::cerr << " at 0x" << std::hex << *error.offset() << std::dec;
        std::cerr << ": " << error.what() << '\n';
        if (error.kind() == regml::ErrorKind::usage) {
            std::cerr << "Use regml --help.\n";
            return 2;
        }
        if (error.kind() == regml::ErrorKind::unsupported)
            return 3;
        if (error.kind() == regml::ErrorKind::vm)
            return 4;
        return 1;
    } catch (const std::exception &error) {
        std::cerr << "reGML host error: " << error.what() << '\n';
        return 1;
    }
#endif
}
