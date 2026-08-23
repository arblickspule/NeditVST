# nedit_state -- the pure, dependency-free state model + binary serialization.
add_library(nedit_state STATIC
    StyleParameters.cpp
    Serialization.cpp
)
target_include_directories(nedit_state PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(nedit_state PRIVATE nedit_warnings)

# nedit_state_json -- debug/tooling JSON mirror (kept out of the core so
# the plugin never links nlohmann_json).
include(FetchContent)
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
)
FetchContent_MakeAvailable(nlohmann_json)

add_library(nedit_state_json STATIC
    JsonIO.cpp
)
target_link_libraries(nedit_state_json
    PUBLIC nedit_state nlohmann_json::nlohmann_json
    PRIVATE nedit_warnings)
