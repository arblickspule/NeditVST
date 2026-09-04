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
#
# nedit_ui_harness -- cross-platform UI-test harness (issue #2 scroll/pan). Runs
#                 the editor IN-PROCESS and dispatches synthetic mouse/wheel
#                 events into the live CFrame via the Debug-only test hooks,
#                 asserting the persisted sequencer viewport. Same per-OS parent
#                 window as editor_smoke; Debug-only (needs the test hooks).

if(NOT NEDIT_BUILD_EDITOR_SMOKE AND NOT NEDIT_BUILD_TOOLS AND NOT NEDIT_BUILD_UI_HARNESS)
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

# --- shared per-OS native parent-window glue ------------------------------
# editor_smoke and nedit_ui_harness both attach the editor to a real native
# window via editor_smoke_platform.h + this per-OS TU. Selected here so either
# tool can be built independently.
if(NEDIT_BUILD_EDITOR_SMOKE OR NEDIT_BUILD_UI_HARNESS)
    if(APPLE)
        enable_language(OBJCXX)
        set(SMOKE_PLATFORM_SOURCE editor_smoke_mac.mm)
    elseif(WIN32)
        set(SMOKE_PLATFORM_SOURCE editor_smoke_win.cpp)
    else()
        set(SMOKE_PLATFORM_SOURCE editor_smoke_linux.cpp)
    endif()
endif()

# --- editor_smoke (all platforms) -----------------------------------------
if(NEDIT_BUILD_EDITOR_SMOKE)
    # The smoke host drives the plugin via the same VST3 hosting helpers the
    # moduleinfotool uses; pick the per-platform module loader.
    if(APPLE)
        set(SMOKE_HOSTING_SOURCES
            ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module.cpp
            ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_mac.mm
            ${VST3_SDK_DIR}/public.sdk/source/vst/utility/stringconvert.cpp)
        set_source_files_properties(
            ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_mac.mm
            PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    elseif(WIN32)
        set(SMOKE_HOSTING_SOURCES
            ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module.cpp
            ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_win32.cpp
            ${VST3_SDK_DIR}/public.sdk/source/vst/utility/stringconvert.cpp)
        set_source_files_properties(
            ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_win32.cpp
            PROPERTIES COMPILE_OPTIONS "/Zc:char8_t-")
    else()
        set(SMOKE_HOSTING_SOURCES
            ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module.cpp
            ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_linux.cpp
            ${VST3_SDK_DIR}/public.sdk/source/vst/utility/stringconvert.cpp)
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

# --- nedit_ui_harness (all platforms, Debug hooks) ------------------------
# In-process editor + synthetic-event driver. Links nedit_plugin (which pulls
# VSTGUI + the SDK + the framework-free geometry) and reuses the smoke parent
# window; no VST3 hosting/module sources (nothing is loaded from a bundle).
if(NEDIT_BUILD_UI_HARNESS)
    add_executable(nedit_ui_harness
        nedit_ui_harness.cpp
        ${SMOKE_PLATFORM_SOURCE}
    )
    target_compile_options(nedit_ui_harness PRIVATE ${NEDIT_TOOL_SILENCE})
    # CRITICAL: match nedit_plugin's DEBUG define on Debug configs. VSTGUI's
    # vstguibase.h auto-defines NDEBUG unless DEBUG is set, which (a) toggles
    # CView::dumpInfo() in every VSTGUI vtable -- an ODR mismatch with the
    # plugin lib we link (see src/plugin/CMakeLists.txt) -- and (b) compiles
    # out the plugin's Debug-only test hooks this harness drives.
    target_compile_definitions(nedit_ui_harness PRIVATE $<$<CONFIG:Debug>:DEBUG>)
    # nedit_plugin PUBLIC-exports src/ + the VSTGUI include dir; that is all
    # the harness needs (plugin headers, events.h, cframe.h). nedit_plugin_entry
    # (OBJECT lib) supplies GetPluginFactory + moduleHandle, which the SDK's
    # VSTGUI ModuleInitializer / module-init reference -- the comment on that
    # target flags it as shared by the loadable module AND the test binaries.
    target_link_libraries(nedit_ui_harness
        PRIVATE nedit_plugin_entry nedit_plugin nedit_warnings)
    target_include_directories(nedit_ui_harness PRIVATE
        ${VST3_SDK_DIR} ${CMAKE_SOURCE_DIR}/src)
    if(LINUX)
        if(NOT SMOKE_XCB_FOUND)
            message(FATAL_ERROR "nedit_ui_harness needs libxcb (pkg-config xcb)")
        endif()
        target_link_libraries(nedit_ui_harness
            PRIVATE PkgConfig::SMOKE_XCB Threads::Threads ${CMAKE_DL_LIBS})
    elseif(WIN32)
        target_link_libraries(nedit_ui_harness PRIVATE user32 gdi32)
    elseif(APPLE)
        target_link_libraries(nedit_ui_harness PRIVATE
            "-framework Cocoa" "-framework CoreFoundation")
    endif()

    if(NEDIT_BUILD_TESTS)
        # Registered as a ctest; a headless runner with no display makes the
        # harness exit 77, which ctest treats as "skipped" (CI wraps ctest in
        # xvfb-run so it actually executes). Release builds also exit 77 (the
        # test hooks are compiled out).
        add_test(NAME ui_harness COMMAND nedit_ui_harness all)
        set_tests_properties(ui_harness PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
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