cmake_minimum_required(VERSION 3.24)
project(regml LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(REGML_SDL2 "Enable SDL2 windowed frontend" ON)
option(REGML_WIIU "Build a Wii U homebrew RPX with devkitPro wut" OFF)
option(REGML_WII "Build a Nintendo Wii homebrew DOL with devkitPro libogc" OFF)
set(REGML_UNDERTALE_DATA "" CACHE FILEPATH "Optional Undertale data.win path for audio integration testing")

if(REGML_WIIU AND REGML_WII)
    message(FATAL_ERROR "REGML_WIIU and REGML_WII are separate targets")
endif()

if(REGML_WIIU OR REGML_WII)
    set(REGML_SDL2 ON CACHE BOOL "Enable SDL2 windowed frontend" FORCE)
    # Console portlibs are static, so include their private link dependencies too.
    set(PKG_CONFIG_ARGN --static)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(REGML_PNG REQUIRED IMPORTED_TARGET libpng)
    pkg_check_modules(REGML_ZLIB REQUIRED IMPORTED_TARGET zlib)
    pkg_check_modules(REGML_VORBISFILE REQUIRED IMPORTED_TARGET vorbisfile)
    pkg_check_modules(REGML_SDL2 REQUIRED IMPORTED_TARGET sdl2)
else()
    find_package(PNG REQUIRED)
    find_package(ZLIB REQUIRED)

    find_path(VORBISFILE_INCLUDE_DIR NAMES vorbis/vorbisfile.h REQUIRED)
    find_library(VORBISFILE_LIBRARY NAMES vorbisfile REQUIRED)
    add_library(regml_vorbisfile UNKNOWN IMPORTED)
    set_target_properties(regml_vorbisfile PROPERTIES
        IMPORTED_LOCATION "${VORBISFILE_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${VORBISFILE_INCLUDE_DIR}"
    )
endif()

if(REGML_SDL2 AND NOT REGML_WIIU AND NOT REGML_WII)
    find_package(SDL2 REQUIRED)
endif()

file(GLOB REGML_ENGINE_SOURCES
    CONFIGURE_DEPENDS
    "src/*.cpp"
    "src/platform/sdl/*.cpp"
    "src/platform/wii/*.cpp"
    "src/rendersoftware/*.cpp"
)
file(GLOB REGML_AUDIO_SOURCES CONFIGURE_DEPENDS "src/audio/sdl/*.cpp")
file(GLOB REGML_DEMO_SOURCES CONFIGURE_DEPENDS "src/demo/*.cpp")
list(APPEND REGML_ENGINE_SOURCES ${REGML_AUDIO_SOURCES} ${REGML_DEMO_SOURCES})
if(REGML_WIIU)
    list(APPEND REGML_ENGINE_SOURCES "src/platform/wiiu/audio_async.cpp")
endif()
list(FILTER REGML_ENGINE_SOURCES EXCLUDE REGEX "/main\\.cpp$")

add_library(regml_engine STATIC ${REGML_ENGINE_SOURCES})

target_include_directories(regml_engine PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

if(REGML_WIIU OR REGML_WII)
    target_link_libraries(regml_engine PUBLIC
        PkgConfig::REGML_PNG
        PkgConfig::REGML_ZLIB
        PkgConfig::REGML_VORBISFILE
        PkgConfig::REGML_SDL2
    )
else()
    target_link_libraries(regml_engine PUBLIC PNG::PNG ZLIB::ZLIB regml_vorbisfile)
endif()

if(REGML_SDL2)
    target_compile_definitions(regml_engine PUBLIC REGML_WITH_SDL2=1 SDL_MAIN_HANDLED)
    if(REGML_WIIU OR REGML_WII)
        # Linked through pkg-config above.
    elseif(TARGET SDL2::SDL2)
        target_link_libraries(regml_engine PUBLIC SDL2::SDL2)
    else()
        target_include_directories(regml_engine PUBLIC ${SDL2_INCLUDE_DIRS})
        target_link_libraries(regml_engine PUBLIC ${SDL2_LIBRARIES})
    endif()
else()
    target_compile_definitions(regml_engine PUBLIC REGML_WITH_SDL2=0)
endif()

if(REGML_WIIU)
    add_executable(regml src/platform/wiiu/main.cpp)
elseif(REGML_WII)
    add_executable(regml src/platform/wii/main.cpp)
else()
    add_executable(regml src/main.cpp)
endif()
target_link_libraries(regml PRIVATE regml_engine)
if(REGML_WIIU)
    target_compile_options(regml_engine PRIVATE $<$<CONFIG:Release>:-O3> $<$<CONFIG:Release>:-flto>)
    target_compile_options(regml PRIVATE $<$<CONFIG:Release>:-O3> $<$<CONFIG:Release>:-flto>)
    target_link_options(regml PRIVATE $<$<CONFIG:Release>:-flto>)
    if(NOT COMMAND wut_create_rpx)
        message(FATAL_ERROR "REGML_WIIU requires devkitPro's powerpc-eabi-cmake and wut toolchain")
    endif()
    target_compile_definitions(regml_engine PUBLIC REGML_WIIU=1)
    wut_create_rpx(regml)
    if(COMMAND wut_create_wuhb)
        wut_create_wuhb(regml NAME "reGML" SHORTNAME "reGML" AUTHOR "TimGoTheCreator")
    endif()
endif()

if(REGML_WII)
    target_compile_definitions(regml_engine PUBLIC REGML_WII=1)
    target_compile_definitions(regml PRIVATE REGML_WII=1)
    target_compile_options(regml_engine PRIVATE $<$<CONFIG:Release>:-O3> $<$<CONFIG:Release>:-flto>)
    target_compile_options(regml PRIVATE $<$<CONFIG:Release>:-O3> $<$<CONFIG:Release>:-flto>)
    target_link_options(regml PRIVATE $<$<CONFIG:Release>:-flto>)
    if(COMMAND ogc_create_dol)
        ogc_create_dol(regml)
    endif()
endif()

include(CTest)
if(BUILD_TESTING AND NOT REGML_WIIU AND NOT REGML_WII)
    add_executable(regml_tests tests/core_tests.cpp)
    target_link_libraries(regml_tests PRIVATE regml_engine)
    add_executable(regml_audio_test tests/audio_integration.cpp)
    target_link_libraries(regml_audio_test PRIVATE regml_engine)
    add_executable(regml_assets_test tests/assets_integration.cpp)
    target_link_libraries(regml_assets_test PRIVATE regml_engine)
    if(REGML_UNDERTALE_DATA)
        add_test(NAME undertale_audio COMMAND regml_audio_test "${REGML_UNDERTALE_DATA}")
        add_test(NAME undertale_collision_masks COMMAND regml_assets_test "${REGML_UNDERTALE_DATA}")
    endif()
    add_test(NAME core COMMAND regml_tests)
    add_test(NAME cli_help COMMAND regml --help)
    add_test(NAME cli_demo COMMAND regml vm-demo)
    set_tests_properties(cli_demo PROPERTIES PASS_REGULAR_EXPRESSION "Result: 42")
    add_test(NAME cli_bad_command COMMAND regml unknown-command)
    set_tests_properties(cli_bad_command PROPERTIES WILL_FAIL TRUE)
    add_test(NAME fixture_emit COMMAND regml_tests --emit-fixture ${CMAKE_CURRENT_BINARY_DIR}/synthetic-data.win)
    set_tests_properties(fixture_emit PROPERTIES FIXTURES_SETUP synthetic_package)
    add_test(NAME cli_inspect COMMAND regml inspect ${CMAKE_CURRENT_BINARY_DIR}/synthetic-data.win --code --strings)
    add_test(NAME cli_disasm COMMAND regml disasm ${CMAKE_CURRENT_BINARY_DIR}/synthetic-data.win 0)
    set_tests_properties(cli_inspect cli_disasm PROPERTIES FIXTURES_REQUIRED synthetic_package)
    set_tests_properties(cli_inspect PROPERTIES PASS_REGULAR_EXPRESSION "gml_Script_test")
    set_tests_properties(cli_disasm PROPERTIES PASS_REGULAR_EXPRESSION "ret.i")
    add_test(NAME cli_play_headless COMMAND regml play-demo --headless --frames 40 --autoplay)
    set_tests_properties(cli_play_headless PROPERTIES PASS_REGULAR_EXPRESSION "Player: 480, 380")
    if(REGML_SDL2)
        add_test(NAME cli_video COMMAND regml play-demo --hidden --frames 2)
        set_tests_properties(cli_video PROPERTIES
            ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software"
            PASS_REGULAR_EXPRESSION "Frames: 2")
    endif()
endif()
