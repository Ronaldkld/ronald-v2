add_library(hexcore SHARED
    src/HexEditorCore.cpp
    src/HexDocument.cpp
    src/HexDocument.h
    src/Logger.cpp
    src/Logger.h
    src/Wiper.cpp
    src/Wiper.h
    include/hexcore/HexEditorCore.h

    # The GUI lives INSIDE this DLL: HexEditorCore.dll is the whole
    # editor, launchable with no separate .exe via:
    #   rundll32.exe HexEditorCore.dll,RunEditor
    src/gui/GuiEntry.cpp
    src/gui/MainWindow.cpp
    src/gui/MainWindow.h
    src/gui/HexGridControl.cpp
    src/gui/HexGridControl.h
    src/gui/FindDialog.cpp
    src/gui/FindDialog.h
    src/gui/TabBar.cpp
    src/gui/TabBar.h
    resources/resource.rc
    resources/resource.h
)

target_include_directories(hexcore PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_include_directories(hexcore PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/resources
)

target_compile_definitions(hexcore PRIVATE HEXCORE_BUILD_DLL UNICODE _UNICODE)
target_compile_features(hexcore PRIVATE cxx_std_17)

if (MINGW)
    target_link_options(hexcore PRIVATE -static-libgcc -static-libstdc++ -static
        -Wl,--kill-at) # strip stdcall '@N' decoration so rundll32's
                        # "dllname,RunEditor" syntax finds the export
endif()

target_link_libraries(hexcore PRIVATE kernel32 user32 gdi32 comctl32 comdlg32 shell32 ole32)

set_target_properties(hexcore PROPERTIES
    OUTPUT_NAME "HexEditorCore"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
)

option(HEXEDITOR_BUILD_CORE_TESTS "Build the manual runtime test for hexcore" ON)
if (HEXEDITOR_BUILD_CORE_TESTS AND WIN32)
    add_executable(hexcore_manual_test tests/manual_test.c)
    target_link_libraries(hexcore_manual_test PRIVATE hexcore)
    target_compile_definitions(hexcore_manual_test PRIVATE UNICODE _UNICODE)
    if (MINGW)
        target_link_options(hexcore_manual_test PRIVATE -municode -static-libgcc -static-libstdc++)
    endif()
    set_target_properties(hexcore_manual_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
endif()
