cmake_minimum_required(VERSION 3.25)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" PROJECT_VERSION_STRING)
string(STRIP "${PROJECT_VERSION_STRING}" PROJECT_VERSION_STRING)

# Project configuration
project(mps-info
    VERSION ${PROJECT_VERSION_STRING}
    LANGUAGES C RC
)

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

# Generated files directory
set(MPS_INFO_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY ${MPS_INFO_GENERATED_DIR})

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.h.in
    ${MPS_INFO_GENERATED_DIR}/version.h
)

# Main executable
add_executable(mps-info
    src/main.c
)

# Add generated include dir so version.h can be included
target_include_directories(mps-info PRIVATE
    ${MPS_INFO_GENERATED_DIR}
)

# Generate and attach Windows Version resource
if (WIN32)
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/resources.rc.in
        ${MPS_INFO_GENERATED_DIR}/resources.rc
        @ONLY
    )

    target_sources(mps-info PRIVATE
        ${MPS_INFO_GENERATED_DIR}/resources.rc
    )
endif()
        

# Link against scanivalve-mps-protocol so its include directories are visible to this target
target_link_libraries(mps-info PRIVATE scanivalve-mps-protocol)

install(TARGETS mps-info RUNTIME DESTINATION .)
