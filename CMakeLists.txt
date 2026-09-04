cmake_minimum_required(VERSION 3.24)

project(Nedit
    VERSION 0.1.0
    DESCRIPTION "Nedit - generative sample slicer (native VST3 rewrite)"
    LANGUAGES CXX)

find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE NEDIT_BUILD_ID
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()
if(NOT NEDIT_BUILD_ID)
    set(NEDIT_BUILD_ID "unknown")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# The VST3 module is a shared object; every static lib feeding it needs PIC.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Left unset, VSTGUI's own CMakeLists.txt (fetched into src/plugin) falls
# back to CMAKE_BUILD_TYPE=Debug for ITSELF, but that fallback is
# directory-scoped -- it never reaches our own targets. VSTGUI then
# compiles with DEBUG defined (adding CView::dumpInfo(), an `#if DEBUG`
# virtual method) while our own TUs (NeditEditor.cpp etc.) do not see
# DEBUG at all, so the two sides compute different vtable layouts for
# every VSTGUI class -- an ODR violation that misdirects every virtual
# call our code makes into a neighboring, wrong vtable slot the moment a
# CParamDisplay-derived control (COptionMenu, CTextLabel, ...) is used.
# Pin one build type up front so both sides agree.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(NEDIT_BUILD_TESTS    "Build unit tests"                         ON)
option(NEDIT_BUILD_PLUGIN   "Build the VST3 plugin (fetches VST3 SDK)"  OFF)
option(NEDIT_BUILD_TOOLS    "Build the Linux X11 host harness"         ON)
option(NEDIT_BUILD_EDITOR_SMOKE "Build the cross-platform editor smoke test" ON)
option(NEDIT_DEBUG_TOOLS    "Compile in debug/profiling utilities"      ON)
option(NEDIT_MAC_UNIVERSAL  "Build a universal (arm64 + x86_64) macOS plugin" ON)

# Universal macOS binary: one .vst3 that loads on both Apple Silicon and
# Intel Macs. Applied only when the caller did not pin
# CMAKE_OSX_ARCHITECTURES explicitly; override with -DNEDIT_MAC_UNIVERSAL=OFF
# (then clear CMAKE_OSX_ARCHITECTURES to fall back to the host arch).
if(APPLE AND NEDIT_MAC_UNIVERSAL AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING
        "Build architectures for macOS (set by NEDIT_MAC_UNIVERSAL)" FORCE)
endif()

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
    # Dev/test tools: editor_smoke is cross-platform (CI), host_harness is
    # an interactive Linux-only X11 window driver.
    if(NEDIT_BUILD_EDITOR_SMOKE OR NEDIT_BUILD_TOOLS)
        add_subdirectory(tools)
    endif()
endif()

if(NEDIT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
