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
    App/SelfTest
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
# Compile the real controller independently of Wave availability. An object
# library is not linked to production until an explicit real Wave source list
# is provided by the coordinator. No placeholder Wave function is supplied.
add_library(sweep_controller OBJECT App/SelfTest/sweep.c)
target_include_directories(sweep_controller PRIVATE App/Contracts App/SelfTest)
option(LAB2_ENABLE_SWEEP "Link real sweep with an explicitly integrated Wave implementation" OFF)
set(LAB2_WAVE_SOURCES "" CACHE STRING "Reviewed real Wave source paths, never test doubles")
set(LAB2_WAVE_INCLUDE_DIRS "" CACHE STRING "Reviewed real Wave include paths")
if(LAB2_ENABLE_SWEEP)
    if(NOT LAB2_WAVE_SOURCES)
        message(FATAL_ERROR "LAB2_ENABLE_SWEEP requires reviewed real LAB2_WAVE_SOURCES")
    endif()
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE $<TARGET_OBJECTS:sweep_controller>
        App/SelfTest/sweep_port_stm32.c ${LAB2_WAVE_SOURCES})
    target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${LAB2_WAVE_INCLUDE_DIRS})
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE LAB2_ENABLE_SWEEP=1)
    target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
        -Wl,--undefined=Sweep_Start -Wl,--undefined=Sweep_Cancel
        -Wl,--undefined=Sweep_GetSnapshot -Wl,--undefined=Sweep_GetDiagnostics)
endif()
if(LAB2_BUILD_BUSINESS_TESTS)
    add_executable(input_logic_tests
        tests/business/test_input.c tests/business/fake_input_port.c App/Input/input.c)
    target_include_directories(input_logic_tests PRIVATE App/Contracts App/Input tests/business)
    target_link_options(input_logic_tests PRIVATE -nostartfiles -Wl,-e,main
        -Wl,-Map=input_logic_tests.map)
    add_executable(sweep_logic_tests tests/business/test_sweep.c
        tests/business/fake_sweep_business.c App/SelfTest/sweep.c)
    target_include_directories(sweep_logic_tests PRIVATE App/Contracts App/SelfTest tests/business)
    target_link_options(sweep_logic_tests PRIVATE -nostartfiles -Wl,-e,main
        -Wl,-Map=sweep_logic_tests.map)
    foreach(scenario RANGE 1 6)
        add_executable(sweep_fault_${scenario} tests/business/test_sweep_faults.c
            tests/business/fake_sweep_business.c App/SelfTest/sweep.c)
        target_include_directories(sweep_fault_${scenario} PRIVATE App/Contracts App/SelfTest tests/business)
        target_compile_definitions(sweep_fault_${scenario} PRIVATE SCENARIO=${scenario})
        target_link_options(sweep_fault_${scenario} PRIVATE -nostartfiles -Wl,-e,main
            -Wl,-Map=sweep_fault_${scenario}.map)
    endforeach()
endif()
