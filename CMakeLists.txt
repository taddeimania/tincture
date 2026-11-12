cmake_minimum_required(VERSION 3.22)

# macOS 11+. Builds for the machine you're on by default; for a universal
# binary pass -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64". Set before project().
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

project(Tincture VERSION 0.1.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# --- JUCE ---------------------------------------------------------------------
# By default JUCE is downloaded on first configure. To use a local checkout:
#   cmake -B build -DTINCTURE_JUCE_DIR=/path/to/JUCE
set(TINCTURE_JUCE_DIR "" CACHE PATH "Optional local JUCE checkout")
option(TINCTURE_ENGINE_ONLY "Build only the engine and its tests (no JUCE)" OFF)

if(NOT TINCTURE_ENGINE_ONLY)
    if(TINCTURE_JUCE_DIR)
        add_subdirectory(${TINCTURE_JUCE_DIR} JUCE)
    else()
        include(FetchContent)
        FetchContent_Declare(JUCE
            GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
            GIT_TAG        8.0.9
            GIT_SHALLOW    TRUE)
        FetchContent_MakeAvailable(JUCE)
    endif()
endif()

# --- Sound engine (no JUCE) ----------------------------------------------------
# Voices, oscillators, filters, modulation, FX, arp and clips. The tests and
# the CPU benchmark build against this alone.
file(GLOB TINCTURE_ENGINE_SOURCES CONFIGURE_DEPENDS
    Source/Engine/*.cpp Source/Engine/*/*.cpp Source/Model/*.cpp)
add_library(TinctureDSP STATIC ${TINCTURE_ENGINE_SOURCES})
target_include_directories(TinctureDSP PUBLIC Source)
# Zstandard decoder (BSD licence, ThirdParty/zstd), for reading Serum 2 presets.
add_library(TinctureZstd STATIC ThirdParty/zstd/zstddeclib.c)
set_target_properties(TinctureZstd PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(TinctureZstd PRIVATE ZSTD_DISABLE_ASM=1)
# (No /w on MSVC: it only conflicts with the default warning level, and the
# decoder compiles clean there.)
if(NOT MSVC)
    target_compile_options(TinctureZstd PRIVATE -O2 -w)
endif()
target_link_libraries(TinctureDSP PUBLIC TinctureZstd)
set_target_properties(TinctureDSP PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(TinctureDSP PRIVATE /utf-8 /fp:fast /wd4324)
else()
    # The engine is the hot path: optimise it even in Debug builds.
    target_compile_options(TinctureDSP PRIVATE -O3 -ffast-math -fno-finite-math-only)
endif()

enable_testing()
# Every Tests/Engine*.cpp is its own test program.
file(GLOB TINCTURE_ENGINE_TESTS CONFIGURE_DEPENDS Tests/Engine*.cpp)
foreach(test_src ${TINCTURE_ENGINE_TESTS})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(Tincture${test_name} ${test_src})
    target_link_libraries(Tincture${test_name} PRIVATE TinctureDSP)
    add_test(NAME ${test_name} COMMAND Tincture${test_name})
endforeach()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/Tools/Bench.cpp)
    add_executable(TinctureBench Tools/Bench.cpp)
    target_link_libraries(TinctureBench PRIVATE TinctureDSP)
endif()

if(TINCTURE_ENGINE_ONLY)
    return()
endif()

# --- Plugin -------------------------------------------------------------------
# Copy the built plugin into the system VST3 folder after each build:
#   macOS   ~/Library/Audio/Plug-Ins/VST3
#   Linux   ~/.vst3
#   Windows C:\Program Files\Common Files\VST3  (needs an administrator shell,
#           so it is off by default there)
if(WIN32)
    set(_tincture_copy_default OFF)
else()
    set(_tincture_copy_default ON)
endif()
option(TINCTURE_COPY_PLUGIN "Install the plugin into the system VST3 folder after building" ${_tincture_copy_default})

file(GLOB TINCTURE_UI_SOURCES CONFIGURE_DEPENDS Source/UI/*.cpp)
set(TINCTURE_PLUGIN_SOURCES ${TINCTURE_UI_SOURCES})
foreach(f Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/Settings.cpp Source/PresetStore.cpp Source/ParamStore.cpp Source/SerumLibrary.cpp)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${f})
        list(APPEND TINCTURE_PLUGIN_SOURCES ${f})
    endif()
endforeach()

set(TINCTURE_HAVE_PLUGIN OFF)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/Source/PluginProcessor.cpp)
    set(TINCTURE_HAVE_PLUGIN ON)
endif()

if(TINCTURE_HAVE_PLUGIN)
    juce_add_plugin(Tincture
        COMPANY_NAME                "Taddei"
        BUNDLE_ID                   com.taddei.tincture
        PLUGIN_MANUFACTURER_CODE    Tadd
        PLUGIN_CODE                 Tnct
        PRODUCT_NAME                "Tincture"
        DESCRIPTION                 "Wavetable synthesizer"
        FORMATS                     VST3
        IS_SYNTH                    TRUE
        NEEDS_MIDI_INPUT            TRUE
        NEEDS_MIDI_OUTPUT           FALSE
        IS_MIDI_EFFECT              FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS TRUE
        VST3_CATEGORIES             Instrument Synth
        COPY_PLUGIN_AFTER_BUILD     ${TINCTURE_COPY_PLUGIN})

    target_sources(Tincture PRIVATE ${TINCTURE_PLUGIN_SOURCES})
    target_include_directories(Tincture PRIVATE Source)
    if(MSVC)
        # C4324: 'structure was padded due to alignment specifier' -- intended (alignas buffers).
        target_compile_options(Tincture PRIVATE /utf-8 /wd4324)
    endif()
    target_compile_definitions(Tincture PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)
    target_link_libraries(Tincture
        PRIVATE
            TinctureDSP
            juce::juce_audio_utils
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
endif()

# --- UI tests (headless; need a display on Linux, e.g. xvfb-run) -------------
#   TinctureMenuTests [out-dir]   drives every menu behaviour with synthetic
#                                 mouse and keyboard events, writes PNGs
option(TINCTURE_BUILD_TOOLS "Build the snapshot tool and the UI tests" OFF)
if(TINCTURE_BUILD_TOOLS)
    juce_add_console_app(TinctureMenuTests PRODUCT_NAME "TinctureMenuTests")
    target_sources(TinctureMenuTests PRIVATE Tests/MenuTests.cpp Source/UI/Menu.cpp Source/UI/Widgets.cpp)
    target_include_directories(TinctureMenuTests PRIVATE Source)
    target_compile_definitions(TinctureMenuTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(TinctureMenuTests PRIVATE juce::juce_gui_basics juce::juce_recommended_config_flags)

    if(TINCTURE_HAVE_PLUGIN)
        juce_add_console_app(TinctureSnapshot PRODUCT_NAME "TinctureSnapshot")
        file(GLOB TINCTURE_SHOT_SOURCES CONFIGURE_DEPENDS Tools/Shots*.cpp)
        target_sources(TinctureSnapshot PRIVATE Tools/Snapshot.cpp ${TINCTURE_SHOT_SOURCES} ${TINCTURE_PLUGIN_SOURCES})
        target_include_directories(TinctureSnapshot PRIVATE Tools)
        target_include_directories(TinctureSnapshot PRIVATE Source)
        if(MSVC)
            target_compile_options(TinctureSnapshot PRIVATE /utf-8 /wd4324)
        endif()
        target_compile_definitions(TinctureSnapshot PRIVATE
            JucePlugin_Name="Tincture"
            JUCE_MODAL_LOOPS_PERMITTED=1
            JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
        target_link_libraries(TinctureSnapshot PRIVATE
            TinctureDSP
            juce::juce_audio_utils
            juce::juce_dsp
            juce::juce_recommended_config_flags)
    endif()
endif()
