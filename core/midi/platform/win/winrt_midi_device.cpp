// WinRT MIDI 2.0 backend — workstream 02 #245 phase 1.
//
// Windows 11's `Windows.Devices.Midi2` namespace exposes real MIDI 2.0
// endpoints, DeviceWatcher hotplug, QueryPerformanceCounter timestamps,
// and MIM_LONGDATA-style sysex via native buffers. The existing
// winmidi_device.cpp uses the legacy mmeapi path, which truncates MIDI
// 2.0 UMP, has no hotplug, and requires manual sysex assembly.
//
// This TU is the skeleton the real WinRT consumer code will live in.
// It is only compiled when PULP_HAS_WINRT_MIDI is set — a build flag
// the user opts into after installing the Windows 11 SDK 10.0.22621+
// and the `cppwinrt` tool. Without the flag, the linker never sees
// this object and the existing mmeapi backend stays authoritative.
//
// Why gate rather than auto-detect: cppwinrt requires generated headers
// that live in %VCINSTALLDIR%\… and vary per SDK version; the hassle of
// cross-compile + CI matrix support argues for opt-in today.

#if defined(_WIN32) && defined(PULP_HAS_WINRT_MIDI)

#include <pulp/midi/device.hpp>
#include <pulp/runtime/log.hpp>

// When the real path lands, uncomment:
//   #include <winrt/Windows.Foundation.h>
//   #include <winrt/Windows.Devices.Midi2.h>
//   using namespace winrt::Windows::Devices::Midi2;

namespace pulp::midi::win::winrt {

// Placeholder API surface. Real impl will:
//   * Midi2Endpoint::CreateForConnection → wrap the endpoint.
//   * MidiMessageReceivedEventArgs → push to MidiBuffer + UmpBuffer.
//   * QueryPerformanceCounter timestamps on the receive event.
//   * DeviceWatcher → fire_device_change on the owning AudioSystem.
bool winrt_midi_available() {
    // Stub: presence of the build flag means the dev chose to link the
    // WinRT SDK; availability of Midi2 at runtime requires Windows 11
    // build 22621+. Full runtime detection lands with the real client.
    runtime::log_info("WinRT MIDI 2.0 backend compiled in (skeleton)");
    return false;  // keeps callers on the legacy mmeapi path for now
}

}  // namespace pulp::midi::win::winrt

#else

// When PULP_HAS_WINRT_MIDI is unset the fallback stub keeps the symbol
// resolved so callers that branch on winrt_midi_available() do not
// need their own _WIN32 gate.
namespace pulp::midi::win::winrt {
bool winrt_midi_available() { return false; }
}  // namespace pulp::midi::win::winrt

#endif
