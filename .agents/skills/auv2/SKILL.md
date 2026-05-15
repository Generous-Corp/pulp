---
name: auv2
description: Audio Unit v2 adapter work for Pulp — picking the right AU component type (aufx/aumf/aumi/aumu), wiring MIDI input, and avoiding the DAW-side component cache that silently masks repackaging.
requires:
  scripts: []
  tools: []
---

# AU v2 Skill

Use this when you are:

- Touching `core/format/src/au_v2_adapter.cpp` / `.hpp` or the AU v2 instrument adapter
- Changing how Pulp packages AU v2 plug-ins in CMake (`tools/cmake/PulpUtils.cmake`, `tools/cmake/PulpInfoPlist.au.in`)
- Debugging "plug-in loads but never receives MIDI" reports against an AU host
- Picking or changing a plug-in's AU component type (4-char `type` in the Info.plist)
- Wiring new AU v2 features that require splitting base-class inheritance (adding `AUMIDIBase`, `MIDIOutput`, etc.)

Scope is AU v2 only. AU v3 (AUAudioUnit-based app extensions) has different rules and lives behind the `ios` skill + `core/format/src/au_adapter.mm`.

## AU v2 Component Types — Pick the Right 4cc

Hosts route MIDI based on the bundle's `type` field. Getting this wrong produces a silent-failure: the plug-in scans, loads, renders audio, and never sees a MIDI event.

| `type` | Constant                    | Audio I/O | MIDI I/O | When to use                                |
|--------|-----------------------------|-----------|----------|--------------------------------------------|
| `aufx` | `kAudioUnitType_Effect`       | in + out  | none     | Audio-only effect (compressor, EQ, reverb) |
| `aumf` | `kAudioUnitType_MusicEffect`  | in + out  | in       | Effect that wants inbound MIDI (arpeggiator on audio, MIDI-triggered gate, vocoder w/ MIDI carrier) |
| `aumi` | `kAudioUnitType_MIDIProcessor`| none      | in + out | MIDI-only processor (transpose, arp, chord, note filter) |
| `aumu` | `kAudioUnitType_MusicDevice`  | out only  | in       | Instrument / synth                         |

**Load-bearing rule:** Logic, MainStage, GarageBand, and other AU hosts will **never** call `MIDIEvent` / `HandleMIDIEvent` on an `aufx`-typed plug-in. If a plug-in's `Processor::descriptor()` sets `accepts_midi = true` and the bundle is still packaged as `aufx`, MIDI silently disappears — the adapter looks correct, the Info.plist looks correct to a casual reader, but no MIDI arrives.

Pulp's `pulp_add_plugin()` automates the choice from two inputs:

1. `CATEGORY` — `Effect` | `Instrument` | `MidiEffect`
2. `ACCEPTS_MIDI` — bool option that mirrors `PluginDescriptor::accepts_midi`

The resulting mapping (`_pulp_add_au` / `_pulp_add_auv3` in `tools/cmake/PulpUtils.cmake`):

```
(Instrument,  *)     -> aumu
(MidiEffect,  *)     -> aumi
(Effect,      true)  -> aumf    <-- easy to forget
(Effect,      false) -> aufx
```

When you add a new example or change an existing one's descriptor to declare `accepts_midi = true`, you **must** also add `ACCEPTS_MIDI` to its `pulp_add_plugin()` call. There is no runtime fallback — the two surfaces are independent and the CMake flag is what ends up in the Info.plist.

## MIDI Input Wiring

The AU v2 effect adapter inherits from `AUMIDIEffectBase` (`AUEffectBase` + `AUMIDIBase`) so the SDK's `MIDIEvent` / `SysEx` entry points exist. Inbound MIDI flows:

```
host -> AUMIDIBase::MIDIEvent(status, data1, data2, frame)
     -> AUMIDIBase::HandleMIDIEvent(strippedStatus, channel, data1, data2, frame)
     -> PulpAUEffect::HandleMIDIEvent(...)   <-- our override
        - lock midi_mutex_
        - push MidiEvent into pending_midi_
```

At the top of `ProcessBufferLists()` we drain under the same lock:

```
lock midi_mutex_
midi_in = std::move(pending_midi_)
pending_midi_ = {}
unlock
midi_in.sort()    // sample-accurate ordering
processor_->process(..., midi_in, midi_out, ctx)
```

The instrument adapter (`core/format/src/au_v2_instrument.cpp`) uses the same `pending_midi_` + `midi_mutex_` pattern against the `MusicDeviceBase` base class. If you're adding a third MIDI-aware AU, mirror that shape exactly — resist the urge to share a mixin until there's a third entry to fold.

### Decode helper: `decode_midi_event()`

`AUMIDIBase::HandleMIDIEvent` delivers the status byte already split into a top nibble and a separate channel. The free function `pulp::format::au::decode_midi_event(status, channel, data1, data2)` in `core/format/include/pulp/format/au_v2_adapter.hpp` recombines them into a `choc::midi::ShortMessage` with the correct on-the-wire status byte and returns a `MidiEvent` with `sample_offset == 0`. Tests cover CC, pitch bend, note-on, program change, and system messages (status 0xF0+ keep their literal byte — channel nibble is ignored).

### SysEx

`AUMIDIBase::HandleSysEx(data, length)` does not carry a per-event sample offset at this SDK layer. We enqueue the payload with `sample_offset == 0` so it is delivered at the leading edge of the current `ProcessBufferLists()` block.

## Current Gaps

- **MIDI output from AU v2 effects** is not wired yet (tracked as #626). `Processor::process()` can write to `midi_out`, but `PulpAUEffect` has no render-notify callback / `MIDIOutput` mixin that emits those events back to the host. Effects that declare `produces_midi = true` work in CLAP / VST3 but stay silent on AU v2. `descriptor.produces_midi` is *not* wired to a CMake flag yet — the AU type selection is driven entirely by `accepts_midi`.

- **AU v3 parity** for MIDI on effects is not re-audited in this pass. If you touch `core/format/src/au_adapter.mm`, confirm the AUv3 `componentType` logic in `_pulp_add_auv3` still matches the fix in `_pulp_add_au`.

## Gotchas

### DAW component cache — clear it after a `type` change

Logic, MainStage, GarageBand, Studio One, Live, and every other AU host maintain a **host-side cache** of AU descriptors, keyed on subtype + manufacturer. When you change a plug-in from `aufx` to `aumf` (or vice versa) *without* also changing the subtype, hosts will keep the cached-old-type descriptor and behave as if the fix never shipped — you'll install a fresh `.component` and the host will still treat it as `aufx`. Symptoms: rebuilt plug-in appears in the correct MIDI-effect slot of the host UI only after a restart, or never appears at all.

Mitigation when you test a type change locally:

```bash
# Kill the AU registration cache so the next host launch re-inspects the bundle.
killall -9 AudioComponentRegistrar 2>/dev/null || true

# Logic / MainStage / GarageBand — clear the AU cache next to the host DBs.
rm -rf ~/Library/Caches/AudioUnitCache
rm -rf ~/Library/Caches/com.apple.audiounits.cache

# auval rescan catches the new type without needing a host restart.
auval -a | grep <subtype>
auval -v <type> <subtype> <manufacturer>
```

Document this step in any issue or PR that flips a shipped plug-in's component type.

### `AUEffectBase` vs `AUMIDIEffectBase`

If you see `HandleMIDIEvent` that never fires: check the base class. `AUEffectBase` alone has no `AUMIDIBase` mixin — the SDK only wires `MIDIEvent` dispatch when the class multiply inherits `AUMIDIBase` (directly or via `AUMIDIEffectBase` / `MusicDeviceBase`). When you add a new AU v2 adapter, inheriting from `AUMIDIEffectBase` is cheap even for audio-only effects — the class does nothing extra until the host actually delivers MIDI, and it future-proofs the adapter against a later `accepts_midi` flip.

### `GetProperty` / `GetPropertyInfo` chain

With `AUMIDIEffectBase`, fall-through calls should go to `AUMIDIEffectBase::GetProperty(...)`, not `AUEffectBase::GetProperty(...)`. `AUMIDIEffectBase::GetProperty` tries `AUEffectBase::GetProperty` first and then falls back to `AUMIDIBase::DelegateGetProperty`. Calling `AUEffectBase` directly skips the MIDI-mapping property delegation — hosts that query `kAudioUnitProperty_AllParameterMIDIMappings` would silently return no mapping.

### CXX_STANDARD on tests that include AU adapter headers

`core/format/include/pulp/format/au_v2_adapter.hpp` pulls `AudioUnitSDK/AUMIDIEffectBase.h`, which on AudioUnitSDK 1.4 uses `std::expected` (C++23). Apple clang only exposes `std::expected` when the *consuming TU* compiles at `-std=c++23`. Any test executable that includes the adapter header must set `CXX_STANDARD 23` explicitly — linking `pulp::format` is not enough because CMake treats `CMAKE_CXX_STANDARD=20` at the root as authoritative per target. See `core/format/CMakeLists.txt` for the equivalent pin.

### `pending_midi_` mutex is a slow-path correctness tool, not a fast path

The `std::mutex` guarding `pending_midi_` is contended only on the MIDI-delivery thread (where the host calls `HandleMIDIEvent`) and the audio thread (once per block, to drain). It is NOT the right primitive for per-event audio-thread publication. Do not extend this pattern to any new path that runs multiple times per block — switch to `choc::fifo::SingleReaderSingleWriterFIFO` if you need lock-free MIDI delivery inside a single block.

### `AUMIDIBase` splits the status byte for EVERY message

`AUMIDIBase::MIDIEvent` (AudioUnitSDK 1.4 `AUMIDIBase.h`) unconditionally splits the wire-format status byte before dispatching:

```
strippedStatus = inStatus & 0xF0   // -> HandleMIDIEvent's inStatus
channel        = inStatus & 0x0F   // -> HandleMIDIEvent's inChannel
```

The split happens for system messages (0xF0-0xFF) the same way as for channel-voice (0x80-0xEF). For 0xF8 (timing clock) the SDK calls `HandleMIDIEvent(inStatus=0xF0, inChannel=0x08, ...)`. The decoder MUST reassemble `(inStatus & 0xF0) | (inChannel & 0x0F)` regardless of the top nibble — special-casing system messages and returning `inStatus` unchanged turns every clock / start / stop / song-position into 0xF0 (sysex start). Codex review on PR #638 caught the buggy special case; the unit test in `test/test_au_v2_effect.cpp` now feeds the post-split shape (status=0xF0, channel=0x08) so the regression cannot reappear without flipping a test red.

### `AUSDK_RTSAFE` position with `override` — Xcode 16.4 incompat

`AUSDK_RTSAFE` expands to `[[clang::nonblocking]]`. AudioUnitSDK's own base-class declarations use `... AUSDK_RTSAFE;` (no `override`), but placing the attribute between a function declarator and the `override` virt-specifier in a derived class compiles under older Xcode and fails on Xcode 16.4 / Clang 17+ with:

```
error: expected ';' at end of declaration list
```

The attribute is a static-analysis hint only — dropping it from derived-class `override` declarations has no runtime effect. `PulpAUInstrument::HandleNoteOn/Off` (the reference pattern for AU v2) doesn't carry `AUSDK_RTSAFE` either. When writing a new AU v2 override that matches an `AUSDK_RTSAFE` base declaration, omit the attribute. Caught on CI's Coverage-macOS leg in PR #638 after the AU v2 effect MIDI fix landed without it.

### Editor `dealloc` ordering — never call `bridge->close()` explicitly

`PulpAUEditorOwnership` (in `core/format/src/au_v2_cocoa_view.mm`) declares its members as `unique_ptr<ViewBridge> bridge` then `unique_ptr<PluginViewHost> host`. C++ destroys members in REVERSE declaration order, so when `delete _ownership` runs in `PulpAUEditorOwner::dealloc`:

1. `~PluginViewHost` runs first. The host calls `root_.set_plugin_view_host(nullptr)` to clear the View → host back-pointer. The View it references is still alive (still owned by `bridge->view_`), so the call is safe.
2. `~ViewBridge` runs second. Its destructor calls `close()` → `Processor::on_view_closed(*view_raw_)` fires → `view_.reset()` destroys the View. The back-pointer was already cleared in step 1, so the View's own teardown can't reach a dead host.

Calling `_ownership->bridge->close()` HERE explicitly (BEFORE `delete _ownership`) reverses that order: the View dies first, then `~PluginViewHost` dereferences a dangling `root_` reference and crashes the AU v2 editor close path. Codex P1 review on PR #653 caught this — the fix is to remove the explicit close, NOT to add it. Same rule applies to any future Cocoa-View ownership wrapper that mixes a `ViewBridge` and a `PluginViewHost` in the same C++ scope.

## Reference pointers

- Adapter source: `core/format/src/au_v2_adapter.cpp`, `core/format/include/pulp/format/au_v2_adapter.hpp`
- Instrument adapter (reference pattern): `core/format/src/au_v2_instrument.cpp`, `core/format/include/pulp/format/au_v2_instrument.hpp`
- Cocoa view factory: `core/format/src/au_v2_cocoa_view.mm` (owned by `view-bridge` + `ios` skills)
- CMake selector: `tools/cmake/PulpUtils.cmake` — `_pulp_add_au` and `_pulp_add_auv3`
- Info.plist template: `tools/cmake/PulpInfoPlist.au.in`
- AudioUnitSDK reference: `external/AudioUnitSDK/include/AudioUnitSDK/AUMIDIBase.h`, `AUMIDIEffectBase.h`
- Support matrix entry: `docs/status/support-matrix.yaml` — `formats.au_v2` and `format_limitations.au_v2`
- Tests:
    - `test/test_au_v2_effect.cpp` — decode / sysex smoke
    - `test/cmake/test_au_v2_type_selection.cmake` — aumf/aufx/aumu/aumi mapping
