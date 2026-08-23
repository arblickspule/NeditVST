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

include(CTest)
include(Catch)
catch_discover_tests(nedit_state_tests)
