# nedit_plugin -- the Phase 3 VST3 shell.
#
# The Steinberg SDK is fetched but NOT configured through its own CMake
# (its SMTG machinery builds samples/VSTGUI/etc. we don't want); instead a
# minimal static library compiles exactly the sources this plugin needs,
# mirroring cmake/modules/SMTG_VST3_SDK.cmake's target lists.

include(FetchContent)

FetchContent_Declare(vst3sdk
    GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
    GIT_TAG        3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(vst3sdk)
if(NOT vst3sdk_POPULATED)
    FetchContent_Populate(vst3sdk)
endif()

set(VST3_SDK_DIR ${vst3sdk_SOURCE_DIR} CACHE PATH "VST3 SDK source root")

# Patch VSTGUI (Linux only): keep the xcb connection alive across editor
# close/open so cairo-xcb's process-global connection cache doesn't go stale
# and crash the next open() in cairo_xcb_surface_create (upstream VSTGUI
# #249/#334/#335). Idempotent: only applied when the marker comment is absent.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Git REQUIRED)
    set(_vstgui_x11platform "${VST3_SDK_DIR}/vstgui4/vstgui/lib/platform/linux/x11platform.cpp")
    if(EXISTS "${_vstgui_x11platform}")
        file(READ "${_vstgui_x11platform}" _vstgui_platform_src)
        if(NOT _vstgui_platform_src MATCHES "Keep the xcb connection")
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" -C "${VST3_SDK_DIR}/vstgui4" apply
                        "${CMAKE_CURRENT_LIST_DIR}/../../cmake/vstgui-x11-reopen-crash.patch"
                RESULT_VARIABLE _vstgui_patch_result
                ERROR_VARIABLE _vstgui_patch_err)
            if(NOT _vstgui_patch_result EQUAL 0)
                message(FATAL_ERROR
                    "Failed to apply VSTGUI reopen-crash patch: ${_vstgui_patch_err}")
            endif()
        endif()
    endif()
endif()

# Third-party code: silence ITS warnings (ours stay fully enabled everywhere
# else; SDK headers are SYSTEM-included so they never leak into consumer TUs).
# MSVC spells the all-warnings-off switch "/w"; GCC/Clang use "-w".
if(MSVC)
    set(NEDIT_SILENCE_WARNINGS /w)
else()
    set(NEDIT_SILENCE_WARNINGS -w)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(NEDIT_IS_LINUX TRUE)
else()
    set(NEDIT_IS_LINUX FALSE)
endif()

# Platform-specific SDK TUs (vst3sdk compiles one variant of each; the .mm
# files on Apple need the OBJCXX language).
if(APPLE)
    enable_language(OBJCXX)
    set(NEDIT_PLATFORM_SDK_SOURCES
        ${VST3_SDK_DIR}/public.sdk/source/common/systemclipboard_mac.mm
        ${VST3_SDK_DIR}/public.sdk/source/common/threadchecker_mac.mm)
elseif(WIN32)
    set(NEDIT_PLATFORM_SDK_SOURCES
        ${VST3_SDK_DIR}/public.sdk/source/common/systemclipboard_win32.cpp
        ${VST3_SDK_DIR}/public.sdk/source/common/threadchecker_win32.cpp)
else()
    set(NEDIT_PLATFORM_SDK_SOURCES
        ${VST3_SDK_DIR}/public.sdk/source/common/systemclipboard_linux.cpp
        ${VST3_SDK_DIR}/public.sdk/source/common/threadchecker_linux.cpp)
endif()

add_library(nedit_vst3_sdk STATIC
    # pluginterfaces ----------------------------------------------------------
    ${VST3_SDK_DIR}/pluginterfaces/base/conststringtable.cpp
    ${VST3_SDK_DIR}/pluginterfaces/base/coreiids.cpp
    ${VST3_SDK_DIR}/pluginterfaces/base/funknown.cpp
    ${VST3_SDK_DIR}/pluginterfaces/base/ustring.cpp

    # base --------------------------------------------------------------------
    ${VST3_SDK_DIR}/base/source/baseiids.cpp
    ${VST3_SDK_DIR}/base/source/fbuffer.cpp
    ${VST3_SDK_DIR}/base/source/fdebug.cpp
    ${VST3_SDK_DIR}/base/source/fdynlib.cpp
    ${VST3_SDK_DIR}/base/source/fobject.cpp
    ${VST3_SDK_DIR}/base/source/fstreamer.cpp
    ${VST3_SDK_DIR}/base/source/fstring.cpp
    ${VST3_SDK_DIR}/base/source/timer.cpp
    ${VST3_SDK_DIR}/base/source/updatehandler.cpp
    ${VST3_SDK_DIR}/base/thread/source/fcondition.cpp
    ${VST3_SDK_DIR}/base/thread/source/flock.cpp

    # public.sdk/common ---------------------------------------------------------
    ${VST3_SDK_DIR}/public.sdk/source/common/commoniids.cpp
    ${VST3_SDK_DIR}/public.sdk/source/common/commonstringconvert.cpp
    ${VST3_SDK_DIR}/public.sdk/source/common/memorystream.cpp
    ${VST3_SDK_DIR}/public.sdk/source/common/openurl.cpp
${VST3_SDK_DIR}/public.sdk/source/common/readfile.cpp
    ${NEDIT_PLATFORM_SDK_SOURCES}

    # public.sdk/main + vst classes ---------------------------------------------
    ${VST3_SDK_DIR}/public.sdk/source/common/pluginview.cpp
    ${VST3_SDK_DIR}/public.sdk/source/main/pluginfactory.cpp
    ${VST3_SDK_DIR}/public.sdk/source/main/moduleinit.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/vstaudioeffect.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/vstbus.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/vstcomponent.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/vstcomponentbase.cpp
    # NOTE: do NOT add vsteditcontroller.cpp here -- it is #included by
    # vstsinglecomponenteffect.cpp below, and compiling both duplicates
    # every EditController/ProgramList symbol.
    ${VST3_SDK_DIR}/public.sdk/source/vst/vstinitiids.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/vstparameters.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/vstsinglecomponenteffect.cpp
)

target_compile_features(nedit_vst3_sdk PUBLIC cxx_std_17)
# The loadable module needs position-independent code all the way down.
set_target_properties(nedit_vst3_sdk PROPERTIES POSITION_INDEPENDENT_CODE ON)
# fdebug.h refuses to compile without an explicit build-flavour macro.
target_compile_definitions(nedit_vst3_sdk PUBLIC RELEASE=1)
target_compile_options(nedit_vst3_sdk PRIVATE ${NEDIT_SILENCE_WARNINGS})
target_include_directories(nedit_vst3_sdk SYSTEM INTERFACE ${VST3_SDK_DIR})
target_include_directories(nedit_vst3_sdk SYSTEM PUBLIC ${VST3_SDK_DIR})
find_package(Threads REQUIRED)
target_link_libraries(nedit_vst3_sdk PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

# ---------------------------------------------------------------------------
# VSTGUI (Phase 4b editor). Built via its own CMake as a subdirectory;
# only the core `vstgui` library target is linked (no uidescription XML,
# no standalone, no tools).
# ---------------------------------------------------------------------------
set(VSTGUI_STANDALONE OFF CACHE BOOL "" FORCE)
set(VSTGUI_TOOLS OFF CACHE BOOL "" FORCE)
set(VSTGUI_UISCRIPTING OFF CACHE BOOL "" FORCE)
set(VSTGUI_ENABLE_XMLPARSER OFF CACHE BOOL "" FORCE)
add_subdirectory(${VST3_SDK_DIR}/vstgui4/vstgui
                 ${CMAKE_CURRENT_BINARY_DIR}/vstgui EXCLUDE_FROM_ALL)
# Third-party code: its warnings are its own business.
target_compile_options(vstgui PRIVATE ${NEDIT_SILENCE_WARNINGS})
set_target_properties(vstgui PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Our shell code (warnings apply here -- SDK headers are SYSTEM-included).
# factory.cpp is deliberately NOT here: it defines GetPluginFactory and is
# owned by the loadable module target below.
add_library(nedit_plugin STATIC
    NeditProcessor.cpp
    NeditEditor.cpp
    WaveformView.cpp
    WavDecoder.cpp
    # SDK's VSTGUI editor base + (Linux only) the runloop bridge; compiled
    # here (not in nedit_vst3_sdk) because they need VSTGUI headers. The
    # Linux runloop glue has no mac/win counterpart -- the Cocoa / Windows
    # VSTGUI backends hook the host run loop themselves.
    ${VST3_SDK_DIR}/public.sdk/source/vst/vstguieditor.cpp
)
if(NEDIT_IS_LINUX)
    target_sources(nedit_plugin PRIVATE
        ${VST3_SDK_DIR}/public.sdk/source/vst/vstgui_linux_runloop_support.cpp)
endif()
target_include_directories(nedit_plugin PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(nedit_plugin SYSTEM PUBLIC ${VST3_SDK_DIR}/vstgui4)
set_target_properties(nedit_plugin PROPERTIES POSITION_INDEPENDENT_CODE ON)
# CRITICAL on Linux: without this, vstguieditor.cpp compiles the branch
# that never calls VSTGUI::init() nor registers the host-context callback
# that wires up the Linux runloop -- leaving VSTGUI's xcb connection null
# and crashing in xcb_generate_id() the instant the editor opens. The
# SDK's own plugin cmake helpers define this; we bypass them, so set it
# ourselves.
target_compile_definitions(nedit_plugin PRIVATE VSTGUI_NEEDS_INIT_LIB=1)
target_compile_definitions(nedit_plugin PRIVATE NEDIT_BUILD_ID=\"${NEDIT_BUILD_ID}\")
# VSTGUI's own build (vstgui/cmake/modules/vstgui_init.cmake) defines DEBUG
# for itself on Debug configs, which adds CView::dumpInfo() -- a `#if DEBUG`
# virtual method -- to every VSTGUI class's vtable. Our own sources here
# (NeditEditor.cpp, WaveformView.cpp) construct and call methods on those
# same VSTGUI classes, so if THEY don't see DEBUG too, they compute a
# DIFFERENT (one-slot-short) vtable layout for the identical classes: an
# ODR violation where every virtual call after that point (setFont,
# setFontColor, setBackColor, ...) silently invokes the wrong function
# through a real-but-mismatched vtable slot. Match VSTGUI's own condition
# exactly so both sides agree.
target_compile_definitions(nedit_plugin PRIVATE $<$<CONFIG:Debug>:DEBUG>)
if(MSVC)
    # getenv/strcpy & co. are C4996-deprecated on MSVC's /W4; these are
    # deliberate ports of POSIX patterns.
    target_compile_definitions(nedit_plugin PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
# vstguieditor.cpp is SDK code compiled inside OUR target: exempt those SDK
# TUs from the project warning set.
set_source_files_properties(
    ${VST3_SDK_DIR}/public.sdk/source/vst/vstguieditor.cpp
    PROPERTIES COMPILE_OPTIONS "${NEDIT_SILENCE_WARNINGS}")
if(NEDIT_IS_LINUX)
    set_source_files_properties(
        ${VST3_SDK_DIR}/public.sdk/source/vst/vstgui_linux_runloop_support.cpp
        PROPERTIES COMPILE_OPTIONS "${NEDIT_SILENCE_WARNINGS}")
endif()
target_link_libraries(nedit_plugin
    PUBLIC nedit_vst3_sdk nedit_engine nedit_state nedit_debug nedit_ui vstgui
    PRIVATE nedit_warnings)

# ---------------------------------------------------------------------------
# .vst3 bundle assembly.
# ---------------------------------------------------------------------------
set(NEDIT_VST3_BUNDLE_DIR ${CMAKE_BINARY_DIR}/nedit.vst3 CACHE PATH
    "Where to assemble the .vst3 bundle")

# Module entry TUs shared by the loadable module AND the test binaries:
# factory.cpp defines GetPluginFactory, the platform main defines the
# module entry/exit + moduleHandle. As an OBJECT library the symbols can
# never be dropped by archive resolution, and tests get real module
# semantics (VSTGUI's ModuleInitializer references GetPluginFactory).
if(APPLE)
    set(NEDIT_MODULE_ENTRY ${VST3_SDK_DIR}/public.sdk/source/main/macmain.cpp)
elseif(WIN32)
    set(NEDIT_MODULE_ENTRY ${VST3_SDK_DIR}/public.sdk/source/main/dllmain.cpp)
else()
    set(NEDIT_MODULE_ENTRY ${VST3_SDK_DIR}/public.sdk/source/main/linuxmain.cpp)
endif()

add_library(nedit_plugin_entry OBJECT
    factory.cpp
    ${NEDIT_MODULE_ENTRY}
)
target_link_libraries(nedit_plugin_entry PRIVATE nedit_plugin nedit_warnings)
set_target_properties(nedit_plugin_entry PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_source_files_properties(
    ${NEDIT_MODULE_ENTRY}
    PROPERTIES COMPILE_OPTIONS "${NEDIT_SILENCE_WARNINGS}")
if(APPLE)
    # macmain.cpp uses CoreFoundation to keep the loaded bundle alive.
    target_link_libraries(nedit_plugin_entry PRIVATE "-framework CoreFoundation")
endif()

# The loadable module. nedit_plugin is linked WHOLE so the static
# initializer-driven factory and module glue are never dropped by archive
# symbol resolution. Each link line spells the whole-archive switch for its
# platform ($<TARGET_FILE> gives MSVC the resolved .lib path; Apple uses
# ld's -force_load; GCC-family keeps the inline pass-through).
add_library(nedit_vst3 MODULE
    $<TARGET_OBJECTS:nedit_plugin_entry>
)
if(APPLE)
    target_link_libraries(nedit_vst3 PRIVATE
        nedit_plugin "-Wl,-force_load,$<TARGET_FILE:nedit_plugin>")
elseif(WIN32)
    target_link_libraries(nedit_vst3 PRIVATE nedit_plugin)
    # Hosts enter via GetPluginFactory on Windows -- force-export it even
    # though factory.cpp already marks it SMTG_EXPORT_SYMBOL. The
    # whole-archive include must go through target_link_options: CMake's
    # link-LIBRARY path handling rewrites the leading '/' + drive path
    # into a rooted filename (LNK1104). Link options are emitted raw.
    target_link_options(nedit_vst3 PRIVATE
        "/WHOLEARCHIVE:$<TARGET_FILE:nedit_plugin>"
        "/EXPORT:GetPluginFactory")
else()
    target_link_libraries(nedit_vst3
        PRIVATE -Wl,--whole-archive nedit_plugin -Wl,--no-whole-archive)
endif()
set_target_properties(nedit_vst3 PROPERTIES PREFIX "" OUTPUT_NAME "nedit")

# .vst3 bundle assembly. The VST3 spec layout is a directory the host scans:
#   <bundle>.vst3/Contents/<arch-dir>/<bundle>[.so|.vst3]
# plus Contents/Resources/moduleinfo.json (Linux hosts read it; the
# SDK's own SMTG only generates it for Linux). On mac CMake itself builds
# the bundle wrapper (MODULE + BUNDLE -> nedit.vst3/Contents/MacOS/nedit
# + Info.plist + PkgInfo), so we only sign it afterwards.
if(APPLE)
    set_target_properties(nedit_vst3 PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION vst3
        # CMake would otherwise put the bundle in the target's own binary
        # dir (build/src/plugin/nedit.vst3) so the artifact upload / deploy
        # path would not match Linux/Windows. A MODULE+bundle wrapper is
        # placed via LIBRARY_OUTPUT_DIRECTORY (BUNDLE_OUTPUT_DIRECTORY only
        # governs true MACOSX_BUNDLE executables), so pin both to the
        # shared bundle dir; this root must be the build dir itself (not a
        # pre-suffixed ".../nedit.vst3"), otherwise CMake appends another
        # "nedit.vst3" target folder and creates a nested bundle path.
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
        LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}
        BUNDLE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
        BUNDLE_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}
    )
    # Ad-hoc sign the whole bundle: unsigned MH_BUNDLE binaries refuse to
    # dlopen on Apple Silicon.
    add_custom_command(TARGET nedit_vst3 POST_BUILD
        COMMAND codesign --force --deep --sign -
                $<TARGET_BUNDLE_DIR:nedit_vst3>
        COMMENT "Ad-hoc signing nedit.vst3 bundle")
elseif(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(NEDIT_VST3_ARCH_DIR "x86_64-win")
    else()
        set(NEDIT_VST3_ARCH_DIR "x86-win")
    endif()
    add_custom_command(TARGET nedit_vst3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                ${NEDIT_VST3_BUNDLE_DIR}/Contents/${NEDIT_VST3_ARCH_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy
                $<TARGET_FILE:nedit_vst3>
                ${NEDIT_VST3_BUNDLE_DIR}/Contents/${NEDIT_VST3_ARCH_DIR}/nedit.vst3
        COMMENT "Assembling nedit.vst3 bundle in ${NEDIT_VST3_BUNDLE_DIR}")
else()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(NEDIT_VST3_ARCH_DIR "aarch64-linux")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(i.86|x86)$")
        set(NEDIT_VST3_ARCH_DIR "i386-linux")
    else()
        set(NEDIT_VST3_ARCH_DIR "x86_64-linux")
    endif()
    add_custom_command(TARGET nedit_vst3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                ${NEDIT_VST3_BUNDLE_DIR}/Contents/${NEDIT_VST3_ARCH_DIR}
        COMMAND ${CMAKE_COMMAND} -E make_directory
                ${NEDIT_VST3_BUNDLE_DIR}/Contents/Resources
        # VST3 spec: the Linux binary MUST be named <bundle>.so -- hosts
        # match it against the .vst3 folder name ("Not a plug-in file"
        # otherwise). Stripped: VSTGUI's own build forces debug info
        # regardless of our CMAKE_BUILD_TYPE, which otherwise ships a
        # 20+MB unstripped binary (large enough to trip some hosts'
        # fast-metadata-scan timeouts).
        COMMAND ${CMAKE_COMMAND} -E copy
                $<TARGET_FILE:nedit_vst3>
                ${NEDIT_VST3_BUNDLE_DIR}/Contents/${NEDIT_VST3_ARCH_DIR}/nedit.so
        COMMAND ${CMAKE_STRIP} --strip-unneeded
                ${NEDIT_VST3_BUNDLE_DIR}/Contents/${NEDIT_VST3_ARCH_DIR}/nedit.so
        # Some hosts (Bitwig) require Contents/Resources/moduleinfo.json;
        # generate it from the real factory of the just-built module.
        COMMAND nedit_moduleinfotool
                ${NEDIT_VST3_BUNDLE_DIR}
                ${NEDIT_VST3_BUNDLE_DIR}/Contents/Resources/moduleinfo.json
        COMMENT "Assembling nedit.vst3 bundle in ${NEDIT_VST3_BUNDLE_DIR}")
endif()

if(NEDIT_IS_LINUX)
    # Host-side generator for moduleinfo.json (runs at build time; Linux
    # only -- the hosting module_linux.cpp is a Linux port).
    add_executable(nedit_moduleinfotool
        moduleinfotool.cpp
        ${VST3_SDK_DIR}/public.sdk/source/vst/moduleinfo/moduleinfocreator.cpp
        ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module.cpp
        ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_linux.cpp
        ${VST3_SDK_DIR}/public.sdk/source/vst/utility/stringconvert.cpp
    )
    target_compile_options(nedit_moduleinfotool PRIVATE ${NEDIT_SILENCE_WARNINGS})
    target_link_libraries(nedit_moduleinfotool PRIVATE nedit_vst3_sdk)
    add_dependencies(nedit_vst3 nedit_moduleinfotool)
endif()

if(NEDIT_IS_LINUX)
    # Convenience: install the assembled bundle into the user VST3 directory
    # as a REAL bundle (hosts don't reliably follow symlinked bundles, and a
    # real dir in ~/.vst3 is scanned VST3-only -- no VST2 "Not a plug-in
    # file" noise from probing the inner .so). `cmake --build . --target deploy`
    set(NEDIT_USER_VST3_DIR "$ENV{HOME}/.vst3" CACHE PATH
        "Where `deploy` installs the .vst3 bundle")
    add_custom_target(deploy
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${NEDIT_USER_VST3_DIR}/nedit.vst3
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${NEDIT_VST3_BUNDLE_DIR} ${NEDIT_USER_VST3_DIR}/nedit.vst3
        DEPENDS nedit_vst3
        COMMENT "Deploying nedit.vst3 to ${NEDIT_USER_VST3_DIR}")
endif()

if(APPLE)
    # Convenience: install the assembled bundle into the user VST3 directory
    # as a REAL bundle. macOS builds make the .vst3/Contents/MacOS/<name>
    # MH_BUNDLE directly; the raw MODULE artifact (nedit.so) is NOT a
    # loadable plugin and Ableton scans the complete bundle, so always ship
    # the directory this target copies.
    # `cmake --build . --target deploy`
    set(NEDIT_USER_VST3_DIR "$ENV{HOME}/Library/Audio/Plug-Ins/VST3" CACHE PATH
        "Where `deploy` installs the .vst3 bundle")
    add_custom_target(deploy
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${NEDIT_USER_VST3_DIR}/nedit.vst3
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${NEDIT_VST3_BUNDLE_DIR} ${NEDIT_USER_VST3_DIR}/nedit.vst3
        DEPENDS nedit_vst3
        COMMENT "Deploying nedit.vst3 to ${NEDIT_USER_VST3_DIR}")
endif()

message(STATUS "Nedit: plugin target enabled (SDK: ${VST3_SDK_DIR})")
