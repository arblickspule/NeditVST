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
