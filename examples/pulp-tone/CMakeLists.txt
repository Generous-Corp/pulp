# PulpTone — oscillator synth with MIDI input
# Validates: audio output, MIDI handling, note-on/off, polyphony, envelopes
# Available as: standalone, VST3, CLAP, AU

# Headless tests
add_executable(pulp-tone-test test_pulp_tone.cpp)
target_link_libraries(pulp-tone-test PRIVATE pulp::format Catch2::Catch2WithMain)
target_include_directories(pulp-tone-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(pulp-tone-test)

# Standalone application with musical typing
add_executable(PulpTone main.cpp
    ${CMAKE_SOURCE_DIR}/core/format/src/standalone.cpp
)
target_link_libraries(PulpTone PRIVATE pulp::format pulp::audio pulp::midi)
target_include_directories(PulpTone PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# ── Plugin Formats ──────────────────────────────────────────────────────

# VST3
if(PULP_HAS_VST3)
    set(VST3_PLATFORM_SOURCES "")
    if(APPLE)
        list(APPEND VST3_PLATFORM_SOURCES
            ${VST3_SDK_DIR}/public.sdk/source/main/macmain.cpp)
    elseif(WIN32)
        list(APPEND VST3_PLATFORM_SOURCES
            ${VST3_SDK_DIR}/public.sdk/source/main/dllmain.cpp)
    elseif(UNIX)
        list(APPEND VST3_PLATFORM_SOURCES
            ${VST3_SDK_DIR}/public.sdk/source/main/linuxmain.cpp)
    endif()

    add_library(PulpTone_VST3 MODULE
        vst3_entry.cpp
        ${VST3_PLATFORM_SOURCES}
        ${VST3_SDK_DIR}/public.sdk/source/main/pluginfactory.cpp
        ${VST3_SDK_DIR}/public.sdk/source/main/moduleinit.cpp
    )
    target_link_libraries(PulpTone_VST3 PRIVATE pulp::format vst3-sdk)
    target_include_directories(PulpTone_VST3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    set_target_properties(PulpTone_VST3 PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "vst3"
        OUTPUT_NAME "PulpTone"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/VST3"
    )
endif()

# CLAP
if(PULP_HAS_CLAP)
    add_library(PulpTone_CLAP MODULE
        clap_entry.cpp
        ${CMAKE_SOURCE_DIR}/core/format/src/clap_adapter.cpp
    )
    target_link_libraries(PulpTone_CLAP PRIVATE pulp::format clap)
    target_include_directories(PulpTone_CLAP PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    set_target_properties(PulpTone_CLAP PROPERTIES
        OUTPUT_NAME "PulpTone"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/CLAP"
        PREFIX ""
        SUFFIX ".clap"
    )

    if(APPLE)
        set_target_properties(PulpTone_CLAP PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION "clap"
        )
    endif()
endif()

# AU v2 (macOS)
if(APPLE AND PULP_HAS_AUSDK)
    add_library(PulpTone_AU MODULE
        au_v2_entry.cpp
    )
    target_link_libraries(PulpTone_AU PRIVATE
        pulp::format ausdk
        "-framework AudioToolbox"
        "-framework CoreFoundation"
        "-framework CoreAudio"
    )
    target_include_directories(PulpTone_AU PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    set_target_properties(PulpTone_AU PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "component"
        OUTPUT_NAME "PulpTone"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/AU"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.au"
    )
endif()
