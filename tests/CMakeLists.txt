include(FetchContent)
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)

add_executable(nedit_state_tests
    state/test_types.cpp
    state/test_style_parameters.cpp
    state/test_state_defaults.cpp
    state/test_sanitize.cpp
    state/test_binary_roundtrip.cpp
    state/test_binary_robustness.cpp
    state/test_json_roundtrip.cpp
)
target_link_libraries(nedit_state_tests
    PRIVATE nedit_state nedit_state_json nedit_debug nedit_warnings
            Catch2::Catch2WithMain)

add_executable(nedit_engine_tests
    engine/test_transient_detector.cpp
    engine/test_slice_builder.cpp
    engine/test_tempo.cpp
    engine/test_easing_fold.cpp
    engine/test_granular_stretcher.cpp
    engine/test_dsp_primitives.cpp
    engine/test_pick_renderer.cpp
    engine/test_scheduler.cpp
    engine/test_threading.cpp
    engine/test_midi_dispatch.cpp
)
target_link_libraries(nedit_engine_tests
    PRIVATE nedit_engine nedit_debug nedit_warnings
            Catch2::Catch2WithMain)

include(CTest)
include(Catch)
catch_discover_tests(nedit_state_tests)
catch_discover_tests(nedit_engine_tests)

# Plugin shell tests (need the VST3 SDK, so only with NEDIT_BUILD_PLUGIN).
if(NEDIT_BUILD_PLUGIN)
    add_executable(nedit_plugin_tests
        plugin/test_parameter_surface.cpp
        plugin/test_nedit_processor.cpp
    )
    target_link_libraries(nedit_plugin_tests
        PRIVATE nedit_plugin nedit_warnings Catch2::Catch2WithMain)
    catch_discover_tests(nedit_plugin_tests)
endif()
