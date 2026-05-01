cmake_minimum_required(VERSION 3.20)
set(CMAKE_CXX_STANDARD 20)

project(bg3-smart-dice-rolls)

# Submodule: o-negative (xinput1_4.dll proxy)
set(BUILD_XINPUT_PROXY ON CACHE BOOL "Build xinput1_4 proxy" FORCE)
add_subdirectory(src/o-negative)

# Submodule: PixelsCpp (PixelsTray.exe tray app)
add_subdirectory(src/PixelsCpp)

# Main mod DLL
add_library(smart-dice-rolls SHARED
    src/dllmain.cpp
    src/Logger.cpp
    src/Utils.cpp
    src/Config.cpp
    src/SmartDiceRolls.cpp
    src/PixelsDiceClient.cpp
    src/minhook/src/buffer.c
    src/minhook/src/hook.c
    src/minhook/src/trampoline.c
    src/minhook/src/hde/hde32.c
    src/minhook/src/hde/hde64.c
    src/rva/sscan/Pattern.cpp
)

target_link_libraries(smart-dice-rolls)

target_include_directories(smart-dice-rolls PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

# Put all key outputs in the same directory
set_target_properties(smart-dice-rolls PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

set_target_properties(PixelsTray PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

set_target_properties(xinput1_4 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

