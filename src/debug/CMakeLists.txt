# nedit_debug -- header-only debug/profiling utilities, compile-time
# excludable via NEDIT_DEBUG_TOOLS.
add_library(nedit_debug INTERFACE)
target_include_directories(nedit_debug INTERFACE ${CMAKE_SOURCE_DIR}/src)

if(NEDIT_DEBUG_TOOLS)
    target_compile_definitions(nedit_debug INTERFACE NEDIT_DEBUG_TOOLS=1)
endif()
