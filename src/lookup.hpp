#pragma once
#include <string_view>

namespace regml {
inline constexpr std::string_view version = "0.2.1";
inline constexpr std::string_view debug_overlay_marker = "regml-debug.enabled";
inline constexpr std::string_view debug_overlay_fps_label = "FPS ";
inline constexpr std::string_view debug_overlay_frame_label = "FRAME ";
inline constexpr std::string_view debug_overlay_room_label = "ROOM ";
inline constexpr std::string_view debug_overlay_code_label = "CODE ";
inline constexpr std::string_view debug_overlay_address_label = "PC 0x";
inline constexpr std::string_view debug_overlay_milliseconds_label = " MS";
} // namespace regml
