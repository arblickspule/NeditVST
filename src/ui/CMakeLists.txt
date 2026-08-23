# nedit_ui -- framework-free UI view-model layer (pure C++).
add_library(nedit_ui STATIC
    WaveformGeometry.cpp
)
target_include_directories(nedit_ui PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(nedit_ui
    PUBLIC nedit_state nedit_engine
    PRIVATE nedit_warnings)
