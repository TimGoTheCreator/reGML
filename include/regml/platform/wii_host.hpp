#pragma once
#include <string_view>

namespace regml {
void initialize_wii_platform();
void report_wii_memory(std::string_view stage);
void report_wii_frame(std::size_t frame, std::string_view room);
} // namespace regml
