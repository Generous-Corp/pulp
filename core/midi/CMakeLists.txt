add_library(pulp-midi)
add_library(pulp::midi ALIAS pulp-midi)

target_include_directories(pulp-midi
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(pulp-midi PUBLIC pulp::runtime)

# CHOC headers (used by MidiEvent via choc::midi)
target_include_directories(pulp-midi PUBLIC
    $<BUILD_INTERFACE:${choc_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

# MIDI file I/O (cross-platform, via CHOC)
target_sources(pulp-midi PRIVATE
    src/midi_file.cpp
    src/midi_ci.cpp
)

if(APPLE AND NOT IOS)
    target_sources(pulp-midi PRIVATE
        platform/mac/coremidi_device.mm
    )
    target_link_libraries(pulp-midi PRIVATE
        "-framework CoreMIDI"
        "-framework CoreFoundation"
    )
elseif(WIN32)
    target_sources(pulp-midi PRIVATE
        platform/win/winmidi_device.cpp
    )
    target_link_libraries(pulp-midi PRIVATE
        winmm
    )
elseif(UNIX AND NOT APPLE)
    target_sources(pulp-midi PRIVATE
        platform/linux/alsa_midi_device.cpp
    )
    find_package(ALSA REQUIRED)
    target_link_libraries(pulp-midi PRIVATE ALSA::ALSA)
else()
    target_sources(pulp-midi PRIVATE src/placeholder.cpp)
    file(WRITE ${CMAKE_CURRENT_SOURCE_DIR}/src/placeholder.cpp
        "// pulp-midi: platform backend pending\nnamespace pulp::midi {}\n")
endif()
