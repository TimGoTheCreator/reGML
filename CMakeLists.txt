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
    "src/bytecode/*.cpp"
    "src/formats/*.cpp"
    "src/platform/*.cpp"
    "src/render/*.cpp"
    "src/runtime/*.cpp"
    "src/vm/*.cpp"
)

add_library(regml_engine STATIC ${REGML_ENGINE_SOURCES})

target_include_directories(regml_engine PUBLIC src)

target_link_libraries(regml_engine PUBLIC
    PNG::PNG
    ZLIB::ZLIB
    PkgConfig::VORBISFILE
)

if(REGML_SDL2)
    target_compile_definitions(regml_engine PUBLIC REGML_HAS_SDL2)
    if(TARGET SDL2::SDL2)
        target_link_libraries(regml_engine PUBLIC SDL2::SDL2)
    else()
        target_include_directories(regml_engine PUBLIC ${SDL2_INCLUDE_DIRS})
        target_link_libraries(regml_engine PUBLIC ${SDL2_LIBRARIES})
    endif()
endif()

file(GLOB_RECURSE REGML_APP_SOURCES
    CONFIGURE_DEPENDS
    "src/app/*.cpp"
)

add_executable(regml ${REGML_APP_SOURCES})
target_link_libraries(regml PRIVATE regml_engine)

include(CTest)
if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    add_subdirectory(tests)
endif()
