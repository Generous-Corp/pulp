# MIDI 1.0 Backends per OS

Status: **audit document**.

Pulp's MIDI 1.0 surface is `pulp::midi::create_midi_system()` →
`MidiSystem::create_input()` / `create_output()`. Each OS ships a backend
implementation under `core/midi/platform/<os>/`. This file records what
ships today, per backend.

## Matrix

| OS / API           | Source                                          | Sysex (in) | Sysex (out) | Hotplug | Callback thread        | Typical jitter |
|--------------------|-------------------------------------------------|------------|-------------|---------|------------------------|----------------|
| macOS — CoreMIDI   | `core/midi/platform/mac/coremidi_device.mm`     | yes (MIDI-1 adapter reassembles UMP SysEx7) | yes | yes (CoreMIDI notify) | CoreMIDI thread (high QoS) | sub-ms typical |
| iOS — CoreMIDI     | shared with macOS path                          | yes (MIDI-1 adapter reassembles UMP SysEx7) | yes | yes | CoreMIDI thread        | sub-ms typical |
| Linux — ALSA raw MIDI | `core/midi/platform/linux/alsa_midi_device.cpp` | yes (raw-byte stream + `SysexAccumulator`) | yes | partial (runtime libudev monitor; manual re-enumeration fallback) | dedicated `std::thread` per port | ~1 ms (`poll()` driven) |
| Windows — mmeapi   | `core/midi/platform/win/winmidi_device.cpp`     | yes (MIM_LONGDATA buffer queue) | yes | yes, by polling once a second (mmeapi has no notify) | OS callback thread (`midiInOpen` CALLBACK_FUNCTION) | ~1 ms (mmeapi tick) |
| Windows — WinRT    | `core/midi/platform/win/winrt_midi_device.cpp`  | yes | yes | yes (Windows.Devices.Midi watcher) | WinRT ThreadPool | sub-ms typical |
| Android — AMidi    | `core/midi/platform/android/android_midi.cpp`   | yes (raw stream → `AndroidMidiFifo`) | yes | partial (USB host events; BT MIDI via separate path) | JNI thread → SPSC FIFO → caller drains | depends on caller drain cadence |

The cross-platform happy-path audit suite is
`test/test_midi1_backend_audit.cpp`. It intentionally avoids opening real OS
MIDI ports — CI has none — and
asserts only the contract: `create_midi_system()` returns non-null,
enumerate returns sane port descriptors, `create_input()` /
`create_output()` return objects whose `is_open()` starts false, and
`open()` with a bogus id fails gracefully on every backend.

## Per-backend notes

### macOS / iOS — CoreMIDI

Both Apple platforms compile the same production `coremidi_device.mm` and
`ump_session_coremidi.mm` translation units. The iOS Release gate builds them
for `iphonesimulator` and `iphoneos`; its installable Simulator harness creates
uniquely named virtual CoreMIDI source/destination endpoints through Pulp's
process-wide shared client, enumerates them through `create_midi_system()`,
checks id/name/direction, requires an active production `UmpSession`, and proves
real CoreMIDI event-list I/O in both directions before disposal and disappearance.
This is an OS-backed discovery/I/O proof, not an in-process
`VirtualUmpEndpoint` substitute.

The event-list/UMP path is availability-guarded at iOS 14 and macOS 11. Older
Apple runtimes retain the virtual-only `UmpSession` fallback rather than calling
a weak-linked CoreMIDI 2.0 API.

In the gate's current iOS Simulator environment, creating virtual endpoints
without the harness bundle's `UIBackgroundModes=audio` declaration returned
`kMIDINotPermitted`; adding that declaration made the same creation oracle pass.
The harness therefore carries it in generated bundle metadata. This is evidence
for the test fixture, not a general recommendation that production MIDI apps
claim background audio; applications must choose background modes according to
their actual behavior and Apple's current policy.

- **Sysex**: the legacy MIDI-1 receive adapter uses the shared
  `UmpSysex7Reassembler`, so multi-packet sysex spanning callback boundaries
  reassembles correctly. The UMP-native session deliberately delivers the raw
  UMP messages to its callback instead of reassembling them. The
  "second word's top nibble is 0x3" UMP edge case is covered by the dedicated
  reassembler test suite.
- **Hotplug**: CoreMIDI fires a notification callback on the MIDI
  client; Pulp re-enumerates ports on each notification. On macOS the
  callback also fires when the host wakes from sleep, and again two seconds
  later once devices have re-enumerated. CoreMIDI delivers notifications on
  the run loop of the thread that registered the callback, so register it
  from a thread that runs one (normally the main thread).
- **Reconnect after sleep**: a device that vanishes and returns is a new
  endpoint object under the same unique id. A port connected to the old
  object receives nothing, so `is_open()` reports false for an input or
  output whose endpoint is no longer the one registered under its id.
- **Threading**: the `MIDIInputPortCreateWithProtocol` receive block runs on a
  CoreMIDI-managed thread. Pulp invokes the user callback there, but callback replacement is a
  control-thread operation: the backend synchronizes and snapshots the
  `std::function`, so this handoff is thread-safe but is not advertised as
  lock-free or allocation-free. Do not treat it as an audio render callback;
  keep the callback short and pass data onward through a bounded FIFO.
- **UMP**: same backend supports UMP via `ump_session_coremidi.mm`
  (CoreMIDI 2.0 path on macOS 11+ / iOS 14+). Static consumers retain it
  through the explicit anchor called by `UmpSession`; discovery pairs source
  and destination directions only for an unambiguous one-source/one-destination
  CoreMIDI entity. Multi-endpoint entities retain every direction separately;
  endpoint unique IDs are never treated as a pairing key. Cached opens recheck
  the live topology, so hot-unplug makes `is_open()` and `send()` fail and a
  repeated open resolves current endpoints instead of returning a stale handle.

### Linux — ALSA raw MIDI

- **Sysex**: byte-stream from ALSA → `raw_midi_parser` →
  `SysexAccumulator`. Running-status interleave + realtime byte
  interleave handled by `core/midi/include/pulp/midi/running_status.hpp`.
- **Hotplug**: `set_port_change_callback()` starts a runtime-loaded libudev
  monitor on the `sound` subsystem. If libudev or udevd is unavailable, the
  callback is stored but no event fires; clients that need portable hotplug
  awareness should still tolerate manual re-enumeration.
- **Threading**: each opened input port owns a `std::thread`
  (`read_thread_func`) that blocks on `poll()` and dispatches to the
  user's callback. Callback runs on that worker thread — not the audio
  thread, and not the main thread.
- **JACK**: separate optional backend lives under `core/audio` (JACK is
  primarily an audio API; its MIDI plane is bridged separately when
  the JACK audio backend is active).

### Windows — mmeapi (default)

- **Sysex**: `MIM_LONGDATA` buffer queue with 4 pre-allocated 4 KB slots
  (`sysex_slots_`). The driver hands a buffer back when it is full or when
  the closing F7 arrives, so a longer message returns in pieces; the pieces
  are joined and delivered once, complete. Every buffer is re-queued after
  it is read, including ones returned empty, unless the input is closing.
  Shutdown calls `midiInReset` before unprepare so pending sysex buffers
  flush before their storage is released.
- **Hotplug**: mmeapi exposes no device-change notification, so
  `set_port_change_callback()` starts a background thread that lists the
  input and output ports once a second and calls back when the lists differ.
  A plug or unplug is therefore reported up to a second late, on that
  thread. Ports are numbered by position, so unplugging one device renumbers
  the ones after it; they are reported as removed and added. The WinRT
  backend reports changes directly and is the better path on modern Windows.
  This path is compile-checked in CI and has not been run against Windows MIDI
  hardware.
- **Threading**: `midiInOpen` with `CALLBACK_FUNCTION` runs Pulp's
  callback on a Windows MM thread. Same RT-safety rules as the
  CoreMIDI callback.
- **Jitter**: mmeapi's `dwParam2` is millisecond resolution — fine for
  user-input control but not for sample-accurate timing. Use the WinRT
  backend or UMP via Windows MIDI Services if you need higher resolution.

### Windows — WinRT (`Windows.Devices.Midi`)

- **Sysex**: full sysex support via `MidiSystemExclusiveMessage`.
- **Hotplug**: `DeviceWatcher` surfaces add / remove events directly.
- **Threading**: callbacks dispatch on the WinRT ThreadPool.
- **Compat**: Windows 10+ only; older Windows versions fall back to
  the mmeapi path.

### Android — AMidi

- **Backend choice**: Pulp uses NDK `AMidi` where available (API 29+)
  and bridges to `android.media.midi` via JNI on older devices. USB
  host MIDI and BLE MIDI follow the OS's standard discovery flow.
- **Sysex**: raw byte stream → JNI thread → `AndroidMidiFifo` (SPSC
  queue with `kMidiFifoCapacity` slots) → caller drains via
  `AndroidMidiFifo::pop()`. Sysex byte boundaries are preserved.
- **Hotplug**: AMidi reports device add / remove; Pulp surfaces them
  by re-enumerating on the next `enumerate_inputs()` call.
- **Threading**: the JNI callback runs on Android's MIDI thread.
  Pulp's contract here is "fan into the FIFO; let the caller decide
  what thread drains" — there is no implicit callback to user code on
  the Android MIDI thread.

## Device disconnection

`AudioDeviceManager` owns the MIDI inputs a user has enabled, so a cable pull
or a sleep does not leave a silent port behind.

- **Enable and disable**: `set_midi_input_enabled(id, true)` opens the input and
  records it. `set_midi_input_enabled(id, false)` closes it and removes it from
  the saved settings. The enabled set is saved under the user settings when
  `bind_settings()` is used, and is restored on the next launch.
- **Unplug**: when a refresh shows an enabled input is gone, the manager closes
  its handle and keeps the input enabled and saved. Nothing is shown to the
  user as an error.
- **Replug**: when an input with the same identifier appears, the manager
  opens it again with no user action, and events resume on the existing
  `subscribe_midi()` subscriptions.
- **Dead handle behind a listed port**: if an input reports it is no longer
  open while its port is still listed (a device replaced across a sleep), the
  next refresh reopens it. A failed open, such as a busy device, is retried on
  every later refresh.
- **Never enabled, never opened**: an input that was not enabled is not opened
  when it appears, and a disabled input stays closed when it returns.
- **Identifiers**: input and output ports are tracked separately, so a port
  list that numbers both from zero does not confuse them.

Refreshes come from the MIDI system's port-change callback, which
`attach_midi_system()` registers. A system has a single callback slot, so a host
that already uses it passes `false` and calls `refresh_midi_endpoints()` from
its own callback. Subscribers must not enable or disable inputs from inside a
MIDI callback.

## Validation Coverage

The cross-platform happy-path contract is pinned by
`test/test_midi1_backend_audit.cpp`. Per-backend deeper behavior — ALSA
hot-unplug recovery, mmeapi sysex race regression, Android FIFO overflow under
stress — lives in the per-backend test files (`test_winmidi_sysex.cpp` etc.).
