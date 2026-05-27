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
    src/midi_ci_pe.cpp
    src/mcoded7.cpp
    src/mpe_voice_tracker.cpp
    src/running_status.cpp
    # macOS plan 8.1 — UMP Session + Endpoint + VirtualEndpoint.
    # Cross-platform virtual-endpoint registry; platform .mm/.cpp adds
    # the OS-backed half via a static-init registrar.
    src/ump_session.cpp
    # BLE MIDI — Apple BLE-MIDI 1.0 transport. The packet codec is
    # cross-platform; create_ble_midi_central() resolves to either the
    # CoreBluetooth backend on Apple or the safe-no-op stub on other
    # platforms (see ble_midi_stub.cpp below).
    src/ble_midi_packet.cpp
)

if(APPLE AND NOT IOS)
    target_sources(pulp-midi PRIVATE
        platform/mac/coremidi_device.mm
        platform/mac/ump_session_coremidi.mm
        platform/mac/ble_midi_coremidi.mm
    )
    target_link_libraries(pulp-midi PRIVATE
        "-framework CoreMIDI"
        "-framework CoreFoundation"
        "-framework CoreBluetooth"
    )
elseif(APPLE AND IOS)
    target_sources(pulp-midi PRIVATE
        platform/mac/ble_midi_coremidi.mm
    )
    target_link_libraries(pulp-midi PRIVATE
        "-framework CoreBluetooth"
        "-framework Foundation"
    )
elseif(WIN32)
    target_sources(pulp-midi PRIVATE
        platform/win/winmidi_device.cpp
        platform/win/winrt_midi_device.cpp
        # BLE MIDI scaffold — WinRT BluetoothLEAdvertisementWatcher
        # backend not yet wired; the stub central reports
        # is_available() == false until a follow-up slice lands the
        # WinRT GATT plumbing.
        src/ble_midi_stub.cpp
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
        # BLE MIDI scaffold — BlueZ D-Bus glue not yet wired; the stub
        # central reports is_available() == false until the BlueZ
        # GattCharacteristic1 backend lands.
        src/ble_midi_stub.cpp
    )
    find_package(ALSA REQUIRED)
    target_link_libraries(pulp-midi PRIVATE ALSA::ALSA)
# Android MIDI wired via PulpAndroid.cmake → pulp_wire_android_sources()
# BLE MIDI on Android: BluetoothLeScanner + BluetoothGatt notification
# via the existing JNI bridge — scaffold only; the stub central is
# linked in until the Java half lands in pulp-android-bridge.
elseif(ANDROID)
    target_sources(pulp-midi PRIVATE
        src/ble_midi_stub.cpp
    )
else()
    target_sources(pulp-midi PRIVATE src/placeholder.cpp src/ble_midi_stub.cpp)
    file(WRITE ${CMAKE_CURRENT_SOURCE_DIR}/src/placeholder.cpp
        "// pulp-midi: platform backend pending\nnamespace pulp::midi {}\n")
endif()
