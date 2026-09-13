# This example intentionally does NOT link against hexcore's import
# library: it demonstrates loading HexEditorCore.dll dynamically at
# runtime via LoadLibrary/GetProcAddress, as any third-party host
# (regardless of language/toolchain) would.
add_executable(console_host console_host.c)
target_include_directories(console_host PRIVATE ${CMAKE_SOURCE_DIR}/core/include)
target_compile_definitions(console_host PRIVATE UNICODE _UNICODE)

if (MINGW)
    target_link_options(console_host PRIVATE -municode -static-libgcc)
endif()

set_target_properties(console_host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

add_dependencies(console_host hexcore)
