cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME lab2_f407)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    BSP/Screen/tftlcd.c
    BSP/Screen/delay.c
    BSP/Screen/touch.c
    App/Screen/screen_app.c
    App/Screen/screen_calibration.c
    App/Screen/screen_selftest.c
    App/Mock/screen_mock.c
    App/Game/snake.c
    App/Input/input.c
    App/Input/input_port_stm32.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    BSP/Screen
    App/Screen
    App/Mock
    App/Game
    App/Contracts
    App/Input
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)

# Keep cursor snapshots shorter than an edge interval even in Debug; -g3 is
# preserved. Measurement arithmetic stays in main-loop input.c.
set_source_files_properties(App/Input/input_port_stm32.c PROPERTIES COMPILE_OPTIONS "-O2")
# Retain query/control entry points until the screen integration calls them.
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
    -Wl,--undefined=Input_GetSnapshot -Wl,--undefined=Input_BeginWindow
    -Wl,--undefined=Input_SetMode -Wl,--undefined=Input_GetMode
    -Wl,--undefined=Input_GetCapabilities)

option(LAB2_BUILD_BUSINESS_TESTS "Build isolated ARM logic tests with fake hardware" OFF)
if(LAB2_BUILD_BUSINESS_TESTS)
    add_executable(input_logic_tests
        tests/business/test_input.c tests/business/fake_input_port.c App/Input/input.c)
    target_include_directories(input_logic_tests PRIVATE App/Contracts App/Input tests/business)
    target_link_options(input_logic_tests PRIVATE -nostartfiles -Wl,-e,main
        -Wl,-Map=input_logic_tests.map)
endif()
