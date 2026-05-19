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
    src/mpe_voice_tracker.cpp
    src/running_status.cpp
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
        platform/win/winrt_midi_device.cpp
    )
    target_link_libraries(pulp-midi PRIVATE
        winmm
    )
    # Workstream 02 #245 — opt-in WinRT MIDI 2.0 backend.
    # Enable with -DPULP_HAS_WINRT_MIDI=ON once Windows 11 SDK 10.0.22621+
    # and the cppwinrt tool are installed. Linker adds the WinRT import
    # libs; the TU itself compiles empty unless the flag is set.
    if(PULP_HAS_WINRT_MIDI)
        target_compile_definitions(pulp-midi PRIVATE PULP_HAS_WINRT_MIDI=1)
        # runtimeobject is the C++/WinRT activation runtime; future work
        # will add the generated headers dir to the include path here.
        target_link_libraries(pulp-midi PRIVATE runtimeobject)
        message(STATUS "Pulp: WinRT MIDI 2.0 backend enabled")
    endif()
elseif(UNIX AND NOT APPLE AND NOT ANDROID)
    target_sources(pulp-midi PRIVATE
        platform/linux/alsa_midi_device.cpp
    )
    find_package(ALSA REQUIRED)
    target_link_libraries(pulp-midi PRIVATE ALSA::ALSA)
# Android MIDI wired via PulpAndroid.cmake → pulp_wire_android_sources()
else()
    target_sources(pulp-midi PRIVATE src/placeholder.cpp)
    file(WRITE ${CMAKE_CURRENT_SOURCE_DIR}/src/placeholder.cpp
        "// pulp-midi: platform backend pending\nnamespace pulp::midi {}\n")
endif()
