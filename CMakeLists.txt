cmake_minimum_required(VERSION 3.24)
project(regml LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(REGML_SDL2 "Enable SDL2 windowed frontend" ON)

find_package(PkgConfig REQUIRED)
find_package(PNG REQUIRED)
find_package(ZLIB REQUIRED)

pkg_check_modules(VORBISFILE REQUIRED IMPORTED_TARGET vorbisfile)

if(REGML_SDL2)
    find_package(SDL2 REQUIRED)
endif()

file(GLOB_RECURSE REGML_ENGINE_SOURCES
    CONFIGURE_DEPENDS
    "src/core/*.cpp"
    "src/bytecode/*.cpp"
    "src/formats/*.cpp"
    "src/platform/*.cpp"
    "src/render/*.cpp"
    "src/runtime/*.cpp"
    "src/vm/*.cpp"
)

add_library(regml_engine STATIC ${REGML_ENGINE_SOURCES})

target_include_directories(regml_engine PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(regml_engine PUBLIC
    PNG::PNG
    ZLIB::ZLIB
    PkgConfig::VORBISFILE
)

if(REGML_SDL2)
    target_compile_definitions(regml_engine PUBLIC REGML_WITH_SDL2=1 SDL_MAIN_HANDLED)
    if(TARGET SDL2::SDL2)
        target_link_libraries(regml_engine PUBLIC SDL2::SDL2)
    else()
        target_include_directories(regml_engine PUBLIC ${SDL2_INCLUDE_DIRS})
        target_link_libraries(regml_engine PUBLIC ${SDL2_LIBRARIES})
    endif()
else()
    target_compile_definitions(regml_engine PUBLIC REGML_WITH_SDL2=0)
endif()

file(GLOB_RECURSE REGML_APP_SOURCES
    CONFIGURE_DEPENDS
    "src/app/*.cpp"
)

add_executable(regml ${REGML_APP_SOURCES})
target_link_libraries(regml PRIVATE regml_engine)

include(CTest)
if(BUILD_TESTING)
    add_executable(regml_tests tests/core_tests.cpp)
    target_link_libraries(regml_tests PRIVATE regml_engine)
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
