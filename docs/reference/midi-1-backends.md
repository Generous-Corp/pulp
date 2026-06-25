# MIDI 1.0 Backends per OS

Status: **audit document**.

Pulp's MIDI 1.0 surface is `pulp::midi::create_midi_system()` →
`MidiSystem::create_input()` / `create_output()`. Each OS ships a backend
implementation under `core/midi/platform/<os>/`. This file records what
ships today, per backend.

## Matrix

| OS / API           | Source                                          | Sysex (in) | Sysex (out) | Hotplug | Callback thread        | Typical jitter |
|--------------------|-------------------------------------------------|------------|-------------|---------|------------------------|----------------|
| macOS — CoreMIDI   | `core/midi/platform/mac/coremidi_device.mm`     | yes (UMP sysex7 reassembled) | yes | yes (CoreMIDI notify) | CoreMIDI thread (high QoS) | sub-ms typical |
| iOS — CoreMIDI     | shared with macOS path                          | yes (UMP sysex7 reassembled) | yes | yes | CoreMIDI thread        | sub-ms typical |
| Linux — ALSA raw MIDI | `core/midi/platform/linux/alsa_midi_device.cpp` | yes (raw-byte stream + `SysexAccumulator`) | yes | partial (runtime libudev monitor; manual re-enumeration fallback) | dedicated `std::thread` per port | ~1 ms (`poll()` driven) |
| Windows — mmeapi   | `core/midi/platform/win/winmidi_device.cpp`     | yes (MIM_LONGDATA buffer queue) | yes | no (mmeapi has no notify; re-enumerate) | OS callback thread (`midiInOpen` CALLBACK_FUNCTION) | ~1 ms (mmeapi tick) |
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

- **Sysex**: receive path uses the shared `UmpSysex7Reassembler` so
  multi-packet sysex spanning callback boundaries reassembles correctly. The
  "second word's top nibble is 0x3" UMP edge case is covered by the dedicated
  reassembler test suite.
- **Hotplug**: CoreMIDI fires a notification callback on the MIDI
  client; Pulp re-enumerates ports on each notification. Not currently
  exposed as a public event — clients poll `enumerate_inputs()`.
- **Threading**: `MIDIClientCreate` runs the callback on CoreMIDI's
  own real-time thread. Pulp's callback runs there directly — keep it
  short, allocation-free, and pass data to your audio / UI thread via
  lock-free FIFOs.
- **UMP**: same backend supports UMP via `ump_session_coremidi.mm`
  (CoreMIDI 2.0 path on macOS 11+ / iOS 14+).

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

- **Sysex**: `MIM_LONGDATA` buffer queue with 4 pre-allocated slots
  (`sysex_slots_`). On callback the slot is dispatched, then re-prepared
  and re-queued. Shutdown calls `midiInReset` before unprepare so pending
  sysex buffers flush before their storage is released.
- **Hotplug**: not supported. mmeapi exposes no device-change
  notification; clients must call `enumerate_inputs()` on a timer if
  hotplug awareness is required. The WinRT backend is the supported
  hotplug path on modern Windows.
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

## Validation Coverage

The cross-platform happy-path contract is pinned by
`test/test_midi1_backend_audit.cpp`. Per-backend deeper behavior — ALSA
hot-unplug recovery, mmeapi sysex race regression, Android FIFO overflow under
stress — lives in the per-backend test files (`test_winmidi_sysex.cpp` etc.).
