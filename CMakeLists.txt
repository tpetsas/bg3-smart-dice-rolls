cmake_minimum_required(VERSION 3.10)
set(CMAKE_CXX_STANDARD 20)

project(bg3-smart-dice-rolls)

# Set the DLL output directory
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Add the source files, including dllmain.cpp
add_library(smart-dice-rolls SHARED
    src/dllmain.cpp
    src/Logger.cpp
    src/Utils.cpp
    src/Config.cpp
    src/SmartDiceRolls.cpp
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

