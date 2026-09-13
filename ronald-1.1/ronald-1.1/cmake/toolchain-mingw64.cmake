# Cross-compilation toolchain for building genuine Windows x64 PE
# binaries from Linux using MinGW-w64.
#
# Usage:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake
#   cmake --build build

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)

find_program(CMAKE_C_COMPILER   NAMES ${TOOLCHAIN_PREFIX}-gcc)
find_program(CMAKE_CXX_COMPILER NAMES ${TOOLCHAIN_PREFIX}-g++)
find_program(CMAKE_RC_COMPILER  NAMES ${TOOLCHAIN_PREFIX}-windres)
find_program(CMAKE_AR           NAMES ${TOOLCHAIN_PREFIX}-ar)

set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(CMAKE_RC_COMPILER_INIT ${TOOLCHAIN_PREFIX}-windres)
set(CMAKE_C_FLAGS_INIT "" CACHE STRING "")
