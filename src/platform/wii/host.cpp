#include "host.hpp"

#if REGML_WII
#include "error.hpp"
#include <cstdio>
#include <fat.h>
#include <malloc.h>
#include <ogc/pad.h>
#include <ogc/system.h>
#include <wiiuse/wpad.h>

namespace regml {
void report_wii_frame(std::size_t frame, std::string_view room) {
    auto *file = std::fopen("sd:/reGML/regml-frame.log", "w");
    if (!file)
        return;
    std::fprintf(file, "frame=%zu room=%.*s\n", frame, static_cast<int>(room.size()), room.data());
    std::fclose(file);
}
void report_wii_memory(std::string_view stage) {
    auto *file = std::fopen("sd:/reGML/regml-boot.log", "a");
    if (!file)
        return;
    const auto heap = mallinfo();
    std::fprintf(file, "%.*s: heap used=%d free=%d MEM1=%u MEM2=%u\n", static_cast<int>(stage.size()),
                 stage.data(), heap.uordblks, heap.fordblks, SYS_GetArenaSize(), SYS_GetArena2Size());
    std::fclose(file);
}
void initialize_wii_platform() {
    if (!fatInitDefault())
        throw Error(ErrorKind::io, "could not mount the Wii SD card");
    PAD_Init();
    WPAD_Init();
}
} // namespace regml
#endif
