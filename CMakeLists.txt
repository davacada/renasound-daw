cmake_minimum_required(VERSION 3.22)

project(Renasound VERSION 0.9.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
    # Build one app that runs natively on both Apple Silicon and Intel Macs
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "" FORCE)
endif()

# ---- JUCE ------------------------------------------------------------------
# Uses a local copy in ./JUCE if you have one, otherwise downloads it once.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
    add_subdirectory(JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        9.0.5
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# ---- Images baked into the app ---------------------------------------------
set(RENASOUND_BUILD_NUMBER 0 CACHE STRING "Build number, set by CI; the updater compares it with GitHub's")

juce_add_binary_data(RenasoundAssets SOURCES Resources/logo_mark.png)

# ---- Shared settings for the app and the test --------------------------------
set(RENASOUND_DEFINITIONS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_PLUGINHOST_VST3=1      # third-party VST3 plugins (Mac + Windows)
    JUCE_PLUGINHOST_AU=1)       # Audio Units (Mac only, ignored elsewhere)

set(RENASOUND_MODULES
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_gui_extra)

# ---- The app ----------------------------------------------------------------
juce_add_gui_app(Renasound
    PRODUCT_NAME            "RENASOUND"
    COMPANY_NAME            "RENASOUND"
    BUNDLE_ID               "com.renasound.daw"
    VERSION                 0.4.0
    ICON_BIG                "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon.png"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "RENASOUND needs your microphone to record audio tracks.")

juce_generate_juce_header(Renasound)

target_sources(Renasound PRIVATE
    Source/Main.cpp
    Source/Model.cpp
    Source/Effects.cpp
    Source/StudioEq.cpp
    Source/Devices.cpp
    Source/Faceplates.cpp
    Source/Updater.cpp
    Source/Pitch.cpp
    Source/Drums.cpp
    Source/MixcraftImport.cpp
    Source/AbletonImport.cpp
    Source/AudioEngine.cpp
    Source/MainComponent.cpp
    Source/UI/TimelineView.cpp
    Source/UI/PianoRoll.cpp
    Source/UI/MixerView.cpp
    Source/UI/LibraryPanel.cpp
    Source/UI/DeviceWindow.cpp
    Source/UI/PitchEditor.cpp)

target_include_directories(Renasound PRIVATE Source)

target_compile_definitions(Renasound PRIVATE
    ${RENASOUND_DEFINITIONS}
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Renasound,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:Renasound,JUCE_VERSION>"
    RENASOUND_BUILD_NUMBER=${RENASOUND_BUILD_NUMBER})

target_link_libraries(Renasound
    PRIVATE
        RenasoundAssets
        ${RENASOUND_MODULES}
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---- Optional headless test (cmake -DRENASOUND_TESTS=ON) --------------------
option(RENASOUND_TESTS "Build the headless render test" OFF)
if(RENASOUND_TESTS)
    juce_add_console_app(RenderTest PRODUCT_NAME "RenderTest")
    juce_generate_juce_header(RenderTest)
    target_sources(RenderTest PRIVATE Tests/RenderTest.cpp Source/Model.cpp Source/Effects.cpp Source/StudioEq.cpp Source/Devices.cpp Source/Faceplates.cpp Source/Pitch.cpp Source/Drums.cpp Source/MixcraftImport.cpp Source/AbletonImport.cpp Source/AudioEngine.cpp)
    target_include_directories(RenderTest PRIVATE Source)
    target_compile_definitions(RenderTest PRIVATE ${RENASOUND_DEFINITIONS})
    target_link_libraries(RenderTest PRIVATE ${RENASOUND_MODULES} juce::juce_recommended_config_flags)
endif()
