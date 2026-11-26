# CMake package configuration for CafeGLSL. Defines the imported target
# cafeglsl::cafeglsl, for use from a devkitPro Wii U project:
#
#   find_package(cafeglsl REQUIRED)
#   target_link_libraries(myapp PRIVATE cafeglsl::cafeglsl)
#   #include <cafeglsl/CafeGLSLCompiler.h>

set(cafeglsl_VERSION "0.3.0")

# This file installs to <prefix>/lib/cmake/cafeglsl/, so the prefix is three up.
get_filename_component(_cafeglsl_prefix "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

set(cafeglsl_INCLUDE_DIR "${_cafeglsl_prefix}/include")
set(cafeglsl_LIBRARY "${_cafeglsl_prefix}/lib/libcafeglsl.a")

if(NOT EXISTS "${cafeglsl_LIBRARY}")
  set(cafeglsl_FOUND FALSE)
  set(cafeglsl_NOT_FOUND_MESSAGE
      "cafeglsl: expected static library at ${cafeglsl_LIBRARY}, but it does not exist")
  return()
endif()

if(NOT TARGET cafeglsl::cafeglsl)
  add_library(cafeglsl::cafeglsl STATIC IMPORTED)
  set_target_properties(cafeglsl::cafeglsl PROPERTIES
    IMPORTED_LOCATION "${cafeglsl_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${cafeglsl_INCLUDE_DIR}"
    # The final link needs --gc-sections. Without it, unreachable Mesa code pulls in
    # getuid, pthread_sigmask and friends, which wut and newlib do not provide, and the
    # link fails. wut.specs already passes it, so this only covers links without it.
    INTERFACE_LINK_OPTIONS "-Wl,--gc-sections"
    INTERFACE_LINK_LIBRARIES "wut;m"
    IMPORTED_LINK_INTERFACE_LANGUAGES "CXX")
endif()

set(cafeglsl_INCLUDE_DIRS "${cafeglsl_INCLUDE_DIR}")
set(cafeglsl_LIBRARIES cafeglsl::cafeglsl)
set(cafeglsl_FOUND TRUE)

unset(_cafeglsl_prefix)
