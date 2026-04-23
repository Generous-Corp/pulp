---
name: auv3
description: Audio Unit v3 (AUAudioUnit) format adapter for Pulp — render-block wiring, parameter tree bridging, MIDI / sysex via AURenderEvent, sidechain pulls, state persistence, iOS extension surface, and the pitfalls discovered while wiring the adapter.
---

# AU v3 Skill

Use this skill when touching Pulp's Audio Unit v3 adapter, when
answering questions about how a Pulp plugin behaves inside Logic Pro,
GarageBand, MainStage, AUM, Cubasis, or any AUv3-aware iOS host, or
when an `auval` run fails. AU v3 is one of Pulp's three first-class
first-party formats; unlike AU v2 (which is owned by the `view-bridge`
skill via `au_v2_adapter.cpp`), AU v3 is the modern `AUAudioUnit`
subclass surface.

## When to use

- Editing `core/format/src/au_adapter.mm` — the `PulpAudioUnit`
  `AUAudioUnit` subclass.
- Editing `core/format/src/au_entry.mm` — the
  `AUAudioUnitFactory`-conforming entry object (`PulpAUFactoryObj`)
  and the `PulpAUFactory` component entry symbol.
- Touching `core/format/src/au_audio_unit.h` — the Obj-C forward
  declaration used by `au_entry.mm` and the iOS view controller.
- Editing the iOS extension view controller
  (`core/format/src/au_view_controller_ios.mm`) — but first read the
  `ios` and `view-bridge` skills; that file is **also** mapped to
  them.
- An AUv3 host reports a behaviour issue — sidechain pull missing,
  sysex dropped, parameter tree blank, preset recall fails, MIDI-out
  not delivered.
- An `auval` pass regresses.
- Working on ARA-aware AU — but start with the `ara` skill; the AU
  story there is the `audioUnitARAFactory` KVO property.

AU v2 (`core/format/src/au_v2_adapter.cpp`) is a **separate** adapter
covered by its own `auv2` skill. AU v2 is `AUEffectBase`-based and
used where hosts require the classic v2 Component Manager API. **Do
not edit `au_v2_adapter.cpp` as part of AU v3 work.**

## Files and entry points

| Role | Path |
|---|---|
| Core adapter (Obj-C++) | `core/format/src/au_adapter.mm` |
| Forward declaration used by entry / view | `core/format/src/au_audio_unit.h` |
| Component entry factory | `core/format/src/au_entry.mm` |
| iOS AUv3 extension view controller | `core/format/src/au_view_controller_ios.mm` (also mapped to `ios` + `view-bridge` skills) |
| iOS AU audio session helper | `core/format/src/ios_audio_session.cpp`, `core/format/include/pulp/format/ios_audio_session.{h,hpp}` |
| Info.plist template (AU component bundle) | `tools/cmake/PulpInfoPlist.au.in` |
| AudioUnitSDK fetch (used primarily by AU v2 but shared utilities reach AU v3) | `external/AudioUnitSDK` (Apache 2.0) |
| Tests | `test/test_ios_audio_session.cpp`, `test/test_ios_background_audio_flag.cpp` (iOS-specific); AU v3 shares state / processor tests with CLAP / VST3 |
| CLI validator invocation | `tools/cli/cmd_validate.cpp` — runs `auval` via the `auval-<name>` CTest target |

There is no `PulpAU3.cmake`; AU v3 targets are wired directly in the
top-level CMake plugin helpers alongside AU v2 and the iOS extension
target.

## Core conventions

### `AUAudioUnit`, not `AUEffectBase`

AU v3 uses `AUAudioUnit` as the plugin base class — Apple's modern,
block-based render API. AU v2 (`AUEffectBase`) and AU v3
(`AUAudioUnit`) are **two different C++ classes and two separate
`.component` bundles**. Pulp ships both where applicable; the
v3 subclass is `PulpAudioUnit` in `au_adapter.mm`.

The bridge struct `pulp::format::au::AUBridge` owns:

- `std::unique_ptr<Processor> processor` + `state::StateStore store`
  — the same Pulp DSP + state objects used by the CLAP/VST3 adapters.
- Pre-allocated `output_ptrs`, `input_ptrs`, `sidechain_ptrs` (sized
  to `kMaxChannels = 8`) so the render block never allocates.
- `InputBufferStorage input_abl` and `SidechainBufferStorage
  sidechain_abl` — pre-sized `AudioBufferList` structs for the
  `AURenderPullInputBlock` pulls. The sidechain has its **own** ABL
  so it doesn't alias the main input pull.
- `sidechain_storage` — a `std::vector<float>` backing buffer for the
  sidechain pull so the adapter can stay allocation-free on the audio
  thread after `allocateRenderResources`.

### Entry point: `PulpAUFactory` + `AUAudioUnitFactory`

`au_entry.mm` defines two symbols a host uses:

1. `PulpAUFactoryObj` — a tiny Obj-C class conforming to
   `<AUAudioUnitFactory>`. Its `createAudioUnitWithComponentDescription:error:`
   allocs a `PulpAudioUnit`.
2. `extern "C" void* PulpAUFactory(const AudioComponentDescription*)` —
   the C component-registration symbol the Info.plist points at. It
   calls `pulp_gain_force_link()` to force the static
   `register_plugin` initialisers to link (prevents the linker from
   stripping `au_register.cpp`), then returns a `__bridge_retained`
   pointer to a freshly-allocated `PulpAUFactoryObj`.

If the registered factory is null at entry time, the function returns
`NULL` and a DAW sees "no factory" rather than a crash. The
`force_link` shim is what makes a static-library-only build actually
ship the factory symbol.

### Bus construction

Inside `initWithComponentDescription:…`:

- Output bus: one `AUAudioUnitBus` at 48 kHz default with
  `desc.default_output_channels()` channels.
- Main input bus: when `desc.default_input_channels() > 0`, one
  `AUAudioUnitBus` with that channel count.
- Sidechain bus: when `desc.input_buses.size() > 1` and
  `desc.input_buses[1].default_channels > 0`, a **second** input
  `AUAudioUnitBus` — bus index 1. Hosts connect their sidechain source
  to bus index 1 and the render block pulls it from that index.

This mirrors the CLAP / VST3 "bus 0 = main, bus 1 = sidechain" rule;
see commit `8a960d51 auv3: sidechain input bus + render-block routing
(workstream 01 slice 1.4b)`.

### Parameters: `AUParameterTree`

`- (AUParameterTree *)parameterTree` builds one `AUParameter` per
`StateStore` param:

- `AUParameterAddress` is the Pulp `ParamID` cast to `uint64_t`.
- `unit` is mapped from Pulp's unit string
  (`dB` → `kAudioUnitParameterUnit_Decibels`, `Hz` → `_Hertz`, `%` →
  `_Percent`, boolean-shaped ranges → `_Boolean`, everything else →
  `_Generic`).
- `implementorValueObserver` writes host param changes into
  `store.set_value(id, value)`.
- `implementorValueProvider` reads current values back from the store.
- `implementorStringFromValueCallback` delegates to
  `ParamInfo::to_string` when provided, otherwise a `%.2f` fallback.

`__weak` capture + `strongSelf` null-check pattern is deliberate —
Obj-C blocks on `AUParameterTree` must not retain the audio unit.

### Render block

`- (AUInternalRenderBlock)internalRenderBlock` returns a block that
captures a raw `&_bridge` pointer (Obj-C `__block` / ARC semantics do
not apply — `_bridge` is a C++ struct). The block:

1. Zeroes outputs and returns `noErr` if the Processor is null (host
   calling render before `allocateRenderResources` succeeded).
2. Points `output_ptrs[i]` at `outputData->mBuffers[i].mData`.
3. Pulls the main input via `pullInputBlock(…, 0, &input_abl)`. This
   reuses the **output** buffers as the input destination — in-place
   processing is allowed (`canProcessInPlace` returns `YES`).
4. Pulls the sidechain (if enabled) via
   `pullInputBlock(…, 1, &sidechain_abl)` into the separate backing
   storage; publishes to `processor->set_sidechain(&view)` only on
   success, nulls out the slot on failure.
5. Walks the realtime event list — short MIDI via `AURenderEventMIDI`,
   long / sysex via `AURenderEventMIDIEventList`. See gotchas below.
6. Calls `processor->process(output_view, input_view, midi_in,
   midi_out, ctx)`.
7. Forwards any `midi_out` events back to the host via
   `self.MIDIOutputEventBlock` (AU v3.1+). Each event's
   `sample_offset` is added to `timestamp->mSampleTime`.

### State: `fullState` dictionary

`fullState` wraps `store_.serialize()` bytes inside an `NSData` keyed
`@"pulpState"` within the dictionary returned by `super.fullState`.
`setFullState:` reads `@"pulpState"` back, calls
`store_.deserialize`. The super call is intentional — AUAudioUnit
merges its own internal state (e.g. maximum frames to render) into the
dictionary, and the round-trip must preserve it.

`supportsUserPresets` currently returns `NO`. `currentPreset` is not
overridden — use `fullState` for persistence, not
`AUAudioUnitPreset`. Wiring user presets requires implementing
`userPresets`, `supportsUserPresets`, `saveUserPreset:error:`,
`deleteUserPreset:error:`, `presetStateFor:error:`, and
`currentPreset` as a matched pair.

### ARA companion factory

`audioUnitARAFactory` is a `@property (readonly, nullable) void *` —
the AU-host-observed KVO property that ARA-aware hosts (Logic Pro 11+)
read during scan. It returns
`pulp::format::ara_companion_factory_for(nullptr)`, which is non-null
in `PULP_HAS_ARA` builds where a Processor overrode
`create_ara_document_controller()`. See commit `cb5812c1 ara(au):
audioUnitARAFactory KVO property on PulpAudioUnit (#252)`.

### iOS AUv3 extension

AUv3 on iOS is a UIKit **app extension**. The view controller
(`PulpAUViewController` in `au_view_controller_ios.mm`) is
`AUViewController`-derived and builds a `ViewBridge` against the
extension's loaded `AUAudioUnit` once KVO fires on
`self.audioUnit`. Extension principal class registration is via
`NSExtensionMain`-style Info.plist — see `docs/guides/ios-auv3-guidance.md`
and the `ios` skill for the extension target wiring.

### Two CMake entry points: keep signatures in lockstep

`pulp_add_plugin(...)` (the general entry) and `pulp_add_ios_auv3(...)`
(the iOS-extension wrapper) both end up calling the internal
`_pulp_add_auv3(target name bundle_id version manufacturer category
plugin_code manufacturer_code accepts_midi)` helper with positional
arguments. When you add or remove an arg on `_pulp_add_auv3`, you
must update BOTH wrappers — a missed update on the iOS wrapper
surfaces as:

```
CMake Error at tools/cmake/PulpUtils.cmake:<line> (_pulp_add_auv3):
  _pulp_add_auv3 Function invoked with incorrect arguments
```

only on the iOS toolchain configure, because the other leg
(`pulp_add_plugin`) never exercises the wrapper. Caught on CI's
Coverage-macOS leg in PR #638 when `ACCEPTS_MIDI` was added to
`_pulp_add_auv3` but not threaded through `pulp_add_ios_auv3`.

## Gotchas

### `AURenderEventMIDIEventList` = UMP — not short MIDI, not raw sysex

AU v3.1+ delivers long MIDI and MIDI 2.0 messages through
`AURenderEventMIDIEventList`, which carries a `MIDIEventList` of
`MIDIEventPacket` structs — **UMP-encoded** 32-bit words. Sysex7
arrives as type-3 UMP messages spread across 2-word packets with a
4-bit status field in bits 20–23 of word 0:

```
status == 0x0  → complete single-packet sysex
status == 0x1  → start (reset accumulator)
status == 0x2  → continue
status == 0x3  → end
```

`au_adapter.mm` accumulates start → continue → end spans into one
`add_sysex` call (`#292` / `#288`). Two hard-learned P1 lessons:

1. **Advance the word cursor by `ump_words`, not by 1.** A type-3
   message is 2 UMP words long; advancing by 1 makes the second
   word's header nibble look like a new message header (P1).
2. **Sysex7 size is 0..6 bytes per 2-word packet.** Extract bytes
   from word0 bits 15..0 + word1 bits 31..0, clamped to `size` and to
   6 (#292 P2 — preserve message boundaries).

Both are tested by `test/test_ump_*.cpp`. Touch the accumulator → add
a test that exercises the boundary.

### Short-MIDI length must be validated

`AURenderEventMIDI.length` is the length in bytes. Short messages are
1..3 bytes and `data[0]`'s MSB must be set (status byte). The adapter
explicitly rejects `length == 0`, `length > 3`, and messages with
`(data[0] & 0x80) == 0`. Do not relax that gate — corrupt short
messages past the gate feed `choc::midi::ShortMessage` garbage.

### `_bridge` captured as raw pointer in the render block

The render block captures `&_bridge` (a C++ struct inside the Obj-C
class) as a raw pointer. ARC does not retain `_bridge`. Keeping the
audio unit alive is the host's job; the block lives for the audio
unit's lifetime. **Do not** capture `self` into the render block —
that creates a retain cycle that only breaks when the host drops the
unit, and Logic will reproduce-steps that via preset hot-swap.

The MIDI-out fan-out in the same block does capture `self.MIDIOutputEventBlock`
via ARC (`__block id` style through the implicit-self path). That one
is intentional — the block the host installs is ARC-retained on the
audio unit and does not form a cycle.

### `allocateRenderResourcesAndReturnError` is where `prepare()` lives

Not in `initWithComponentDescription:`. The host may instantiate the
audio unit to enumerate parameters / buses without ever rendering;
calling `Processor::prepare()` before the host has a sample rate +
max frames in hand wastes work and can mis-size buffers. Mirror:
`deallocateRenderResources` calls `processor->release()`.

### `tailTime` is in **seconds**, not samples

Pulp's `descriptor().tail_samples` is an integer sample count;
`tailTime` returns seconds. `< 0` means infinite and returns
`std::numeric_limits<double>::infinity()` (AU's sentinel). Do not
return `0` — a `0` tail tells the host "this plugin emits nothing
after input stops" and delay/reverb tails get chopped.

### Sidechain pull uses its **own** `AudioBufferList`

Aliasing the main `input_abl` into the sidechain pull corrupts the
main input (the pull overwrites it). `sidechain_abl` +
`sidechain_storage` are separate by design — the storage is sized for
`kMaxChannels * max_frames` at `allocate`, with a defensive re-size
inside the render block for the rare case where a host asks for more
frames than `maximumFramesToRender` claimed.

### Factory entry point needs `force_link` to keep registration alive

`pulp_gain_force_link()` is an empty function whose only purpose is to
keep the linker from stripping `au_register.cpp`. The static
initialisers in that TU are what populate the Pulp plugin registry
before `PulpAUFactory` runs. If you add a new auto-registering TU,
either reference it from `force_link` or the linker will drop it in
release builds — and `registered_factory()` returns null at AU
instantiation time.

### Channel count hard limit of 8

`kMaxChannels = 8`. Bumping that requires re-sizing every
pre-allocated buffer array and validating hosts don't ask for more
channels than the descriptor declares. Not a surround-readiness flag
yet.

### No `kAudioUnitProperty_CocoaUI` v3-native view plumbing today

AU v3 uses `requestViewControllerWithCompletionHandler:` to fetch an
`AUViewController`. macOS bundles that ship a desktop editor for an AU
v3 `.component` rely on the AU v2 Cocoa view path
(`au_v2_cocoa_view.mm`) for the editor. iOS bundles use the
`AUViewController` subclass directly. Cross-platform editor work lives
in the `view-bridge` skill.

### `auval` is the AU gate

`auval` ships with macOS; `pulp validate` wraps the CTest target
`auval-<name>` rather than running `auval` directly. On a raw
development machine, run manually via e.g.
`auval -v aufx MyPl Plup`. A freshly built `.component` that was just
copied into `~/Library/Audio/Plug-Ins/Components/` requires a cached-
plist rebuild — delete
`~/Library/Caches/AudioUnitCache/` and `~/Library/Caches/com.apple.audiounits.cache`
(or call `killall -9 AudioComponentRegistrar`) before validating a new
bundle.

### iOS extension principal class is declared in Info.plist

AUv3 iOS extensions use `NSExtensionPrincipalClass` =
`PulpAUViewController` in the extension target's Info.plist, not
`NSExtensionMain`. If the extension fails to load in a host (Cubasis /
AUM), check the Info.plist before the Obj-C — a typo in the principal
class name fails silently.

### `PulpAUViewController::dealloc` — never call `_bridge->close()` explicitly

The view controller declares its ivars `_bridge` (ViewBridge), then
`_viewHost` (PluginViewHost), then `_fallbackView`. When `[super
dealloc]` runs, the runtime destroys C++-typed ivars in REVERSE
declaration order: `_fallbackView`, `_viewHost`, `_bridge`. That
ordering is load-bearing:

1. `~PluginViewHost` runs second. It calls
   `root_.set_plugin_view_host(nullptr)` — the View `root_`
   references is still alive (still owned by `_bridge->view_`), so
   the call is safe and clears the back-pointer.
2. `~ViewBridge` runs last. Its destructor calls `close()` →
   `Processor::on_view_closed` → `view_.reset()` destroys the View.
   The back-pointer was already cleared in step 1, so the View's
   own teardown can't reach a dead host.

Calling `_bridge->close()` HERE explicitly (before `[super dealloc]`)
reverses that order: the View dies first, then `~PluginViewHost`
dereferences a dangling `root_` reference and crashes AUv3 editor
close. Codex P1 review on PR #653 caught this — the fix is to remove
the explicit close, NOT to add it.

## Validation recipes

Build and validate via the Pulp CLI:

```bash
./build/tools/cli/pulp build
./build/tools/cli/pulp validate         # runs auval via the auval-<name> CTest target
```

Manual `auval` (macOS only — `auval` is an Apple tool):

```bash
# List all registered AUs; find yours in the list
auval -a

# Validate an effect (type/subtype/manufacturer are 4-char codes)
auval -v aufx MyPl Plup

# Validate an instrument
auval -v aumu MySy Plup
```

If `auval -a` doesn't list the plugin, the AU cache is stale. Reset
it:

```bash
killall -9 AudioComponentRegistrar
rm -rf ~/Library/Caches/AudioUnitCache/ \
       ~/Library/Caches/com.apple.audiounits.cache
```

`auval -r` runs the longer reinit-stress pass; use it before shipping
a release but not on every iteration — it takes minutes.

iOS: no standalone `auval`-equivalent. Run the AUv3 extension in the
AUHost sample app (available from Apple's developer portal) or inside
AUM / Cubasis to smoke-test instantiation + render. See the `ios`
skill for device deploy.

## Cross-references

- `.agents/skills/ios/SKILL.md` — iOS extension wiring, simulator
  deploy, audio session handling.
- `.agents/skills/view-bridge/SKILL.md` — editor contract. On iOS,
  `au_view_controller_ios.mm` is the canonical AUv3 example of the
  protocol.
- `.agents/skills/auv2/SKILL.md` — the AU v2 adapter, separate bundle.
- `.agents/skills/ara/SKILL.md` — `audioUnitARAFactory` KVO property.
- `.agents/skills/mpe/SKILL.md` — MPE sidecar contract (AU v3 delivers
  MPE as short MIDI via `AURenderEventMIDI`; the Pulp path is the same
  `MpeVoiceTracker` as CLAP / VST3).
- `.agents/skills/clap/SKILL.md` and `.agents/skills/vst3/SKILL.md` —
  cross-format parity sanity-check for host-specific regressions.
- `docs/guides/ios-auv3-guidance.md` — the human-facing iOS AUv3 guide.
- `docs/guides/formats.md` — user-facing format overview + auval
  recipes.
- Memory note: AAX-parity sweep — AU sysex (#288), VST3 sysex (#274),
  CLAP sysex (#269) and AAX sysex (#239) all share the same
  sidecar. Fixing one means checking the other three.
