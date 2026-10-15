#include "linker.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

int main(int argc, char **argv) {
    try {
        if (argc != 2)
            throw std::runtime_error("expected an Undertale data.win path");
        const auto package = regml::Package::load(argv[1]);
        auto linked = regml::link_legacy_package(package, {}, false, false, true);
        std::size_t decoded_instructions = 0;
        std::size_t peak_decoded_bytes = 0;
        for (std::size_t i = 0; i < linked.programs.size(); ++i) {
            decoded_instructions += linked.program(package, i).instructions.size();
            peak_decoded_bytes = std::max(peak_decoded_bytes, linked.decoded_program_bytes);
            linked.release_programs();
        }
        if (decoded_instructions != linked.instruction_count)
            throw std::runtime_error("lazy decoding changed the package instruction count");
        std::cout << "Lazy bindings restored across " << linked.programs.size() << " code entries ("
                  << decoded_instructions << " instructions, " << linked.reference_symbols.size()
                  << " compact symbol IDs); decoded cache peaked at " << peak_decoded_bytes << " bytes\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
