cmake_minimum_required(VERSION 3.22)

project(FronasmaskinenAU VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)

add_subdirectory(/Users/fronas/Documents/Dev/JUCE JUCE)

juce_add_plugin(FronasmaskinenAU
    COMPANY_NAME "Fronas"
    BUNDLE_ID "com.fronas.fronasmaskinen"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    PLUGIN_MANUFACTURER_CODE Frns
    PLUGIN_CODE Fr01
    FORMATS AU
    PRODUCT_NAME "Fronasmaskinen")

juce_generate_juce_header(FronasmaskinenAU)

target_sources(FronasmaskinenAU
    PRIVATE
        Source/PluginEditor.cpp
        Source/PluginProcessor.cpp)

target_compile_definitions(FronasmaskinenAU
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(FronasmaskinenAU
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
