# nedit_engine -- pure C++ DSP/scheduling engine operating on
# nedit::state + raw audio buffers. Framework-free, unit-testable offline.
add_library(nedit_engine STATIC
    TransientDetector.cpp
    SliceBuilder.cpp
)
target_include_directories(nedit_engine PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(nedit_engine
    PUBLIC nedit_state
    PRIVATE nedit_warnings)
