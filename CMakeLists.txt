cmake_minimum_required(VERSION 3.25)

# Project configuration
project(mps-info C)

# Use clang as the C compiler
if (NOT CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER clang)
endif()

# Require C11 standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Export compile commands (compile_commands.json) for tooling like clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_definitions(
    _CRT_SECURE_NO_WARNINGS
)
# Add external scanivalve-mps-protocol interface library
add_subdirectory(external/scanivalve-mps-protocol)


# Main executable
add_executable(mps-info
    src/main.c
)

# Link against scanivalve-mps-protocol so its include directories are visible to this target
target_link_libraries(mps-info PRIVATE scanivalve-mps-protocol)

