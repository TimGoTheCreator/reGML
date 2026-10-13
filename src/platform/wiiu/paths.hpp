#pragma once
#include <filesystem>

namespace regml {
inline std::filesystem::path wiiu_save_directory() {
    return "/vol/external01/wiiu/apps/reGML/saves";
}
} // namespace regml
