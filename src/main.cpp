#include "gmrt_vm.hpp"
#include "gmrt_wad.hpp"
#include "interpreter.hpp"
#include "linker.hpp"
#include "lookup.hpp"
#include "package.hpp"
#include "play.hpp"
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
            out << "\\x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte)
                << std::dec;
        else
            out << static_cast<char>(byte);
    }
    if (text.size() > limit)
        out << "...";
    out << '"';
    return out.str();
}
void help() {
    std::cout << "reGML " << regml::version
              << " - GameMaker VM runner\n"
                 "Usage:\n"
                 "  regml inspect <data.win> [--strings] [--code]\n"
                 "  regml disasm <data.win> <code-index> [--linked]\n"
                 "  regml audit <data.win>\n"
                 "  regml gmrt inspect <game.wad>\n"
                 "  regml gmrt extract <game.wad> <output-directory>\n"
                 "  regml gmrt run-function <game.wad> <module-index> <function-index> [--trace]\n"
                 "  regml gmrt run-ir-function <module.gmir> <function-index> [--trace]\n"
                 "  regml vm-demo [--trace]\n"
                 "  regml run <data.win|game.wad> [--frames N] [--headless | --hidden]\n"
                 "                  [--screenshot output.bmp] [--input-script file]\n"
                 "                  [--seed N] [--save-dir directory] [--speed Nx] [--speed-control]\n"
                 "                  [--debug-overlay]\n"
                 "                  [--profile-log file]\n"
                 "                  --speed-control enables [ / ] to halve or double speed during play.\n"
                 "                  Speed affects real-time playback; hidden/headless runs are unthrottled.\n"
                 "  regml --help | --version\n\n"
                 "Inspection is read-only. Resource tables: v14-v17 prefix/layout.\n"
                 "Instruction decoding: v15/v16/v17. VM demo: numeric scalar subset.\n"
                 "GMRT support: WAD/GMIR parsing, cross-module calls, and a boot path for indexed sample projects; "
                 "game compatibility is incomplete.\n"
                 "Native game execution is experimental and compatibility is incomplete.\n"
                 "Undertale has been tested on Windows and in Cemu; no full playthrough\n"
                 "is verified. F10 closes the native game window.\n";
}
void print_instruction(const regml::Instruction &ins, std::size_t origin, bool resolved = false) {
    std::cout << "  0x" << std::hex << std::setw(8) << std::setfill('0') << origin + ins.offset << "  "
              << std::setw(8) << ins.word << std::setfill(' ') << std::dec << "  "
              << regml::opcode_name(ins.opcode());
    if (ins.has_branch_target())
        std::cout << " -> 0x" << std::hex << origin + ins.branch_target_value() << std::dec;
    else if (ins.environment_exit())
        std::cout << " <environment-exit>";
    else {
        std::cout << '.' << regml::type_name(ins.type1);
        if (ins.opcode() >= regml::Opcode::conv && ins.opcode() <= regml::Opcode::cmp)
            std::cout << '.' << regml::type_name(ins.type2());
        if (ins.immediate_kind == regml::Instruction::ImmediateKind::int32)
            std::cout << ' ' << ins.immediate_i32();
        if (ins.immediate_kind == regml::Instruction::ImmediateKind::int64)
            std::cout << ' ' << ins.immediate_i64();
        if (ins.immediate_kind == regml::Instruction::ImmediateKind::real64)
            std::cout << ' ' << regml::value_text(ins.immediate_real());
        if (ins.immediate_kind == regml::Instruction::ImmediateKind::real32)
            std::cout << ' ' << regml::value_text(ins.immediate_real());
        if (ins.has_reference())
            std::cout << " reference=0x" << std::hex << ins.reference_value() << std::dec
                      << (resolved ? "" : " (unresolved)");
        if (ins.opcode() == regml::Opcode::cmp)
            std::cout << " comparison=" << static_cast<unsigned>(ins.comparison());
        if (ins.opcode() == regml::Opcode::extended)
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
                      << "Display name: " << escaped(package.string_pointer(info->display_name_pointer))
                      << '\n'
                      << "Window: " << info->width << 'x' << info->height << '\n';
        } else
            std::cout << "Metadata/resource layout: unsupported; chunk inventory only\n";
    } else
        std::cout << "GEN8: absent; chunk inventory only (may be an audio group)\n";
    std::cout << "Chunks: " << package.chunks().size() << '\n';
    for (const auto &chunk : package.chunks())
        std::cout << "  " << chunk.name() << "  offset=0x" << std::hex << chunk.data_offset << std::dec
                  << "  bytes=" << chunk.size << '\n';
    std::cout << "Parsed strings: " << package.strings().size()
              << "\nParsed code entries: " << package.code().size() << '\n';
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
            std::cout << "  code[" << i << "] " << escaped(package.code_name(i))
                      << " bytes=" << entry.bytecode_length << " locals=" << entry.locals
                      << " args=" << entry.arguments << " entry-offset=" << entry.entry_offset << '\n';
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
    const std::string text(input);
    char *end = nullptr;
    errno = 0;
    const double result = std::strtod(text.c_str(), &end);
    if (input.empty() || errno == ERANGE || end != text.c_str() + text.size() || !std::isfinite(result) ||
        result < 0.01 || result > 1000.0)
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
            std::cout << "reGML " << regml::version << '\n';
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
                  << "\nInstructions: " << linked.instruction_count
                  << "\nVariables: " << linked.variables.size() << "\nFunctions: " << linked.functions.size()
                  << "\nLinked variable occurrences: " << linked.variable_at.size()
                  << "\nLinked function occurrences: " << linked.function_at.size()
                  << "\nReference chains verified. Runtime compatibility is not established.\n";
        return 0;
    }
    if (name == "gmrt") {
        if (argc < 4)
            throw regml::Error(regml::ErrorKind::usage, "gmrt requires inspect or extract and a WAD path");
        const std::string_view operation = argv[2];
        if (operation == "inspect") {
            if (argc != 4)
                throw regml::Error(regml::ErrorKind::usage, "gmrt inspect requires one WAD path");
            const auto wad = regml::GmrtWad::load(argv[3]);
            std::cout << "GMRT WAD inspection\nSize: " << wad.size()
                      << " bytes\nChunks: " << wad.chunks().size() << '\n';
            for (const auto &chunk : wad.chunks())
                std::cout << "  " << chunk.name() << "  header=0x" << std::hex << chunk.header_offset
                          << "  data=0x" << chunk.data_offset << std::dec << "  bytes=" << chunk.size << '\n';

            std::size_t total_module_bytes = 0;
            std::size_t total_functions = 0;
            std::size_t total_instructions = 0;
            for (std::size_t i = 0; i < wad.modules().size(); ++i) {
                const auto &module = wad.modules()[i];
                total_module_bytes += module.size;
                const auto ir = regml::GmrtModule::parse(wad.read_module(i));
                total_functions += ir.functions.size();
                for (const auto &function : ir.functions)
                    for (const auto &block : function.blocks)
                        total_instructions += block.instructions.size();
            }
            std::cout << "GMIR modules: " << wad.modules().size() << "\nGMIR bytes: " << total_module_bytes
                      << "\nParsed functions: " << total_functions
                      << "\nParsed instructions: " << total_instructions << "\nFirst modules:\n";
            constexpr std::size_t module_listing_limit = 16;
            for (std::size_t i = 0; i < std::min(wad.modules().size(), module_listing_limit); ++i) {
                const auto &module = wad.modules()[i];
                std::cout << "  [" << i << "] record=0x" << std::hex << module.record_offset << " data=0x"
                          << module.data_offset << " id=0x" << module.opaque_id << std::dec
                          << " bytes=" << module.size << '\n';
            }
            if (wad.modules().size() > module_listing_limit)
                std::cout << "  (listing limited to " << module_listing_limit << " modules)\n";
            std::cout << "The IR tables are parsed. Game-level resource and host-function integration is "
                         "incomplete.\n";
            return 0;
        }
        if (operation == "extract") {
            if (argc != 5)
                throw regml::Error(regml::ErrorKind::usage,
                                   "gmrt extract requires a WAD path and output directory");
            const auto wad = regml::GmrtWad::load(argv[3]);
            wad.extract_modules(argv[4]);
            std::cout << "Extracted " << wad.modules().size() << " GMIR modules to " << argv[4] << '\n';
            return 0;
        }
        if (operation == "run-function") {
            const bool trace = argc == 7 && std::string_view(argv[6]) == "--trace";
            if ((argc != 6 && !trace) || (argc == 7 && !trace))
                throw regml::Error(regml::ErrorKind::usage, "gmrt run-function requires a WAD path, module "
                                                            "index, function index, and optional --trace");
            const auto wad = regml::GmrtWad::load(argv[3]);
            const auto module_index = parse_index(argv[4]);
            const auto function_index = parse_index(argv[5]);
            if (module_index >= wad.modules().size())
                throw regml::Error(regml::ErrorKind::usage, "GMIR module index is out of range");
            std::vector<regml::GmrtModule> modules;
            modules.reserve(wad.modules().size());
            for (std::size_t i = 0; i < wad.modules().size(); ++i)
                modules.push_back(regml::GmrtModule::parse(wad.read_module(i)));
            const auto &module = modules[module_index];
            regml::GmrtExecutionContext context;
            context.self = std::make_shared<regml::StructValue>();
            regml::GmrtVmOptions options;
            if (trace)
                options.trace = [](const regml::GmrtFunction &function, const regml::GmrtBlock &block,
                                   const regml::GmrtInstruction &instruction, std::span<const regml::Value>) {
                    std::cout << function.symbol << " / " << block.name << "  "
                              << regml::gmrt_opcode_name(instruction.opcode) << '\n';
                };
            const auto result = regml::execute_gmrt(modules, module_index, function_index, std::move(context),
                                                    std::move(options));
            std::cout << "Executed GMIR function " << module.functions.at(function_index).symbol
                      << "\nInstructions: " << result.instructions_executed
                      << "\nReturn: " << regml::value_text(result.returned) << '\n';
            return 0;
        }
        if (operation == "run-ir-function") {
            const bool trace = argc == 6 && std::string_view(argv[5]) == "--trace";
            if ((argc != 5 && !trace) || (argc == 6 && !trace))
                throw regml::Error(
                    regml::ErrorKind::usage,
                    "gmrt run-ir-function requires a GMIR path, function index, and optional --trace");
            const auto module = regml::GmrtModule::load(argv[3]);
            const auto function_index = parse_index(argv[4]);
            regml::GmrtExecutionContext context;
            context.self = std::make_shared<regml::StructValue>();
            regml::GmrtVmOptions options;
            if (trace)
                options.trace = [](const regml::GmrtFunction &function, const regml::GmrtBlock &block,
                                   const regml::GmrtInstruction &instruction, std::span<const regml::Value>) {
                    std::cout << function.symbol << " / " << block.name << "  "
                              << regml::gmrt_opcode_name(instruction.opcode) << '\n';
                };
            const auto result =
                regml::execute_gmrt(module, function_index, std::move(context), std::move(options));
            std::cout << "Executed GMIR function " << module.functions.at(function_index).symbol
                      << "\nInstructions: " << result.instructions_executed
                      << "\nReturn: " << regml::value_text(result.returned) << '\n';
            return 0;
        }
        throw regml::Error(regml::ErrorKind::usage, "unknown gmrt operation: " + std::string(operation));
    }
    if (name == "disasm") {
        const bool resolve = argc == 5 && std::string_view(argv[4]) == "--linked";
        if (argc != 4 && !resolve)
            throw regml::Error(regml::ErrorKind::usage,
                               "disasm requires a package path and code index; optional --linked");
        const auto index = parse_index(argv[3]);
        const auto package = regml::Package::load(argv[2]);
        if (!package.general_info() || !package.general_info()->has_known_prefix)
            throw regml::Error(regml::ErrorKind::unsupported,
                               "no supported metadata profile for disassembly");
        const auto code = package.code_bytes(index);
        const auto program = regml::decode(regml::BinaryView(code, package.code()[index].bytecode_offset),
                                           package.general_info()->bytecode_version);
        std::optional<regml::LinkedPackage> linked;
        if (resolve)
            linked = regml::link_legacy_package(package);
        std::cout << "Code " << index << ": " << escaped(package.code_name(index))
                  << "\nExecution is not implied.\n";
        for (const auto &ins : program.instructions) {
            print_instruction(ins, program.origin, resolve);
            if (linked) {
                const auto address = program.origin + ins.offset;
                if (const auto it = linked->variable_at.find(address); it != linked->variable_at.end())
                    std::cout << "    variable: " << escaped(linked->variables[it->second].name)
                              << " reference-kind=0x" << std::hex << ((ins.reference_value() >> 24) & 0xf8)
                              << std::dec << '\n';
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
                  << "\nInstructions: " << result.instructions_executed
                  << "\nPeak stack: " << result.peak_stack_values << '\n';
        return 0;
    }
    if (name == "run") {
        if (argc < 3)
            throw regml::Error(regml::ErrorKind::usage, "run requires a data.win path");
        regml::PlayOptions options;
        for (int i = 3; i < argc; ++i) {
            const std::string_view flag = argv[i];
            if (flag == "--headless")
                options.headless = true;
            else if (flag == "--hidden")
                options.hidden = true;
            else if (flag == "--speed-control")
                options.speed_controls = true;
            else if (flag == "--debug-overlay")
                options.debug_overlay = true;
            else if (flag == "--frames" || flag == "--screenshot" || flag == "--input-script" ||
                     flag == "--seed" || flag == "--save-dir" || flag == "--speed" ||
                     flag == "--profile-log") {
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
                else if (flag == "--profile-log")
                    options.profile_log = argv[i];
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
        if (options.speed_controls && (options.headless || options.hidden))
            throw regml::Error(regml::ErrorKind::usage, "--speed-control requires a visible game window");
        if (options.debug_overlay && (options.headless || options.hidden))
            throw regml::Error(regml::ErrorKind::usage, "--debug-overlay requires a visible game window");
        const auto report = regml::play_game(argv[2], options);
        std::cout << "reGML package execution"
                  << "\nFrames: " << report.frames << "\nVM instructions: " << report.instructions;
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
}
