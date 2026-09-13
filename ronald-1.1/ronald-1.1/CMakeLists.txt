cmake_minimum_required(VERSION 3.16)
project(WinHexEditor LANGUAGES CXX C)

if (NOT WIN32)
    message(WARNING
        "This project targets Windows (the DLL uses the Win32 API directly). "
        "Configure with -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake "
        "to cross-compile real Windows binaries from a non-Windows host.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(HEXEDITOR_BUILD_EXAMPLES "Build the console example host" ON)

# The GUI is built INTO HexEditorCore.dll (see core/CMakeLists.txt) and
# launched via "rundll32.exe HexEditorCore.dll,RunEditor" - there is no
# separate GUI .exe target.
add_subdirectory(core)

if (HEXEDITOR_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
