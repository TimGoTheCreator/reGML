if(NOT DEFINED ENV{OO_PS4_TOOLCHAIN} OR "$ENV{OO_PS4_TOOLCHAIN}" STREQUAL "")
    message(FATAL_ERROR "Set OO_PS4_TOOLCHAIN to the OpenOrbis PS4 toolchain directory")
endif()

file(TO_CMAKE_PATH "$ENV{OO_PS4_TOOLCHAIN}" REGML_PS4_TOOLCHAIN_ROOT)
if(NOT EXISTS "${REGML_PS4_TOOLCHAIN_ROOT}/link.x" OR
   NOT EXISTS "${REGML_PS4_TOOLCHAIN_ROOT}/include/c++/v1")
    message(FATAL_ERROR "OO_PS4_TOOLCHAIN must point to an extracted OpenOrbis toolchain")
endif()

find_program(REGML_PS4_CLANG NAMES clang REQUIRED)
find_program(REGML_PS4_CLANGXX NAMES clang++ REQUIRED)
find_program(REGML_PS4_LLD NAMES ld.lld REQUIRED)
find_program(REGML_PS4_LLVM_AR NAMES llvm-ar REQUIRED)
find_program(REGML_PS4_LLVM_RANLIB NAMES llvm-ranlib REQUIRED)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_SYSROOT "${REGML_PS4_TOOLCHAIN_ROOT}")
set(CMAKE_C_COMPILER "${REGML_PS4_CLANG}" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${REGML_PS4_CLANGXX}" CACHE FILEPATH "" FORCE)
set(CMAKE_C_COMPILER_TARGET x86_64-pc-freebsd12-elf)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-freebsd12-elf)
set(CMAKE_AR "${REGML_PS4_LLVM_AR}" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${REGML_PS4_LLVM_RANLIB}" CACHE FILEPATH "" FORCE)
set(CMAKE_LINKER "${REGML_PS4_LLD}" CACHE FILEPATH "" FORCE)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(NOT DEFINED REGML_PS4_DEPS_ROOT AND NOT "$ENV{REGML_PS4_DEPS_ROOT}" STREQUAL "")
    set(REGML_PS4_DEPS_ROOT "$ENV{REGML_PS4_DEPS_ROOT}" CACHE PATH
        "Install prefix for PS4 third-party libraries")
endif()

set(_regml_ps4_common_flags
    "-fPIC -funwind-tables -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -isystem \"${REGML_PS4_TOOLCHAIN_ROOT}/include\"")
set(CMAKE_C_FLAGS_INIT "${_regml_ps4_common_flags}")
set(CMAKE_CXX_FLAGS_INIT
    "${_regml_ps4_common_flags} -stdlib=libc++ -isystem \"${REGML_PS4_TOOLCHAIN_ROOT}/include/c++/v1\" -include \"${REGML_PS4_TOOLCHAIN_ROOT}/include/time.h\"")
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-fuse-ld=lld -nostdlib -Wl,-m,elf_x86_64 -Wl,-pie -Wl,--script,\"${REGML_PS4_TOOLCHAIN_ROOT}/link.x\" -Wl,--eh-frame-hdr -L\"${REGML_PS4_TOOLCHAIN_ROOT}/lib\"")
set(_regml_ps4_system_libraries
    "-lc -lkernel -lc++ -lm -lSceUserService -lSceVideoOut -lSceAudioOut -lScePad -lSceSysmodule -lSceFreeType \"${REGML_PS4_TOOLCHAIN_ROOT}/lib/crt1.o\"")
set(CMAKE_C_STANDARD_LIBRARIES_INIT "${_regml_ps4_system_libraries}")
set(CMAKE_CXX_STANDARD_LIBRARIES_INIT "${_regml_ps4_system_libraries}")

set(CMAKE_FIND_ROOT_PATH "${REGML_PS4_TOOLCHAIN_ROOT}")
if(DEFINED REGML_PS4_DEPS_ROOT AND NOT REGML_PS4_DEPS_ROOT STREQUAL "")
    list(APPEND CMAKE_FIND_ROOT_PATH "${REGML_PS4_DEPS_ROOT}")
endif()
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

unset(_regml_ps4_common_flags)
unset(_regml_ps4_system_libraries)
