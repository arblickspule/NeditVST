cmake_minimum_required(VERSION 3.24)

project(Nedit
    VERSION 0.1.0
    DESCRIPTION "Nedit - generative sample slicer (native VST3 rewrite)"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# The VST3 module is a shared object; every static lib feeding it needs PIC.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(NEDIT_BUILD_TESTS   "Build unit tests"                          ON)
option(NEDIT_BUILD_PLUGIN  "Build the VST3 plugin (fetches VST3 SDK)"  OFF)
option(NEDIT_DEBUG_TOOLS   "Compile in debug/profiling utilities"      ON)

# ---------------------------------------------------------------------------
# Warnings (applied to our own targets via nedit_warnings interface lib)
# ---------------------------------------------------------------------------
add_library(nedit_warnings INTERFACE)
if(MSVC)
    target_compile_options(nedit_warnings INTERFACE /W4 /permissive-)
else()
    target_compile_options(nedit_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
add_subdirectory(src/state)
add_subdirectory(src/engine)
add_subdirectory(src/ui)
add_subdirectory(src/debug)

if(NEDIT_BUILD_PLUGIN)
    # Phase 4: VST3 SDK + VSTGUI via FetchContent. See AGENTS.md roadmap.
    # Added BEFORE tests/ so test targets can reference the entry objects.
    add_subdirectory(src/plugin)
endif()

if(NEDIT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
