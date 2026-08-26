# host_harness -- standalone VST3 host that opens the editor in a real X11
# window for visual / interaction testing outside a DAW.

find_package(PkgConfig REQUIRED)
pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb)
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
pkg_check_modules(XTST REQUIRED IMPORTED_TARGET xtst)

add_executable(nedit_host_harness
    host_harness.cpp
    # VST3 hosting helpers (Module, ClassInfo) -- same sources as
    # nedit_moduleinfotool but needed here for the host-side loader.
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_linux.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/utility/stringconvert.cpp
)
target_compile_options(nedit_host_harness PRIVATE -w)
target_link_libraries(nedit_host_harness
    PRIVATE nedit_plugin PkgConfig::XCB PkgConfig::X11 PkgConfig::XTST
            nedit_vst3_sdk nedit_warnings)
target_include_directories(nedit_host_harness PRIVATE ${CMAKE_SOURCE_DIR}/src)
