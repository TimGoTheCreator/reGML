cmake_minimum_required(VERSION 3.22)
project(regml LANGUAGES C CXX)

if(PS2)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(REGML_240P "Render the main screen at half resolution (320x240 for 640x480 games)" OFF)
option(REGML_PS2_NTSC "Force NTSC video mode on PS2 instead of using the console signal" OFF)
option(REGML_LOW_MEMORY "Use lazy bytecode and smaller caches for low-memory targets" OFF)

option(REGML_SDL2 "Enable SDL2 windowed frontend" ON)
option(REGML_WEB "Build the Emscripten web runner" OFF)
option(REGML_ANDROID "Build the Android JNI library" OFF)
option(REGML_MINIAUDIO "Use the experimental miniaudio output backend on desktop" OFF)
option(REGML_OPENAL "Use OpenAL for desktop audio output" OFF)
option(REGML_WIIU "Build a Wii U homebrew RPX with devkitPro wut" OFF)
option(REGML_WII "Build a Nintendo Wii homebrew DOL with devkitPro libogc" OFF)
set(REGML_UNDERTALE_DATA "" CACHE FILEPATH "Optional Undertale data.win path for audio integration testing")

if(REGML_WIIU AND REGML_WII)
    message(FATAL_ERROR "REGML_WIIU and REGML_WII are separate targets")
endif()

if(ANDROID)
    set(REGML_ANDROID ON)
endif()

if(EMSCRIPTEN)
    set(REGML_WEB ON CACHE BOOL "Build the Emscripten web runner" FORCE)
endif()
if(REGML_WEB AND NOT EMSCRIPTEN)
    message(FATAL_ERROR "REGML_WEB requires the Emscripten toolchain (emcmake)")
endif()
if(REGML_WEB AND (REGML_ANDROID OR REGML_WIIU OR REGML_WII OR PS2))
    message(FATAL_ERROR "REGML_WEB cannot be combined with another platform target")
endif()

if(REGML_MINIAUDIO AND (REGML_WIIU OR REGML_WII))
    message(FATAL_ERROR "REGML_MINIAUDIO is experimental and currently desktop-only")
endif()
if(REGML_MINIAUDIO AND NOT REGML_SDL2 AND NOT REGML_ANDROID)
    message(FATAL_ERROR "REGML_MINIAUDIO currently requires the SDL2 frontend")
endif()
if(REGML_OPENAL AND (REGML_WIIU OR REGML_WII))
    message(FATAL_ERROR "REGML_OPENAL is currently desktop-only")
endif()
if(REGML_OPENAL AND NOT REGML_SDL2)
    message(FATAL_ERROR "REGML_OPENAL currently requires the SDL2 frontend")
endif()
if(REGML_OPENAL AND REGML_MINIAUDIO)
    message(FATAL_ERROR "Choose only one optional audio output backend")
endif()

if(PS2)
    set(REGML_SDL2 OFF CACHE BOOL "Enable SDL2 windowed frontend" FORCE)
elseif(REGML_WII)
    set(REGML_SDL2 OFF CACHE BOOL "Enable SDL2 windowed frontend" FORCE)
elseif(REGML_WIIU)
    set(REGML_SDL2 ON CACHE BOOL "Enable SDL2 windowed frontend" FORCE)
endif()

if(REGML_ANDROID)
    include(FetchContent)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(INSTALL_DOCS OFF CACHE BOOL "" FORCE)
    set(INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    set(INSTALL_CMAKE_PACKAGE_MODULE OFF CACHE BOOL "" FORCE)
    set(PNG_SHARED OFF CACHE BOOL "" FORCE)
    set(PNG_TESTS OFF CACHE BOOL "" FORCE)
    set(PNG_TOOLS OFF CACHE BOOL "" FORCE)
    set(PNG_BUILD_ZLIB OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(regml_libpng
        GIT_REPOSITORY https://github.com/pnggroup/libpng.git
        GIT_TAG v1.6.50
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(regml_libpng)
    FetchContent_Declare(regml_ogg
        GIT_REPOSITORY https://github.com/xiph/ogg.git
        GIT_TAG v1.3.5
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(regml_ogg)
    FetchContent_Declare(regml_vorbis
        GIT_REPOSITORY https://github.com/xiph/vorbis.git
        GIT_TAG v1.3.7
        GIT_SHALLOW TRUE)
    set(OGG_INCLUDE_DIR "${regml_ogg_SOURCE_DIR}/include")
    set(OGG_LIBRARY ogg)
    set(OGG_VERSION_STRING 1.3.5)
    FetchContent_MakeAvailable(regml_vorbis)
    target_include_directories(vorbis PUBLIC "${regml_ogg_BINARY_DIR}/include")
elseif(REGML_WIIU OR REGML_WII)
    # Console portlibs are static, so include their private link dependencies too.
    set(PKG_CONFIG_ARGN --static)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(REGML_PNG REQUIRED IMPORTED_TARGET libpng)
    pkg_check_modules(REGML_ZLIB REQUIRED IMPORTED_TARGET zlib)
    pkg_check_modules(REGML_VORBISFILE REQUIRED IMPORTED_TARGET vorbisfile)
    if(REGML_WIIU)
        pkg_check_modules(REGML_SDL2 REQUIRED IMPORTED_TARGET sdl2)
    endif()
elseif(PS2)
    find_package(PNG REQUIRED)
    find_package(ZLIB REQUIRED)
    find_library(REGML_PS2_VORBISFILE NAMES vorbisfile REQUIRED)
    find_library(REGML_PS2_VORBIS NAMES vorbis REQUIRED)
    find_library(REGML_PS2_OGG NAMES ogg REQUIRED)
    find_path(REGML_PS2_GSKIT_INCLUDE NAMES gsKit.h HINTS "$ENV{GSKIT}/include" REQUIRED)
    find_library(REGML_PS2_GSKIT NAMES gskit HINTS "$ENV{GSKIT}/lib" REQUIRED)
    find_library(REGML_PS2_DMAKIT NAMES dmakit HINTS "$ENV{GSKIT}/lib" REQUIRED)
    find_library(REGML_PS2_PAD NAMES pad HINTS "$ENV{PS2SDK}/ee/lib" REQUIRED)
elseif(REGML_WEB)
    # Emscripten provides SDL2, zlib, libpng, and Vorbis as ports.
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

if(REGML_SDL2 AND NOT REGML_WIIU AND NOT REGML_WII AND NOT REGML_ANDROID AND NOT PS2 AND NOT REGML_WEB)
    find_package(SDL2 REQUIRED)
endif()

file(GLOB REGML_ENGINE_SOURCES
    CONFIGURE_DEPENDS
    "src/*.cpp"
    "src/platform/sdl/*.cpp"
    "src/platform/wii/*.cpp"
    "src/platform/ps2/*.cpp"
    "src/rendersoftware/*.cpp"
)
file(GLOB REGML_AUDIO_SOURCES CONFIGURE_DEPENDS "src/audio/sdl/*.cpp")
list(APPEND REGML_ENGINE_SOURCES ${REGML_AUDIO_SOURCES})
if(REGML_ANDROID)
    list(FILTER REGML_ENGINE_SOURCES EXCLUDE REGEX "/platform/sdl/sdl\\.cpp$")
    list(APPEND REGML_ENGINE_SOURCES
        "src/audio/miniaudio/output.cpp"
        "src/audio/miniaudio/miniaudio_impl.cpp"
        "src/platform/android/native.cpp")
endif()
if(REGML_WIIU)
    list(APPEND REGML_ENGINE_SOURCES "src/platform/wiiu/audio_async.cpp")
endif()
if(REGML_MINIAUDIO)
    list(APPEND REGML_ENGINE_SOURCES
        "src/audio/miniaudio/output.cpp"
        "src/audio/miniaudio/miniaudio_impl.cpp"
    )
endif()
if(REGML_OPENAL)
    find_package(OpenAL REQUIRED)
    list(APPEND REGML_ENGINE_SOURCES "src/audio/openal/output.cpp")
endif()
if(REGML_WII)
    list(APPEND REGML_ENGINE_SOURCES "src/audio/wii/output.cpp")
endif()
list(FILTER REGML_ENGINE_SOURCES EXCLUDE REGEX "/main\\.cpp$")

if(REGML_ANDROID)
    add_library(regml_android SHARED ${REGML_ENGINE_SOURCES})
    set(REGML_TARGET regml_android)
else()
    add_library(regml_engine STATIC ${REGML_ENGINE_SOURCES})
    set(REGML_TARGET regml_engine)
endif()
if(REGML_ANDROID)
    set_target_properties(regml_android PROPERTIES OUTPUT_NAME regml)
    target_compile_definitions(regml_android PUBLIC REGML_ANDROID=1 REGML_MINIAUDIO=1 REGML_WITH_SDL2=0 REGML_WII=0 REGML_WIIU=0)
    target_include_directories(regml_android PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(regml_android PRIVATE android log OpenSLES)
else()
    set(REGML_TARGET regml_engine)
endif()
if(PS2)
    if(DEFINED ENV{PS2SDK})
        set(PS2_BZIP2_DIR "$ENV{PS2SDK}/ports")
    elseif(DEFINED ENV{PS2DEV})
        set(PS2_BZIP2_DIR "$ENV{PS2DEV}/ps2sdk/ports")
    endif()
    if(PS2_BZIP2_DIR AND EXISTS "${PS2_BZIP2_DIR}/include/bzlib.h" AND
       EXISTS "${PS2_BZIP2_DIR}/lib/libbz2_static.a")
        target_include_directories(${REGML_TARGET} SYSTEM PRIVATE "${PS2_BZIP2_DIR}/include")
        target_compile_definitions(${REGML_TARGET} PRIVATE REGML_HAS_BZIP2=1)
        target_link_libraries(${REGML_TARGET} PUBLIC "${PS2_BZIP2_DIR}/lib/libbz2_static.a")
    else()
        message(WARNING "PS2SDK BZip2 port not found; BZ2-QOI textures are unavailable")
        target_compile_definitions(${REGML_TARGET} PRIVATE REGML_HAS_BZIP2=0)
    endif()
else()
    if(NOT REGML_ANDROID AND NOT REGML_WEB AND NOT REGML_WIIU AND NOT REGML_WII)
        find_package(BZip2 QUIET)
    endif()
    if(REGML_WEB)
        target_compile_definitions(${REGML_TARGET} PRIVATE REGML_HAS_BZIP2=1)
    elseif(BZip2_FOUND AND NOT REGML_ANDROID AND NOT REGML_WIIU AND NOT REGML_WII)
        target_compile_definitions(${REGML_TARGET} PRIVATE REGML_HAS_BZIP2=1)
        target_link_libraries(${REGML_TARGET} PUBLIC BZip2::BZip2)
    else()
        target_compile_definitions(${REGML_TARGET} PRIVATE REGML_HAS_BZIP2=0)
    endif()
endif()
target_compile_definitions(${REGML_TARGET} PUBLIC
    REGML_240P=$<BOOL:${REGML_240P}>
    REGML_PS2_NTSC=$<BOOL:${REGML_PS2_NTSC}>
    REGML_LOW_MEMORY=$<BOOL:${REGML_LOW_MEMORY}>
)

target_include_directories(${REGML_TARGET} PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_include_directories(${REGML_TARGET} SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

if(REGML_MINIAUDIO OR REGML_ANDROID)
    target_compile_definitions(${REGML_TARGET} PUBLIC REGML_MINIAUDIO=1)
    if(WIN32)
        target_link_libraries(${REGML_TARGET} PUBLIC winmm ole32 ksuser version setupapi)
    elseif(ANDROID)
        target_link_libraries(${REGML_TARGET} PUBLIC android log OpenSLES)
    elseif(UNIX)
        find_package(Threads REQUIRED)
        target_link_libraries(${REGML_TARGET} PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
        if(NOT APPLE)
            target_link_libraries(${REGML_TARGET} PUBLIC m)
        endif()
    endif()
endif()
if(REGML_OPENAL)
    target_compile_definitions(${REGML_TARGET} PUBLIC REGML_OPENAL=1)
    target_link_libraries(${REGML_TARGET} PUBLIC OpenAL::OpenAL)
endif()

if(REGML_ANDROID)
    target_link_libraries(${REGML_TARGET} PUBLIC png_static vorbisfile)
elseif(REGML_WEB)
    target_compile_options(${REGML_TARGET} PUBLIC -fexceptions -sUSE_SDL=2 -sUSE_ZLIB=1 -sUSE_LIBPNG=1 -sUSE_VORBIS=1 -sUSE_BZIP2=1)
    target_link_options(${REGML_TARGET} PUBLIC -fexceptions -sUSE_SDL=2 -sUSE_ZLIB=1 -sUSE_LIBPNG=1 -sUSE_VORBIS=1 -sUSE_BZIP2=1)
elseif(REGML_WIIU OR REGML_WII)
    target_link_libraries(${REGML_TARGET} PUBLIC
        PkgConfig::REGML_PNG
        PkgConfig::REGML_ZLIB
        PkgConfig::REGML_VORBISFILE
    )
    if(REGML_WIIU)
        target_link_libraries(${REGML_TARGET} PUBLIC PkgConfig::REGML_SDL2)
    endif()
elseif(PS2)
    target_include_directories(${REGML_TARGET} SYSTEM PUBLIC "${REGML_PS2_GSKIT_INCLUDE}")
    target_link_libraries(${REGML_TARGET} PUBLIC
        PNG::PNG
        ZLIB::ZLIB
        ${REGML_PS2_VORBISFILE}
        ${REGML_PS2_VORBIS}
        ${REGML_PS2_OGG}
        ${REGML_PS2_GSKIT}
        ${REGML_PS2_DMAKIT}
        ${REGML_PS2_PAD}
        kernel
        patches
        fileXio
        c
        m
    )
else()
    target_link_libraries(regml_engine PUBLIC PNG::PNG ZLIB::ZLIB regml_vorbisfile)
endif()

if(PS2)
    target_compile_definitions(${REGML_TARGET} PUBLIC REGML_PS2=1 REGML_WITH_SDL2=0 REGML_WII=0 REGML_WIIU=0)
elseif(REGML_ANDROID)
    target_compile_definitions(${REGML_TARGET} PUBLIC REGML_WITH_SDL2=0)
elseif(REGML_WII)
    target_compile_definitions(${REGML_TARGET} PUBLIC REGML_WITH_SDL2=0)
elseif(REGML_WEB)
    target_compile_definitions(${REGML_TARGET} PUBLIC REGML_WITH_SDL2=1 REGML_WEB=1)
elseif(REGML_SDL2)
    target_compile_definitions(${REGML_TARGET} PUBLIC REGML_WITH_SDL2=1)
    if(REGML_WIIU)
        target_compile_definitions(regml_engine PUBLIC SDL_MAIN_HANDLED)
        # Linked through pkg-config above.
    elseif(TARGET SDL2::SDL2)
        target_link_libraries(${REGML_TARGET} PUBLIC SDL2::SDL2)
    else()
        target_include_directories(${REGML_TARGET} PUBLIC ${SDL2_INCLUDE_DIRS})
        target_link_libraries(${REGML_TARGET} PUBLIC ${SDL2_LIBRARIES})
    endif()
else()
    target_compile_definitions(${REGML_TARGET} PUBLIC REGML_WITH_SDL2=0)
endif()

if(REGML_ANDROID)
    # The Android Activity loads the shared library through JNI.
elseif(REGML_WIIU)
    add_executable(regml src/platform/wiiu/main.cpp)
elseif(REGML_WII)
    add_executable(regml src/platform/wii/main.cpp)
elseif(PS2)
    add_executable(regml src/platform/ps2/main.cpp)
else()
    add_executable(regml src/main.cpp)
endif()
if(NOT REGML_ANDROID)
    target_link_libraries(regml PRIVATE regml_engine)
endif()
if(REGML_WEB)
    set_target_properties(regml PROPERTIES OUTPUT_NAME index SUFFIX ".html")
    target_link_options(regml PRIVATE
        "--shell-file=${CMAKE_CURRENT_SOURCE_DIR}/src/platform/web/shell.html"
        "--pre-js=${CMAKE_CURRENT_SOURCE_DIR}/src/platform/web/runner.js"
        -lidbfs.js
        -sASYNCIFY=1
        -sALLOW_MEMORY_GROWTH=1
        -sNO_EXIT_RUNTIME=1
        -sFORCE_FILESYSTEM=1
        "-sEXPORTED_RUNTIME_METHODS=['callMain','FS']"
    )
endif()
if(PS2)
    find_program(REGML_PS2_BIN2C bin2c HINTS "$ENV{PS2SDK}/bin" REQUIRED)
    set(REGML_PS2_IRX_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/ps2")
    file(MAKE_DIRECTORY "${REGML_PS2_IRX_DIR}")
    foreach(_module IN ITEMS iomanX fileXio usbd usbmass_bd)
        add_custom_command(
            OUTPUT "${REGML_PS2_IRX_DIR}/${_module}.c"
            COMMAND "${REGML_PS2_BIN2C}" "$ENV{PS2SDK}/iop/irx/${_module}.irx"
                    "${REGML_PS2_IRX_DIR}/${_module}.c" "${_module}_irx"
            DEPENDS "$ENV{PS2SDK}/iop/irx/${_module}.irx"
            COMMENT "Embedding PS2 ${_module} driver"
            VERBATIM
        )
        target_sources(regml PRIVATE "${REGML_PS2_IRX_DIR}/${_module}.c")
    endforeach()
    target_include_directories(regml PRIVATE "${REGML_PS2_IRX_DIR}")
endif()
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
    target_link_libraries(regml PRIVATE fat)
    target_compile_options(regml_engine PRIVATE $<$<CONFIG:Release>:-O3> $<$<CONFIG:Release>:-flto>)
    target_compile_options(regml PRIVATE $<$<CONFIG:Release>:-O3> $<$<CONFIG:Release>:-flto>)
    target_link_options(regml PRIVATE $<$<CONFIG:Release>:-flto>)
    if(COMMAND ogc_create_dol)
        ogc_create_dol(regml)
    endif()
endif()

if(PS2)
    target_compile_definitions(regml PRIVATE REGML_PS2=1)
    set_target_properties(regml PROPERTIES SUFFIX ".elf")
endif()

include(CTest)
if(BUILD_TESTING AND NOT REGML_WIIU AND NOT REGML_WII AND NOT REGML_ANDROID AND NOT PS2 AND NOT REGML_WEB)
    add_executable(regml_tests tests/core_tests.cpp)
    target_link_libraries(regml_tests PRIVATE regml_engine)
    add_executable(regml_audio_test tests/audio_integration.cpp)
    target_link_libraries(regml_audio_test PRIVATE regml_engine)
    add_executable(regml_assets_test tests/assets_integration.cpp)
    target_link_libraries(regml_assets_test PRIVATE regml_engine)
    if(REGML_UNDERTALE_DATA)
        add_test(NAME undertale_audio COMMAND regml_audio_test "${REGML_UNDERTALE_DATA}")
        add_test(NAME undertale_collision_masks COMMAND regml_assets_test "${REGML_UNDERTALE_DATA}")
        add_executable(regml_linker_test tests/linker_integration.cpp)
        target_link_libraries(regml_linker_test PRIVATE regml_engine)
        add_test(NAME undertale_lazy_linker COMMAND regml_linker_test "${REGML_UNDERTALE_DATA}")
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
endif()
