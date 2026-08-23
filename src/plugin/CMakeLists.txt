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

set(VST3_SDK_DIR ${vst3sdk_SOURCE_DIR})

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
    ${VST3_SDK_DIR}/public.sdk/source/common/systemclipboard_linux.cpp
    ${VST3_SDK_DIR}/public.sdk/source/common/threadchecker_linux.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/vstpresetfile.cpp

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
# Third-party code: silence ITS warnings (ours stay fully enabled
# everywhere else); SDK headers are SYSTEM-included so they never leak
# into consumer TUs either.
target_compile_options(nedit_vst3_sdk PRIVATE -w)
target_include_directories(nedit_vst3_sdk SYSTEM INTERFACE ${VST3_SDK_DIR})
target_include_directories(nedit_vst3_sdk SYSTEM PUBLIC ${VST3_SDK_DIR})
find_package(Threads REQUIRED)
target_link_libraries(nedit_vst3_sdk PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

# Our shell code (warnings apply here -- SDK headers are SYSTEM-included).
# factory.cpp is deliberately NOT here: it defines GetPluginFactory and is
# owned by the loadable module target below.
add_library(nedit_plugin STATIC
    NeditProcessor.cpp
)
target_include_directories(nedit_plugin PUBLIC ${CMAKE_SOURCE_DIR}/src)
set_target_properties(nedit_plugin PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(nedit_plugin
    PUBLIC nedit_vst3_sdk nedit_engine nedit_state nedit_debug
    PRIVATE nedit_warnings)

# ---------------------------------------------------------------------------
# .vst3 bundle assembly (Linux layout; other platforms arrive with their
# build hosts in Phase 5).
# ---------------------------------------------------------------------------
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(NEDIT_VST3_ARCH_DIR "aarch64-linux")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(i.86|x86)$")
    set(NEDIT_VST3_ARCH_DIR "i386-linux")
else()
    set(NEDIT_VST3_ARCH_DIR "x86_64-linux")
endif()

set(NEDIT_VST3_BUNDLE_DIR ${CMAKE_BINARY_DIR}/nedit.vst3 CACHE PATH
    "Where to assemble the .vst3 bundle")

# The loadable module. factory.cpp is the module's own TU (it carries the
# class-factory definitions); nedit_plugin is linked WHOLE so the static
# initializer-driven factory and module glue are never dropped by archive
# symbol resolution.
add_library(nedit_vst3 MODULE
    factory.cpp
)
target_link_libraries(nedit_vst3
    PRIVATE -Wl,--whole-archive nedit_plugin -Wl,--no-whole-archive)
set_target_properties(nedit_vst3 PROPERTIES PREFIX "" OUTPUT_NAME "nedit")

add_custom_command(TARGET nedit_vst3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            ${NEDIT_VST3_BUNDLE_DIR}/Contents/${NEDIT_VST3_ARCH_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:nedit_vst3>
            ${NEDIT_VST3_BUNDLE_DIR}/Contents/${NEDIT_VST3_ARCH_DIR}/libnedit.vst.so
    COMMENT "Assembling nedit.vst3 bundle in ${NEDIT_VST3_BUNDLE_DIR}")

message(STATUS "Nedit: plugin target enabled (SDK: ${VST3_SDK_DIR})")
