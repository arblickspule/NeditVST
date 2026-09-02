# Dev/test tools for nedit.
#
# editor_smoke -- cross-platform editor-open smoke test used in CI
#                (Linux xcb / macOS NSView / Windows HWND). It loads a real
#                nedit.vst3 bundle through the VST3 hosting API and opens
#                the editor on a native parent window to prove open()
#                completes without crashing. Runs in the same build config
#                that a DAW would load (Debug matters: that is where the
#                #25 vtable-mismatch crash exists).
#
# host_harness -- interactive Linux X11 harness (xcb + XTest); needs a real
#                 display, so it is only built when NEDIT_BUILD_TOOLS is on.

if(NOT NEDIT_BUILD_EDITOR_SMOKE AND NOT NEDIT_BUILD_TOOLS)
    return()
endif()

if(MSVC)
    set(NEDIT_TOOL_SILENCE /w)
else()
    set(NEDIT_TOOL_SILENCE -w)
endif()

find_package(PkgConfig QUIET)
find_package(Threads REQUIRED)

if(LINUX)
    pkg_check_modules(SMOKE_XCB QUIET IMPORTED_TARGET xcb)
endif()

# --- editor_smoke (all platforms) -----------------------------------------
if(NEDIT_BUILD_EDITOR_SMOKE)
    # The smoke host drives the plugin via the same VST3 hosting helpers the
    # moduleinfotool uses; pick the per-platform module loader.
    if(APPLE)
        enable_language(OBJCXX)
        set(SMOKE_HOSTING_SOURCES
            ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module.cpp
            ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_mac.mm
            ${VST3_SDK_DIR}/public.sdk/source/vst/utility/stringconvert.cpp)
        set_source_files_properties(
            ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_mac.mm
            PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        set(SMOKE_PLATFORM_SOURCE editor_smoke_mac.mm)
    elseif(WIN32)
        set(SMOKE_HOSTING_SOURCES
            ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module.cpp
            ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_win32.cpp
            ${VST3_SDK_DIR}/public.sdk/source/vst/utility/stringconvert.cpp)
        set_source_files_properties(
            ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_win32.cpp
            PROPERTIES COMPILE_OPTIONS "/Zc:char8_t-")
        set(SMOKE_PLATFORM_SOURCE editor_smoke_win.cpp)
    else()
        set(SMOKE_HOSTING_SOURCES
            ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module.cpp
            ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_linux.cpp
            ${VST3_SDK_DIR}/public.sdk/source/vst/utility/stringconvert.cpp)
        set(SMOKE_PLATFORM_SOURCE editor_smoke_linux.cpp)
    endif()

    add_executable(nedit_editor_smoke
        editor_smoke.cpp
        ${SMOKE_PLATFORM_SOURCE}
        ${SMOKE_HOSTING_SOURCES}
    )
    target_compile_options(nedit_editor_smoke PRIVATE ${NEDIT_TOOL_SILENCE})
    target_link_libraries(nedit_editor_smoke
        PRIVATE nedit_vst3_sdk nedit_warnings)
    # Module/ClassInfo need the SDK headers + the plugin's static base lib.
    target_include_directories(nedit_editor_smoke PRIVATE
        ${VST3_SDK_DIR} ${CMAKE_SOURCE_DIR}/src)
    if(LINUX)
        if(NOT SMOKE_XCB_FOUND)
            message(FATAL_ERROR "editor_smoke needs libxcb (pkg-config xcb)")
        endif()
        target_link_libraries(nedit_editor_smoke
            PRIVATE PkgConfig::SMOKE_XCB Threads::Threads ${CMAKE_DL_LIBS})
    elseif(WIN32)
        target_link_libraries(nedit_editor_smoke PRIVATE user32 gdi32)
    elseif(APPLE)
        target_link_libraries(nedit_editor_smoke PRIVATE
            "-framework Cocoa" "-framework CoreFoundation")
    endif()
endif()

# --- host_harness (Linux interactive, needs a real X display) --------------
if(NEDIT_BUILD_TOOLS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
    pkg_check_modules(XTST REQUIRED IMPORTED_TARGET xtst)
    pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb)

    add_executable(nedit_host_harness
        host_harness.cpp
        # VST3 hosting helpers (Module, ClassInfo) -- same sources as
        # nedit_moduleinfotool but needed here for the host-side loader.
        ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module.cpp
        ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_linux.cpp
        ${VST3_SDK_DIR}/public.sdk/source/vst/utility/stringconvert.cpp
    )
    target_compile_options(nedit_host_harness PRIVATE ${NEDIT_TOOL_SILENCE})
    target_link_libraries(nedit_host_harness
        PRIVATE nedit_plugin PkgConfig::XCB PkgConfig::X11 PkgConfig::XTST
                nedit_vst3_sdk nedit_warnings)
    target_include_directories(nedit_host_harness PRIVATE ${CMAKE_SOURCE_DIR}/src)
endif()