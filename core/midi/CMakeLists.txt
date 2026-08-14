add_library(pulp-midi)
add_library(pulp::midi ALIAS pulp-midi)

target_include_directories(pulp-midi
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(pulp-midi PUBLIC pulp::runtime pulp::music pulp::timebase)

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
    src/tuning.cpp
    # macOS plan 8.1 — UMP Session + Endpoint + VirtualEndpoint.
    # Cross-platform virtual-endpoint registry; platform .mm/.cpp adds
    # the OS-backed half via a static-init registrar.
    src/ump_session.cpp
    # BLE MIDI — Apple BLE-MIDI 1.0 transport. The packet codec is
    # cross-platform; create_ble_midi_central() resolves to either the
    # CoreBluetooth backend on Apple, the BlueZ D-Bus backend on Linux, or
    # the safe-no-op stub on other platforms (see the per-platform branch).
    src/ble_midi_packet.cpp
    # Process-wide registry that bridges a connected BLE peripheral into the
    # platform MidiSystem's port list (the off-Apple analog of CoreMIDI
    # auto-exposing OS-paired BLE peripherals as endpoints). Empty + inert on
    # platforms whose central never registers anything (the stub central).
    src/ble_midi_registry.cpp
)

if(PULP_HAS_MTS_ESP)
    target_sources(pulp-midi PRIVATE
        src/mts_esp_tuning.cpp
        "${PULP_MTS_ESP_SOURCE_DIR}/Client/libMTSClient.cpp"
    )
    target_include_directories(pulp-midi PRIVATE
        "${PULP_MTS_ESP_SOURCE_DIR}/Client"
    )
    target_compile_definitions(pulp-midi PUBLIC PULP_HAS_MTS_ESP=1)
    if(CMAKE_DL_LIBS)
        target_link_libraries(pulp-midi PRIVATE ${CMAKE_DL_LIBS})
    endif()
endif()

if(PULP_HAS_SCALA_TUNING)
    target_sources(pulp-midi PRIVATE
        src/scala_tuning.cpp
    )
    target_include_directories(pulp-midi PRIVATE
        "${PULP_SST_TUNING_LIBRARY_SOURCE_DIR}/include"
    )
    target_compile_definitions(pulp-midi PUBLIC PULP_HAS_SCALA_TUNING=1)
endif()

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
        platform/mac/coremidi_device.mm
        platform/mac/ump_session_coremidi.mm
        platform/mac/ble_midi_coremidi.mm
    )
    target_link_libraries(pulp-midi PRIVATE
        "-framework CoreMIDI"
        "-framework CoreFoundation"
        "-framework CoreBluetooth"
        "-framework Foundation"
    )

    # Keep the shared-client identity/lifetime contract compiling in the real
    # iOS SDK graph even though Catch2 tests do not execute on iphoneos.
    add_library(pulp-ios-coremidi-shared-client-contract OBJECT EXCLUDE_FROM_ALL
        ${CMAKE_SOURCE_DIR}/test/test_coremidi_shared_client.cpp)
    target_compile_definitions(pulp-ios-coremidi-shared-client-contract PRIVATE
        PULP_COREMIDI_SHARED_CLIENT_COMPILE_ONLY=1)
    target_link_libraries(pulp-ios-coremidi-shared-client-contract PRIVATE
        pulp::midi)

    # A real installable Simulator app drives production MidiSystem discovery
    # against virtual CoreMIDI endpoints. The P0-4 gate builds this for both
    # Apple SDKs and launches the simulator variant.
    add_executable(pulp-ios-coremidi-harness MACOSX_BUNDLE EXCLUDE_FROM_ALL
        ${CMAKE_SOURCE_DIR}/test/ios/coremidi_backend_harness.mm)
    target_link_libraries(pulp-ios-coremidi-harness PRIVATE
        pulp::midi
        "-framework CoreMIDI"
        "-framework Foundation")
    set(pulp_ios_coremidi_harness_plist
        "${CMAKE_CURRENT_BINARY_DIR}/PulpCoreMidiHarness-Info.plist")
    file(GENERATE OUTPUT "${pulp_ios_coremidi_harness_plist}" CONTENT [=[
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>English</string>
    <key>CFBundleExecutable</key>
    <string>$(EXECUTABLE_NAME)</string>
    <key>CFBundleIdentifier</key>
    <string>$(PRODUCT_BUNDLE_IDENTIFIER)</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>$(PRODUCT_NAME)</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>LSRequiresIPhoneOS</key>
    <true/>
    <key>UIBackgroundModes</key>
    <array>
        <string>audio</string>
    </array>
</dict>
</plist>
]=])
    set_target_properties(pulp-ios-coremidi-harness PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${pulp_ios_coremidi_harness_plist}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.pulp.tests.coremidi"
        MACOSX_BUNDLE_BUNDLE_NAME "PulpCoreMidiHarness"
        OUTPUT_NAME "PulpCoreMidiHarness"
        XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "dev.pulp.tests.coremidi"
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")
elseif(WIN32)
    target_sources(pulp-midi PRIVATE
        platform/win/winmidi_device.cpp
        platform/win/winrt_midi_device.cpp
        # BLE MIDI — WinRT BluetoothLEAdvertisementWatcher scan +
        # GATT connect/notify backend. Unlike the WinRT MIDI 2.0 backend
        # (winrt_midi_device.cpp), BLE GATT lives in
        # Windows.Devices.Bluetooth* / Windows.Devices.Bluetooth.
        # GenericAttributeProfile and the byte-buffer helpers in
        # Windows.Storage.Streams, all in the BASE Windows SDK cppwinrt
        # projection — so no out-of-band vcpkg/NuGet dependency is needed,
        # just the OS umbrella import libs below. This TU defines
        # create_ble_midi_central() for Windows, so it REPLACES
        # src/ble_midi_stub.cpp (linking both would duplicate the symbol).
        # Connected peripherals are published into the BleMidiPortRegistry,
        # which winmidi_device.cpp / winrt_midi_device.cpp merge into the
        # platform MidiSystem's port list.
        platform/win/ble_midi_win.cpp
    )
    target_link_libraries(pulp-midi PRIVATE
        winmm
        # WinRT activation + COM apartment for the BLE advertisement watcher.
        # WindowsApp is the desktop umbrella import lib (init_apartment,
        # activation factories, event tokens); runtimeobject provides the
        # WinRT/COM runtime helpers. Both ship with the base Windows SDK.
        WindowsApp
        runtimeobject
    )
    # Opt-in WinRT MIDI 2.0 (UMP) backend — Windows MIDI Services SDK.
    #
    # Enable with -DPULP_HAS_WINRT_MIDI=ON. The backend consumes the
    # Microsoft.Windows.Devices.Midi2 C++/WinRT projection, which ships
    # out-of-band with the Windows MIDI Services SDK (NuGet
    # Microsoft.Windows.Devices.Midi2 — GitHub-only, NOT on nuget.org) — it
    # is not part of the base Windows SDK. The TU compiles to a single
    # fallback stub unless the flag is set, so leaving the flag off keeps the
    # default build free of the out-of-band Midi2 projection dependency.
    #
    # Provisioning the projection (the gating dependency). The CI lane uses
    # the official Microsoft vcpkg port `microsoft-windows-devices-midi2`,
    # which downloads the SDK NuGet, runs cppwinrt to generate the projection
    # headers (winrt/Microsoft.Windows.Devices.Midi2*.h) plus the bootstrap
    # initializer header
    # (winmidi/init/Microsoft.Windows.Devices.Midi2.Initialization.hpp), and
    # exports an IMPORTED CMake target Microsoft::Windows::Devices::Midi2.
    # The port requires Windows SDK >= 10.0.26100.0 and cppwinrt as a host
    # dependency. See tools/ci/midi2/ for the manifest and the
    # `windows-midi2-gate` job in .github/workflows/build.yml for the wiring.
    #
    # Two consumption modes:
    #   1. vcpkg (preferred — what CI uses): configure with the vcpkg
    #      toolchain so find_package(microsoft-windows-devices-midi2 CONFIG)
    #      resolves the Microsoft::Windows::Devices::Midi2 IMPORTED target,
    #      which carries the projection include dirs transitively.
    #   2. Manual: set -DPULP_WINRT_MIDI_INCLUDE=<dir> to a directory that
    #      already contains winrt/ and winmidi/ projection headers (e.g. a
    #      hand-run cppwinrt output). Pulp then links the OS umbrella import
    #      libs directly (WindowsApp + runtimeobject).
    if(PULP_HAS_WINRT_MIDI)
        target_compile_definitions(pulp-midi PRIVATE PULP_HAS_WINRT_MIDI=1)

        # Prefer the vcpkg IMPORTED target — the only path that carries the
        # generated projection headers in one place, matching microsoft/MIDI's
        # own cpp-winrt-cmake sample.
        find_package(microsoft-windows-devices-midi2 CONFIG QUIET)
        if(microsoft-windows-devices-midi2_FOUND
                AND TARGET Microsoft::Windows::Devices::Midi2)
            target_link_libraries(pulp-midi PRIVATE
                Microsoft::Windows::Devices::Midi2)
            # OneCoreUap.lib is the umbrella import library the SDK sample
            # links for WinRT activation (init_apartment, activation
            # factories, event tokens) on desktop. cppwinrt's CONFIG package
            # adds its fast_forward library during configure, so it is not
            # listed here.
            target_link_libraries(pulp-midi PRIVATE OneCoreUap)
            message(STATUS
                "Pulp: WinRT MIDI 2.0 backend enabled (vcpkg projection)")
        elseif(PULP_WINRT_MIDI_INCLUDE)
            if(NOT EXISTS "${PULP_WINRT_MIDI_INCLUDE}")
                message(FATAL_ERROR
                    "PULP_HAS_WINRT_MIDI=ON with PULP_WINRT_MIDI_INCLUDE="
                    "'${PULP_WINRT_MIDI_INCLUDE}' but that directory does not "
                    "exist. Point it at a cppwinrt projection output "
                    "containing winrt/Microsoft.Windows.Devices.Midi2.h and "
                    "winmidi/init/Microsoft.Windows.Devices.Midi2.Initialization.hpp.")
            endif()
            target_include_directories(pulp-midi PRIVATE
                "${PULP_WINRT_MIDI_INCLUDE}")
            target_link_libraries(pulp-midi PRIVATE WindowsApp runtimeobject)
            message(STATUS
                "Pulp: WinRT MIDI 2.0 backend enabled "
                "(manual projection: ${PULP_WINRT_MIDI_INCLUDE})")
        else()
            message(FATAL_ERROR
                "PULP_HAS_WINRT_MIDI=ON but the Microsoft.Windows.Devices.Midi2 "
                "projection was not found. Either configure with the vcpkg "
                "toolchain and the microsoft-windows-devices-midi2 port "
                "installed (preferred — see tools/ci/midi2/README.md), or set "
                "-DPULP_WINRT_MIDI_INCLUDE=<cppwinrt-output-dir>.")
        endif()
    endif()
elseif(UNIX AND NOT APPLE AND NOT ANDROID)
    target_sources(pulp-midi PRIVATE
        platform/linux/alsa_midi_device.cpp
        # BLE MIDI — BlueZ (org.bluez) over the runtime-dlopened libdbus client
        # in core/platform. No new library is linked: BlueZ is a system daemon,
        # libdbus is loaded at runtime. Replaces the former no-op stub.
        platform/linux/ble_midi_linux.cpp
    )
    find_package(ALSA REQUIRED)
    target_link_libraries(pulp-midi PRIVATE ALSA::ALSA)
    # pulp::platform carries the DBus client (system-bus BlueZ access) and the
    # ALSA MidiSystem's registry merge. PUBLIC include dirs come transitively.
    target_link_libraries(pulp-midi PRIVATE pulp::platform)
# Android MIDI wired via PulpAndroid.cmake → pulp_wire_android_sources().
# BLE MIDI uses Android's MidiManager Bluetooth transport through the Kotlin
# bridge and publishes connected ports into BleMidiPortRegistry.
elseif(ANDROID)
    target_sources(pulp-midi PRIVATE
        platform/android/ble_midi_android.cpp
    )
    add_library(pulp-android-ble-midi-validation SHARED EXCLUDE_FROM_ALL
        platform/android/ble_midi_android.cpp
        src/ble_midi_registry.cpp
    )
    set_target_properties(pulp-android-ble-midi-validation PROPERTIES
        OUTPUT_NAME "pulp_ble_midi_validation")
    target_compile_definitions(pulp-android-ble-midi-validation PRIVATE
        PULP_ANDROID_BLE_VALIDATION_LIBRARY=1)
    target_include_directories(pulp-android-ble-midi-validation PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/core/platform/include
        ${CMAKE_SOURCE_DIR}/core/runtime/include
        ${choc_SOURCE_DIR}
    )
    target_link_libraries(pulp-android-ble-midi-validation PRIVATE log)
    foreach(_pulp_android_ble_jni_symbol IN ITEMS
        Java_com_pulp_midi_PulpBluetoothMidi_nativeInstallBridge
        Java_com_pulp_midi_PulpBluetoothMidi_nativeOnPeripheralFound
        Java_com_pulp_midi_PulpBluetoothMidi_nativeOnConnected
        Java_com_pulp_midi_PulpBluetoothMidi_nativeOnConnectionFailed
        Java_com_pulp_midi_PulpBluetoothMidi_nativeOnDisconnected
        Java_com_pulp_midi_PulpBluetoothMidi_nativeOnScanStopped
        Java_com_pulp_midi_PulpBluetoothMidi_nativeOnMidiReceived
        Java_com_pulp_midi_PulpBluetoothMidi_nativeBeginRegistryValidation
        Java_com_pulp_midi_PulpBluetoothMidi_nativeConnectRegistryValidation
        Java_com_pulp_midi_PulpBluetoothMidi_nativeAttachRegistryValidationInput
        Java_com_pulp_midi_PulpBluetoothMidi_nativeRegistryValidationReceived
        Java_com_pulp_midi_PulpBluetoothMidi_nativeSendRegistryValidationOutput
        Java_com_pulp_midi_PulpBluetoothMidi_nativeDisconnectRegistryValidation
        Java_com_pulp_midi_PulpBluetoothMidi_nativeRegistryFlags
        Java_com_pulp_midi_PulpBluetoothMidi_nativeRegistryEventCounts
        Java_com_pulp_midi_PulpBluetoothMidi_nativeRegistryValidationIsScanning)
        target_link_options(pulp-midi INTERFACE
            "-Wl,--undefined=${_pulp_android_ble_jni_symbol}")
    endforeach()
else()
    target_sources(pulp-midi PRIVATE src/placeholder.cpp src/ble_midi_stub.cpp)
    file(WRITE ${CMAKE_CURRENT_SOURCE_DIR}/src/placeholder.cpp
        "// pulp-midi: platform backend pending\nnamespace pulp::midi {}\n")
endif()
