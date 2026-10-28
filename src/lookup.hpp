/* this actually isn't a lookup table, it's called lookup.hpp because i got tired of changing the version on
 * each platform */
#pragma once
#include <string_view>

namespace regml {
#if REGML_240P
inline constexpr int render_width = 320, render_height = 240;
#else
inline constexpr int render_width = 640, render_height = 480;
#endif
inline constexpr std::string_view version = "0.3.0";
inline constexpr std::string_view debug_overlay_marker = "regml-debug.enabled";
inline constexpr std::string_view debug_overlay_fps_label = "FPS ";
inline constexpr std::string_view debug_overlay_frame_label = "FRAME ";
inline constexpr std::string_view debug_overlay_room_label = "ROOM ";
inline constexpr std::string_view debug_overlay_code_label = "CODE ";
inline constexpr std::string_view debug_overlay_address_label = "PC 0x";
inline constexpr std::string_view debug_overlay_milliseconds_label = " MS";
} // namespace regml
