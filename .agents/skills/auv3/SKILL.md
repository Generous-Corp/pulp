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
- `param_events` (`state::ParameterEventQueue`) — cleared each render
  block and filled from inbound AU parameter/ramp render events with
  their sample offsets while the StateStore dual-write keeps normal
  block-level parameter reads current.

### Entry point: `PulpAUFactory` + `AUAudioUnitFactory`

**macOS (Phase 3.5):** the .appex stub uses Apple's `_NSExtensionMain`
entry point. `NSExtensionPrincipalClass` in Info.plist points at
`PulpAUMacViewController`, which adopts `AUAudioUnitFactory` directly
— its `createAudioUnitWithComponentDescription:error:` allocs a
`PulpAudioUnit`. The legacy `PulpAUFactory` C component-registration
function is NOT used on macOS; everything goes through PlugInKit's
extension lifecycle. See the "macOS AU v3 packaging" section below
for the full framework + stub .appex + container .app architecture.

**iOS (legacy monolithic .appex):** `au_entry.mm` still defines
`PulpAUFactoryObj` (NSObject conforming to `<AUAudioUnitFactory>`) and
the C entry symbol `PulpAUFactory(const AudioComponentDescription*)`.
iOS uses the same `_NSExtensionMain` path via Apple's
`AUViewController`-based principal class
(`PulpAUViewController` in `au_view_controller_ios.mm`); the C entry
remains as a no-op safety net for `AudioComponentInstantiate`-style
direct loads. The `PULP_AUV3_PLUGIN()` macro (in
`<pulp/format/au_v3_entry.hpp>`) is what registers the per-plugin
processor factory at static init — no `force_link` shim required.

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
  `store.set_value_rt(id, value)`. **AUv3 hosts may invoke the observer
  from arbitrary threads, including the render thread** — use
  `set_value_rt`, not the generic `set_value`. The RT path writes the
  atomic and pushes an SPSC event for `ListenerThread::Main` listeners;
  the editor drains via `store.pump_listeners()`. The generic path
  would heap-allocate the dispatch lambda on a possibly-audio thread.
  See Slice 2 in `planning/2026-05-18-rt-safety-and-debug-dx.md`.
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
5. Walks the realtime event list — parameter/ramp events append to
   `param_events` and call `store.set_value_rt`, short MIDI arrives via
   `AURenderEventMIDI`, and long / sysex arrives via
   `AURenderEventMIDIEventList`. See gotchas below.
   The sorted `param_events` queue is attached to the processor via
   `set_param_events(&param_events)` before render, so
   `Processor::param_events()` exposes the same sample offsets.
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

## macOS AU v3 packaging — framework + stub .appex + container .app (Phase 3.5)

**Apple's architecture, not a Pulp invention. Get this wrong and you
will spend an entire session debugging silent Pluginkit rejections.**

Apple's "Creating custom audio effects" sample doc states verbatim:

> "Your extension's main binary cannot be dynamically loaded into
> another app, which means all executable AU code must reside in a
> separate framework bundle. The extension target still needs to
> contain at least one source file for the extension binary to be
> created, properly loaded, and linked with the framework bundle."

iPlug2 ships the same 3-tier architecture. Pulp's macOS AU v3 lane
(`tools/cmake/PulpAuv3.cmake`'s `_pulp_add_auv3_macos_*` helpers,
Phase 3.5) follows this pattern exactly:

```
ChainerSynth.app/                                 ← container .app
├── Contents/
│   ├── MacOS/ChainerSynth                        ← tiny Cocoa shell, launched once to register
│   ├── PlugIns/
│   │   └── ChainerSynth.appex/                   ← stub .appex (NSExtensionMain entry)
│   │       └── Contents/
│   │           ├── Info.plist
│   │           │   • NSExtensionPointIdentifier = com.apple.AudioUnit-UI
│   │           │   • NSExtensionPrincipalClass = PulpAUMacViewController
│   │           │   • NSExtensionAttributes.AudioComponentBundle =
│   │           │       <bundle-id>.AUv3Framework    ← MUST match framework's CFBundleIdentifier
│   │           └── MacOS/ChainerSynth            ← ~50KB stub binary, links framework
│   └── Frameworks/
│       └── ChainerSynthAUv3Framework.framework/  ← REAL code lives here
│           ├── Info.plist  (CFBundlePackageType=FMWK)
│           └── Versions/A/
│               ├── ChainerSynthAUv3Framework     ← contains PulpAudioUnit + PulpAUMacViewController
│               └── libwgpu_native.dylib          ← any embedded dylibs
```

**iOS is different — iOS AU v3 still uses the monolithic .appex.** The
framework split is macOS-specific because of Apple's `loadInProcess`
out-of-process requirement on macOS. `pulp_add_ios_auv3()` stays on the
legacy monolithic path; `pulp_add_plugin(FORMATS AUv3)` dispatches to
the macOS framework path on macOS.

### What goes in the framework vs the .appex stub

- **Framework** (`_pulp_add_auv3_macos_framework`): the per-plugin
  Core OBJECT lib + `au_adapter.mm` (PulpAudioUnit) +
  `au_view_controller_mac.mm` (PulpAUMacViewController +
  AUAudioUnitFactory) + per-plugin `au_v3_entry.cpp` (the
  `PULP_AUV3_PLUGIN` macro that registers the processor factory).
- **Stub .appex** (`_pulp_add_auv3_macos_appex`): a generated 1-function
  `.mm` source — `void Pulp_<plugin>_AUv3_keep_alive(void)`. Entry
  point is Apple-provided `_NSExtensionMain`; we pass
  `-e _NSExtensionMain` and `-fapplication-extension`. The stub links
  the framework with `-Wl,-force_load,$<TARGET_FILE:framework>` so its
  Obj-C classes register with the runtime — without `-force_load`,
  `NSClassFromString(@"PulpAUMacViewController")` returns nil and the
  host fails to instantiate the AU.
- **Container .app** (`_pulp_add_auv3_macos_host`): tiny Cocoa shell
  with a "this is the registration host" placeholder window. Bundle ID
  `<plugin-bundle-id>.AUv3Host`. The user runs it once after install
  to trigger Launch Services scan.

**Do NOT put `au_entry.mm`'s `PulpAUFactoryObj` (legacy
AudioComponentRegister factory C function) anywhere in the macOS AU v3
lane.** The macOS path uses `_NSExtensionMain` + `NSExtensionPrincipalClass`
to find the factory class. iPlug2 follows the same convention.

### rpath: 4 levels up, not 2

The .appex's binary at
`MyApp.app/Contents/PlugIns/MyApp.appex/Contents/MacOS/MyApp` needs to
find the framework at `MyApp.app/Contents/Frameworks/`. From the
binary, that's **4 parent dirs up** (`MacOS → Contents → MyApp.appex →
PlugIns → Contents → Frameworks`):

```cmake
set_target_properties(${appex_target} PROPERTIES
    INSTALL_RPATH "@executable_path/../../../../Frameworks")
```

**iPlug2's modern CMake helper has this wrong** — it sets
`@executable_path/../../Frameworks` which works for iOS's flat .appex
layout but breaks macOS where the .appex has its own
`Contents/MacOS/`. Don't copy that recipe.

The container .app's binary at `MyApp.app/Contents/MacOS/MyApp` needs
2 parent dirs up: `INSTALL_RPATH "@executable_path/../Frameworks"`.

### Signing + notarization is mandatory on Sequoia/Tahoe

macOS Tahoe's Pluginkit silently rejects ad-hoc-signed, Developer-ID-
signed-without-notarization, and even properly Developer-ID-signed but
unnotarized AU v3 .appex bundles. `pluginkit -mAvvv -p com.apple.AudioUnit-UI`
returns "no matches" with zero log diagnostics. The only signal you
get is the absence of the plugin.

You MUST:

1. **Sign embedded dylibs first** (`libwgpu_native.dylib`, etc.) with
   `--timestamp --options runtime` and the same Developer ID identity
2. **Sign the framework**
3. **Sign the .appex** with `--entitlements <sandbox>.plist` (the
   `com.apple.security.app-sandbox` entitlement is REQUIRED for app
   extensions; without it pkd logs "plug-ins must be sandboxed" and
   rejects). Plus `allow-jit` + `allow-unsigned-executable-memory` +
   `disable-library-validation` for JS-engine + Skia/Dawn editors.
4. **Sign the container .app** (also with hardened-runtime entitlements
   for library validation)
5. **Notarize the container .app** via `xcrun notarytool submit
   --apple-id <id> --team-id <team> --password <app-specific-pwd>
   --wait`
6. **Staple** with `xcrun stapler staple`
7. **Install to /Applications** and `lsregister -f -R`
8. **Open the container .app once** to trigger Launch Services scan
   → Pluginkit then registers the embedded extension

The full recipe is in `tools/scripts/sign-notarize-auv3-mac.sh`.

**Diagnostic for silent Pluginkit rejection:**

```bash
# Should return the plugin's bundle ID + path
pluginkit -mAvvv -p com.apple.AudioUnit-UI | grep <your-plugin>

# Should be registered as an AU component
auval -a | grep <your-fourcc>

# Should pass FORMAT + RENDER tests (validates the AU loads + processes
# audio in AUHostingServiceXPC out-of-process)
auval -v aumu <subtype> <manufacturer>
```

**auval does NOT exercise the AU v3 controller path** — auval calls
`AudioComponentInstantiate` directly, bypassing the
`AUAudioUnitFactory` lifecycle that hosts use via XPC. Threading bugs
in `createAudioUnitWithComponentDescription:error:` /
`PulpAUMacViewController` will pass auval and crash inside Logic /
Reaper / Ableton. A proper integration test needs an XPC client that
calls `requestViewControllerWithCompletionHandler` — Apple's AUv3Host
sample is the template.

### CMake POST_BUILD embed step doesn't re-fire on framework-only edits

Without a sentinel, `add_custom_command(TARGET host POST_BUILD ... cp
framework into app)` only runs when the host target itself relinks.
A framework-only source edit (e.g. tweaking
`au_view_controller_mac.mm`) won't relink the host, so the embedded
framework in the .app stays stale. You sign + notarize the OLD binary
while thinking you're testing the new one — symptom: the same
crash repeats with the same byte offset after every "rebuild".

`PulpAuv3.cmake` fixes this with a stamp-file `add_custom_command` +
`add_custom_target(${host}_Embed ALL DEPENDS stamp)`. The host's
output triggers the embed step whenever the framework or .appex
binary is newer than the stamp. Don't revert to plain POST_BUILD.

`tools/scripts/sign-notarize-auv3-mac.sh` also re-syncs the embed at
sign time as a belt-and-suspenders.

### Threading: `createAudioUnit:error:` runs on the XPC queue, NOT main

The host (Logic / Reaper / Ableton / AUM) invokes
`-[PulpAUMacViewController createAudioUnitWithComponentDescription:error:]`
on the `com.apple.NSXPCConnection.user.endpoint` serial queue, not the
main thread. Any AppKit/UIKit call from there throws
`NSInternalInconsistencyException` (`setPreferredContentSize:`,
`self.view`, the `PluginViewHost::attach_to_parent` AppKit attach).
The thrown exception kills the .appex process and Logic reports
"Failed to load Audio Unit".

The fix in `au_view_controller_mac.mm` is a HARD GUARD at the top of
`rebuildEditorIfReady`:

```objc
- (void)rebuildEditorIfReady {
    if (![NSThread isMainThread]) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self rebuildEditorIfReady];
        });
        return;
    }
    // ... AppKit work
}
```

**Don't just guard at `setAudioUnit:` (the property setter).** The
compiler can inline through the property setter when `createAudioUnit`
assigns to `self.audioUnit`, bypassing your thread check. The hard
guard inside `rebuildEditorIfReady` is the only safe place. Same
gotcha on iOS — `au_view_controller_ios.mm` has the same guard.

### Logic's per-plugin failed-state cache

Logic Pro remembers AU v3 plugins that previously failed to validate
in `~/Library/Preferences/com.apple.logic10.plist` under
`audioUnitConfig.<type>-<subtype>-<manufacturer>`. Working entries are
populated dicts; failed entries are `<dict/>` (empty). Logic will
**not** re-attempt loading an empty-dict entry on relaunch — even
after you've fixed the bug and reinstalled, Logic refuses to list the
plugin until you delete that entry.

Recovery without a full AU rescan:

```bash
# Logic Pro must be QUIT first
killall -9 cfprefsd

# Edit on disk while cfprefsd is dead so it reads fresh on next access
plutil -convert xml1 -o /tmp/logic10.xml ~/Library/Preferences/com.apple.logic10.plist
sed -i.bak '/<key>aumu-Chnr-Pulp<\/key>/{N;d;}' /tmp/logic10.xml
plutil -convert binary1 -o ~/Library/Preferences/com.apple.logic10.plist /tmp/logic10.xml
killall -9 cfprefsd AudioComponentRegistrar pkd
rm -f ~/Library/Caches/AudioUnitCache/com.apple.audiounits.cache \
      ~/Library/Caches/AudioUnitCache/com.apple.audiounits.sandboxed.cache

# Now launch Logic — it'll incrementally rescan (NOT a full scan) and
# pick up the fresh registration from AudioComponentRegistrar.
```

`PlistBuddy` does NOT work for editing this — it talks to cfprefsd
which serves a cached in-memory view of the plist. Edit the XML
directly while cfprefsd is killed.

### Phase 3 / 3.5 view configuration plumbing

`PulpAUMacViewController` + `PulpAUViewController` implement
`AUAudioUnitFactory`, open `ViewBridge` against the AU's real
`pulpProcessor` + `pulpStore`, build `PluginViewHost` via
`decide_gpu_host`, and call `set_design_viewport(w, h)` +
`set_fixed_aspect_ratio(w/h)` so the editor paints at design size and
host-driven window resize is letterboxed proportionally.

`PulpAudioUnit::supportedViewConfigurations:` accepts configurations
within ~5% aspect tolerance of the design (a JUCE forum thread
documents that Logic 10.6.1 probes 1024x768 / 1366x1024 — accepting
at least one fixes a known Logic reopen-size bug). Falls back to
accepting all configurations if none match the aspect floor.

Padding at the top/bottom (or left/right) of the editor in a host
window is the **expected letterbox** when the host gives us a window
whose aspect ratio doesn't match the design's. Tighten by either
adjusting the plugin's `view_size()` hint or implementing a tighter
view-configuration policy.

### The `PULP_AUV3_PLUGIN()` macro replaces hardcoded force_link

Pre-Phase 3.5, `au_entry.mm` called `pulp_gain_force_link()` to force
the linker to retain pulp-gain-specific static initializers. This
broke AU v3 for every plugin OTHER than pulp-gain. Phase 3.5 ships
`<pulp/format/au_v3_entry.hpp>` with `PULP_AUV3_PLUGIN(factory_fn)`
— place it in ONE `.cpp` per plugin (convention: `au_v3_entry.cpp`
in the plugin's source dir). The CMake helper auto-discovers and
links it into the framework. Mirrors `PULP_CLAP_PLUGIN` and
`PULP_AU_INSTRUMENT`.

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

### Factory entry point: use `PULP_AUV3_PLUGIN()`, NOT a hand-rolled force_link

**Pre-Phase 3.5 (removed):** `au_entry.mm` called `pulp_gain_force_link()`
to force-retain pulp-gain's `au_register.cpp` static initializers.
That symbol was hardcoded to pulp-gain and broke AU v3 for every
other plugin.

**Phase 3.5+:** every plugin includes a per-plugin `au_v3_entry.cpp`
in its source dir with:

```cpp
#include "my_plugin.hpp"
#include <pulp/format/au_v3_entry.hpp>
PULP_AUV3_PLUGIN(my_namespace::create_my_plugin)
```

`PulpAuv3.cmake` auto-discovers this file (by path convention) and
links it into the AU v3 framework (macOS) or .appex (iOS). The
macro expands to `PULP_REGISTER_PLUGIN`, which puts a static
initializer in the TU; the linker keeps the file because CMake's
OBJECT lib + framework SHARED lib both reference its symbols.
Mirrors `PULP_CLAP_PLUGIN` and `PULP_AU_INSTRUMENT`.

### Channel count hard limit of 8

`kMaxChannels = 8`. Bumping that requires re-sizing every
pre-allocated buffer array and validating hosts don't ask for more
channels than the descriptor declares. Not a surround-readiness flag
yet.

### AU v3 native view plumbing (Phase 3.5)

AU v3 uses `requestViewControllerWithCompletionHandler:` to fetch an
`AUViewController`. macOS uses `PulpAUMacViewController` (in the
framework, in macOS AU v3); iOS uses `PulpAUViewController` (in the
monolithic .appex). Both implement `AUAudioUnitFactory` so the same
class is both the factory and the view-providing controller — Apple's
recommended pattern.

`au_v2_cocoa_view.mm` (the AU v2 Cocoa view path) remains the editor
mechanism for the AU v2 `.component` bundle. AU v3 has its own,
parallel view path via the principal class.

Cross-platform editor wiring (ViewBridge, PluginViewHost, design
viewport, GPU host selection) is shared between both AU v3
controllers — see the `view-bridge` skill.

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
`_fallbackView` (View), then `_viewHost` (PluginViewHost). When `[super
dealloc]` runs, the runtime destroys C++-typed ivars in REVERSE
declaration order: `_viewHost`, `_fallbackView`, `_bridge`. That
ordering is load-bearing:

1. `~PluginViewHost` runs FIRST. It calls
   `root_.set_plugin_view_host(nullptr)` (and `set_frame_clock(nullptr)`
   on the GPU host). `root_` references either `_bridge->view_` OR
   `_fallbackView` — BOTH are still alive at this point, so clearing
   the back-pointers is safe on either path.
2. `~View` (`_fallbackView`) runs next — no-op on the bridge-success
   path; on the fallback path the back-pointer was cleared in step 1.
3. `~ViewBridge` runs last. Its destructor calls `close()` →
   `Processor::on_view_closed` → `view_.reset()`. The back-pointer was
   already cleared in step 1.

**`_viewHost` MUST be declared last (destroy first).** The original order
`_bridge, _viewHost, _fallbackView` destroyed `_fallbackView` before
`_viewHost`; on the no-`audioUnit` preview path `_fallbackView` *is* the
View `_viewHost->root_` references, so the host cleared a back-pointer into
a freed View (GPU-plugin-view-host work, 2026-05; Codex P1).

Calling `_bridge->close()` HERE explicitly (before `[super dealloc]`)
reverses that order: the View dies first, then `~PluginViewHost`
dereferences a dangling `root_` reference and crashes AUv3 editor
close. An earlier variant explicitly closed the bridge here and shipped
that crash; the fix is to remove the explicit close, NOT to add it.

The AUv3 editor now also auto-selects the GPU host via the shared
`decide_gpu_host()` helper (Options overload) — see the `view-bridge`
skill's "GPU view host auto-selection" section.

### Headless automation must not create fallback AUv3 UI

When `PULP_DISABLE_PLUGIN_EDITOR`, `PULP_HEADLESS`, `PULP_TEST_MODE`,
or `CI` is set, `PulpAUViewController` returns after setting its basic
view state and does not build `ViewBridge`, `PluginViewHost`, or the
fallback empty view. The fallback is only for preview/no-audioUnit cases;
do not use it to satisfy a test/CI launch because it still creates a
native host surface.

## Validation recipes

Build and validate via the Pulp CLI:

```bash
./build/pulp build
./build/pulp validate         # runs auval via the auval-<name> CTest target
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
