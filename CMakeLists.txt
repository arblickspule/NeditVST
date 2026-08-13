cmake_minimum_required(VERSION 3.22)

# Pin an explicit minimum macOS version (must be set before project()) so
# CI builds target a consistent baseline regardless of whichever macOS
# version GitHub's hosted runners happen to default to -- rather than
# implicitly matching whatever OS the build machine is running, which can
# silently drift out from under us. macOS 11 (Big Sur) is a reasonably
# old-but-modern floor; only honoured on Apple platforms, harmless no-op
# elsewhere. Left overridable via -DCMAKE_OSX_DEPLOYMENT_TARGET=... on the
# command line.
if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS deployment target")
endif()

project(NeditVST VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CMake-based CI build, alongside (not replacing) the existing Projucer/
# Xcode project in NeditVST/NeditVST.jucer — that stays the Mac dev
# workflow exactly as it is today. This file exists so GitHub Actions can
# build VST3s for Windows/Linux/macOS without needing JUCE preinstalled on
# the runner or a Projucer-generated per-platform project checked in.
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.14 # matches the JUCE version installed at /Applications/JUCE locally (JUCE v8.0.14)
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

# Plugin identity below is copied exactly from NeditVST/NeditVST.jucer (and
# its Projucer-resolved defaults in NeditVST/JuceLibraryCode/JucePluginDefines.h)
# so the CMake-built VST3 has an identical identity to the existing
# Xcode-built one — same manufacturer/plugin codes, same bundle ID, same
# characteristics. None of these are new values.
#
# NEEDS_MIDI_INPUT TRUE matches the .jucer's pluginCharacteristicsValue
# ("pluginIsSynth,pluginWantsMidiIn") and PluginProcessor::acceptsMidi(),
# which now returns true -- MIDI note-on input drives Sequencer pattern-bank
# recall while Sequenced Trigger Mode is active (see PluginProcessor.h's
# "MIDI input / Sequencer pattern bank" section).
juce_add_plugin(NeditVST
    COMPANY_NAME "yourcompany"
    COMPANY_WEBSITE "www.yourcompany.com"
    BUNDLE_ID "com.yourcompany.NeditVST"
    PRODUCT_NAME "NeditVST"
    DESCRIPTION "NeditVST"
    PLUGIN_MANUFACTURER_CODE Manu
    PLUGIN_CODE Tpmh
    FORMATS VST3
    VST3_CATEGORIES "Instrument" "Synth"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
)

target_sources(NeditVST
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginProcessor.h
        Source/SlicerModel.cpp
        Source/SlicerModel.h
        Source/SlicerEngine.cpp
        Source/SlicerEngine.h
        Source/PluginEditor.cpp
        Source/PluginEditor.h
        Source/TransientDetector.cpp
        Source/TransientDetector.h
        Source/WaveformDisplay.cpp
        Source/WaveformDisplay.h
        Source/SubdivisionProbabilityGrid.cpp
        Source/SubdivisionProbabilityGrid.h
        Source/GranularStretcher.cpp
        Source/GranularStretcher.h
        Source/PlaybackStyleGrid.cpp
        Source/PlaybackStyleGrid.h
        Source/SequencerGrid.cpp
        Source/SequencerGrid.h
        Source/PlaybackStylePalette.cpp
        Source/PlaybackStylePalette.h
        Source/PlaybackStyleParameterPanel.cpp
        Source/PlaybackStyleParameterPanel.h
        Source/PatternBankPanel.cpp
        Source/PatternBankPanel.h
        Source/PerformanceKeyboardPanel.cpp
        Source/PerformanceKeyboardPanel.h
        Source/EasingCurve.h
        Source/NeditPalette.h
        Source/SegmentedButtonRow.cpp
        Source/SegmentedButtonRow.h
        Source/SectionPanel.cpp
        Source/SectionPanel.h
)

juce_generate_juce_header(NeditVST)

target_compile_definitions(NeditVST
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        # Matches <JUCEOPTIONS> in NeditVST.jucer exactly.
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_STRICT_REFCOUNTEDPOINTER=1
)

# Matches <MODULES> in NeditVST.jucer, minus juce_audio_plugin_client and
# juce_audio_processors_headless — both are handled internally by
# juce_add_plugin()'s FORMATS argument and are never linked directly in
# the CMake JUCE workflow (linking juce_audio_plugin_client manually here
# would duplicate what juce_add_plugin already wires up).
target_link_libraries(NeditVST
    PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
        juce::juce_dsp
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
)

# --- Unit tests (doctest) -----------------------------------------------------
# A separate console executable that compiles the non-UI engine/model sources
# directly against JUCE -- no plugin client, no GUI -- so the DSP and model
# logic can be exercised headlessly and runnable from ctest.
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.11
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(doctest)

enable_testing()

# juce_add_console_app (not plain add_executable) so the target gets the
# generated-sources dir juce_generate_juce_header() below requires.
juce_add_console_app(NeditVST_tests)

target_sources(NeditVST_tests
    PRIVATE
    tests/test_main.cpp
    tests/test_smoke.cpp
    tests/test_engine_statics.cpp
    tests/test_model_tempo.cpp
    tests/test_transient_detector.cpp
    tests/test_granular_stretcher.cpp
    tests/test_model_slices.cpp
    tests/test_engine_process.cpp
    Source/SlicerModel.cpp
    Source/SlicerModel.h
    Source/SlicerEngine.cpp
    Source/SlicerEngine.h
    Source/TransientDetector.cpp
    Source/TransientDetector.h
    Source/GranularStretcher.cpp
    Source/GranularStretcher.h
    Source/EasingCurve.h
)

# SlicerModel/SlicerEngine/TransientDetector reference no GUI or plugin-client
# code (AudioPlayHead comes from juce_audio_processors, which is linkable
# standalone without the VST3 client juce_add_plugin wires up for the plugin).
target_link_libraries(NeditVST_tests
    PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_core
        juce::juce_data_structures
        juce::juce_dsp
        juce::juce_events
        doctest::doctest
)

# Generates the <JuceHeader.h> the model/engine includes, configured for the
# module set linked above (same macro the plugin target uses).
juce_generate_juce_header(NeditVST_tests)

# The model/engine include each other by bare filename, so the source dir has
# to be on the test target's include path too.
target_include_directories(NeditVST_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
)

add_test(NAME NeditVST_tests COMMAND NeditVST_tests)
