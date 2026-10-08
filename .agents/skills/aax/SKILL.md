---
name: aax
description: Optional AAX support for Pulp, including developer-supplied Avid SDK setup, CMake enablement, DigiShell/AAX Validator workflows, and local AAX builds on macOS or Windows.
requires:
  scripts:
    - tools/audit.py
    - tools/deps/audit.py
  tools: []
---

# AAX Skill

Use this when working on Pulp's optional AAX support or when guiding users who
want to build and validate AAX plugins locally.

## Scope

- Supported hosts: macOS and Windows
- Unsupported: Linux and Ubuntu
- Current scope: AAX Native, including the custom editor (`AAX_CEffectGUI`) and
  parameter gestures
- Out of scope: bundling Avid assets, DSP support, PACE release automation
- Out of scope, with a caveat worth knowing: **AudioSuite**. The descriptor
  declares `AAX_ePlugInRole_InsertOrAudioSuite`, so the role is advertised, but
  Pulp registers no `AAX_IHostProcessor` — the dedicated offline-render path is
  not implemented. Say "the role is declared; the offline path is not
  implemented", not "AudioSuite works" and not "the role is absent".

## The custom editor

`core/format/src/aax_effect_gui.cpp` embeds a Pulp editor in the Pro Tools
plugin window through the shared, format-agnostic `view::PluginViewHost` — the
same seam VST3 / AU v2 / AU v3 / CLAP use, so there is no AAX-specific render
path to maintain. Its `ViewBridge` is built from
`ViewBridge::Options::hosted_editor()` like every other hosted adapter — never a
hand-assembled `Options`, and a structural test enforces that. See the
`view-bridge` skill.

**The editor is opt-in, and defaulting it off is deliberate — do not "fix" it.**
`PULP_AAX_PLUGIN` registers no editor, so a plugin gets Pro Tools'
auto-generated parameter strip; `PULP_AAX_PLUGIN_WITH_GUI` registers the custom
editor. The strip is plain but it works, and it is the only AAX UI any Pulp
plugin has shipped with. The custom editor has not been validated in Pro Tools
itself, so a plugin that merely rebuilds must not trade a working strip for an
unproven editor — an author has to ask for it. `pulp-test-aax-entry-registration`
asserts both directions; if you flip the default, that suite fails, and that is
the point. Reach for `PULP_AAX_PLUGIN_WITH_GUI` in an example or template only
once the editor has actually been driven in Pro Tools.

Gotchas that are specific to AAX and cost time if you rediscover them:

- **The proc pointer is the whole game.** A custom UI appears only because
  `get_effect_descriptions()` registers `kAAX_ProcPtrID_Create_EffectGUI`
  alongside `kAAX_ProcPtrID_Create_EffectParameters`. Drop that one
  `AddProcPtr` and Pro Tools silently shows its auto-generated parameter strip
  no matter how good `create_view()` is. The validator's "does not contain
  EffectGUI" warning is the tell. That fallback is exactly why registering the
  proc conditionally is a safe default rather than a broken plugin.
- **The editor has its own `Processor`, deliberately.** AAX splits the
  host-side data model from the real-time algorithm; the algorithm's
  `Processor` lives in its private data block and the model cannot reach it. So
  `EffectParameters` builds a *second* model-side `Processor` for
  `create_view()`. This is the exact opposite of AU v2, where a second
  `Processor` was a real bug (parameters drifted). The reason it is safe here:
  the AAX parameter manager — not either `Processor` — is the value authority,
  and the model mirrors it into the editor store both ways
  (`UpdateParameterNormalizedValue` in, `AAX_IParameter::SetValueWithFloat`
  out), via `ParameterMirror` in `aax_editor.hpp`.
- **The mirror stops the echo by comparing values, not with a re-entrancy
  flag.** `ParameterMirror` writes back only when a write would actually change
  something. A transient "I am syncing" bool around the inbound write looks
  equivalent and is not: it encodes *when* the listener runs, and that is not
  guaranteed. `StateStore` runs a `ListenerThread::Main` listener inline only
  while no `EventLoop` is installed (`set_main_loop()` is never called on this
  store today) — install one and the flag is long cleared before the deferred
  listener fires. The rule is a statement about the two values, so it holds
  wherever the callback lands. It is load-bearing, not an optimization:
  `StateStore::set_value()` notifies unconditionally, even when nothing changed,
  so the echo has no fixed point of its own and would re-enter the host at its
  dispatch rate forever.
- **Never compare the store's value against the parameter's raw real value.**
  They are not in the same coordinate system, and `==` between them does not
  terminate. Two lossy transforms sit on the two sides: AAX's value has been
  through the taper (`NormalizedToReal` of whatever the host sent, snapping to
  `ParamRange::step` only when a step is set), while the store's has been
  through `constrain_stored_value`, which applies an **implicit step of 1 to any
  non-`Continuous` `ParamKind` even when the range declares no step**. For a
  discrete parameter with no explicit step the two are therefore *structurally
  different values*, `==` is never true, and — because `SetValueWithFloat` does
  not store what it is handed — the corrective write never reproduces them
  either. That is an unbounded automation write stream reaching Pro Tools.
  `ParameterMirror` instead asks two questions, both required:
  *do the two sides normalize to the same value* (`taper_real_to_normalized` —
  the normalized token is the only thing that actually travels to the host), and
  *would the write land AAX where it already is* (`aax_value_after_write`, for
  ranges whose real and normalized grids do not line up — a discrete kind over a
  wide skewed range is the case that bites). A per-parameter fuse
  (`kCorrectionFuseLimit`) bounds any remaining host quirk and logs it rather
  than wedging the host. Covered by `pulp-test-aax-editor` (SDK-free), whose
  sweep drives 21 ranges × 4 kinds × 201 start points through the real taper and
  requires every case to settle in one write with the fuse never firing.
- **`AAX_IParameter::SetValueWithFloat` does not store the value.** It posts a
  request through the automation delegate (`AAX_CParameter::SetValue` does
  `Touch(); PostSetValueRequest(Identifier(), RealToNormalized(newValue));
  Release();`), so `GetValueAsFloat` keeps returning the old value until the host
  answers with `UpdateParameterNormalizedValue` — the same path a fader or
  control surface takes. Code that assumes a synchronous write will misread the
  parameter. What comes back is a *normalized* value, stored as
  `NormalizedToReal` of it, so AAX ends up holding
  `denormalize(normalize(constrained))` and **not** the value that was written.
- **`AAX_IParameter::GetValueAsFloat` can fail without writing its out-param.**
  It answers `false` and leaves the float untouched for any parameter whose value
  type is not `float` (`SetValueWithFloat` refuses the same way). Pulp registers
  only `AAX_CParameter<float>` today, so it always succeeds — but ignoring the
  return means the day that changes, a caller reports success with a fabricated
  zero. Propagate the bool.
- **A test double for the parameter manager must round-trip the taper.** The
  manager's asynchrony is the *easy* half to model; the value transform is the
  half that hides bugs. A fake that echoes the real value back verbatim, or that
  pushes it with `set_value` where the adapter uses `set_normalized`, drops both
  lossy transforms and can only ever confirm whatever rule the mirror already
  implements — it cannot fail on a mirror-convergence bug by construction.
- **Update values only through `AAX_IParameter::SetValue*`.** Avid's own header
  is explicit that a GUI must never call `UpdateParameterNormalizedValue`
  directly; `SetValue*` manages the automation locks and posts coefficients.
- **Gestures are not optional.** Without `TouchParameter` / `ReleaseParameter`
  a custom UI records every edit as an isolated automation point instead of a
  stroke — worse than shipping no UI. `state::Binding` gestures route through
  `GestureRouter` (`aax_editor.hpp`), which enforces AAX's balance invariant.
- **`AAX_Point`'s constructor is `(vert, horz)` — vertical first.** Passing
  width first silently transposes the editor.
- **Sizing is plugin-driven.** AAX reads a size from `GetViewSize()`; the
  plugin pushes later changes through `AAX_IViewContainer::SetViewSize`. Follow
  the AU v2 model (forward native size changes), not VST3's.
- **Never defer to `AAX_CEffectGUI::GetViewSize()` / `GetMinimumViewSize()`.**
  Both base implementations `return AAX_SUCCESS` *without writing the point*, so
  the host reads back whatever it passed in and the plug-in appears to have
  agreed to it. There is no "let the base class answer" fallback here. AAX may
  ask for a size before any window exists, but the plug-in's hints are always
  reachable: `GetEffectParameters()` yields the data model, and
  `EditorHost::editor_view_size()` returns `Processor::view_size()`, which needs
  no view tree. Answer from that, and report `AAX_ERROR_NULL_OBJECT` when the
  plug-in genuinely has no editor.
- **The editor model's member declaration order is load-bearing.**
  `EditorModel` in `aax_runtime.cpp` declares `store` before `processor` because
  members die in reverse declaration order and `Processor::state()` dereferences
  a pointer to that store — a Processor may touch it from its destructor or from
  a worker thread that destructor joins. The router is declared before the
  Processor too: a parameter write from `~Processor` routes through the store's
  gesture callbacks. `static_assert(offsetof(...))` guards both, so a reorder
  fails the build rather than the teardown. Every other adapter
  (`vst3_adapter.hpp`, `standalone.hpp`, `au_v2_instrument.hpp`, `headless.hpp`)
  carries the same store-before-Processor convention.
- **The editor needs Skia.** Without `PULP_HAS_SKIA` the Windows
  `PluginViewHost` falls back to the no-op stub factory, `create()` returns
  null, and the plugin loads with no editor.

The SDK-free logic (gesture routing, sizing) lives in
`core/format/include/pulp/format/aax_editor.hpp` on purpose, so it is testable
with no Avid SDK: `pulp-test-aax-editor` runs everywhere.

**"SDK-gated" currently means "never built," not "built elsewhere."** No Avid
SDK is present in Pulp's CI or on any Pulp development machine, so
`pulp-test-aax-effect-gui`, `pulp-test-aax-midi-node`, and
`pulp-test-aax-entry-registration` are configured out of every lane that runs.
Do not cite them as coverage for a claim: the AAX behavior actually verified on
every push is the SDK-free part. If you have an Avid SDK locally, building these
suites is the only way they have ever run — and worth doing before you assert
anything about the SDK glue.


- Never commit the AAX SDK, DigiShell, validator binaries, or Avid example code.
- Never unpack Avid downloads inside the Pulp repo.
- Keep AAX developer-supplied, opt-in, and out-of-tree.
- Run the repo audits after AAX-related changes:

```bash
python3 tools/deps/audit.py --strict
```

## What Users Should Download

Tell users to sign in at:

```text
https://developer.avid.com/aax/
```

Required downloads:

- `AAX SDK`
- `DigiShell and AAX Validator`

Optional later:

- `AAX Plug-In Page Table Editor`

Do not recommend these for normal Pulp AAX setup unless the task explicitly
needs them:

- `AAX Developer Tools` beta bundles
- Pro Tools installers
- HD Driver
- Avid Cloud Client Services

## Suggested Install Locations

Preferred user-local locations so Pulp can auto-discover them:

```text
~/SDKs/avid/aax-sdk/current
~/SDKs/avid/aax-validator/current
%USERPROFILE%\SDKs\avid\aax-sdk\current
%USERPROFILE%\SDKs\avid\aax-validator\current
```

`current/` must **be** the SDK/validator root, not contain a nested wrapper.
The Avid archives unzip to a versioned dir (e.g. `aax-sdk-2-9-0/`,
`aax-validator-dsh-2024-6-0-…-mac-arm64/`), so a common mistake is leaving
`current/aax-sdk-2-9-0/Interfaces/...`. Move the contents up (or symlink
`current` → the versioned dir) so `current/Interfaces/AAX.h` and
`current/CommandLineTools` resolve directly. `pulp doctor` confirms discovery.
The user-facing worked example lives in `docs/guides/aax.md`.

Environment variables override auto-discovery:

```bash
export PULP_AAX_SDK_DIR=~/SDKs/avid/aax-sdk/current
export PULP_AAX_VALIDATOR_DIR=~/SDKs/avid/aax-validator/current
```

## macOS PACE and private tool backup

The Avid SDK and DigiShell validator are developer-supplied materials. Keep
them outside every public repository, preferably at:

```text
~/SDKs/avid/aax-sdk/current
~/SDKs/avid/aax-validator/current
```

Keep PACE Fusion installers and a small hash manifest in a private directory,
for example:

```text
~/SDKs/private/pace/fusion/6.0.1/
```

The manifest should record the source files, Fusion version, date, and SHA-256
hashes. Do not put iLok credentials, developer certificates, wrap configs, or
license receipts in Git, shell history, command arguments, or build logs.

The macOS Fusion package installs several components. The `Fusion_tools_Lite`
component provides `wraptool`, normally at:

```text
/Applications/PACEAntiPiracy/Eden/Fusion/Versions/6/bin/wraptool
```

The full installer has a brittle preflight that rejects any process whose name
contains `xcodebuild`. Confirm that no real `xcodebuild` compilation is active
before installing. Persistent `xcodebuildmcp` helpers can trigger the same
message even when no build is running; do not kill shared helpers merely to
satisfy that check. If needed, install the signed component packages from the
mounted PACE installer individually, preserving the parent DMG/pkg hash and
recording the component versions. Existing iLok/license support may already be
installed; verify receipts with `pkgutil --pkg-info` before upgrading.

After installation, verify the expected macOS receipts and tool path without
printing account data:

```bash
pkgutil --pkgs | grep -E 'com\.paceap\.pkg\.eden\.(licensed|activationexperience|fusion\.tools\.lite\.6|iLokLicenseManager)'
ls -l /Applications/PACEAntiPiracy/Eden/Fusion/Versions/6/bin/wraptool
/Applications/PACEAntiPiracy/Eden/Fusion/Versions/6/bin/wraptool list
```

Keep the original installer DMG/PKG and a SHA-256 manifest in the private
backup. A useful restore record includes the Fusion version, package names,
installation date, and hashes; it does not include iLok credentials, customer
numbers, certificates, or wrap passwords.

PACE installation does not sign an AAX binary automatically. Treat these as
separate gates:

1. AAX compile/link against the out-of-tree Avid SDK.
2. Apple/PACE signing with the developer's own identities and wrap
   configuration.
3. DigiShell/AAX Validator checks.
4. Discovery and editor/audio smoke in Pro Tools.

An ad-hoc or linker-signed bundle can pass data-model and parameter validation
while still failing `wraptool verify` or Pro Tools's production signature
requirements. Never claim the signing or Pro Tools gates from a successful
compile alone. Windows setup and signing remain a separate, deferred path.

## Poka-yoke signing and release gates

Make the safe path the easy path and make an unsafe path fail before it can
produce a misleading artifact. These checks are deliberately redundant:

1. **Keep trust domains separate.** A connected, certified iLok and a successful
   `wraptool list` prove that the local PACE license can be used. They do not
   prove that a bundle was signed. Require a fresh `wraptool verify` on the
   exact final bundle before calling it signed.
2. **Keep secrets out of process arguments.** Never put an iLok password,
   customer number, wrap password, certificate private key, or receipt JSON in
   chat, shell history, an environment capture, a repository, or an argv
   string. Prefer the interactive PACE prompt or a permissions-restricted
   file-backed configuration owned by the developer. If a secret is ever
   pasted into a transcript, rotate it before continuing.
3. **Refuse unsigned-looking inputs.** Sign only a freshly built bundle from a
   known build directory. Record its SHA-256 before signing and record the
   post-signing SHA-256 separately; never overwrite the only unsigned copy.
4. **Seal the bundle shape before Apple signing.** Arbitrary JSON evidence files
   under `Contents/MacOS` are treated as nested code by `codesign`. Relocate
   Pulp's `*.inspector-capabilities.json`, `*.control-shipping.json`, and
   `*.control-shipping-report.json` sidecars to `Contents/Resources` before
   signing, then fail if any of those files remain under `Contents/MacOS`.
5. **Use ordered gates.** The only valid order is: build, inspect bundle shape,
   Apple/PACE sign, `wraptool verify`, `codesign --verify --deep --strict`,
   AAX Validator, install the exact verified bundle, then prove discovery and
   load in Pro Tools. A passing validator or a certified iLok cannot substitute
   for a later gate.
6. **Use negative controls when changing the workflow.** A deliberate unsigned
   copy must fail `wraptool verify`; a deliberate bundle with a sidecar left in
   `Contents/MacOS` must fail the Apple verification step. Keep these checks
   disposable and outside public repositories.
7. **Make installation identity-based.** Install only from the verified
   artifact, copy to the system and user AAX plug-in locations only when needed
   for the host test, and compare the installed bundle hash to the verified
   source hash. Remove stale copies before retrying discovery so a host cannot
   load an older build by accident.
8. **Report evidence, not intent.** A release handoff must include the exact
   source commit, artifact path, artifact SHA-256, signing and validator
   receipts, install locations, and host result. If Pro Tools was not launched,
   say that the host gate is pending.

For a local macOS run, the minimum preflight should be equivalent to:

```bash
set -euo pipefail
test -x /Applications/PACEAntiPiracy/Eden/Fusion/Versions/6/bin/wraptool
test -d "$AAX_BUNDLE"
! find "$AAX_BUNDLE/Contents/MacOS" -maxdepth 1 -type f \
    \( -name '*.json' -o -name '*.inspector-capabilities.json' \
       -o -name '*.control-shipping.json' \
       -o -name '*.control-shipping-report.json' \) -print -quit | grep -q .
shasum -a 256 "$AAX_BUNDLE/Contents/MacOS/$(basename "$AAX_BINARY")"
```

The command is a shape check, not a signing recipe: the actual wrap
configuration and credentials stay in the developer's private PACE setup.
When the PACE account reports **license verified**, record that as the local
authorization precondition and continue through the independent signing and
host gates rather than treating it as completion.

On macOS, `wraptool sign` also needs either a PACE-issued customer number or a
local wrap-configuration file in addition to the Apple Developer ID signing
identity. If neither is available, stop at the compile/validator gates and
report that missing PACE configuration; do not guess identifiers or put a
password in a command line.

### Optional macOS environment management with mise

`mise` can be useful for the non-proprietary parts of this setup: pinning
supported CLI versions, defining repeatable `build`/`validate` tasks, and
selecting environment variables such as `PULP_AAX_SDK_DIR` and
`PULP_AAX_VALIDATOR_DIR`. Keep that configuration in a private machine-setup
repository if it contains local paths. Do not use `mise` to distribute or
manage the Avid SDK, DigiShell, PACE Fusion binaries, iLok state, signing
certificates, customer numbers, wrap configurations, or credentials. Those
remain developer-supplied macOS assets and should be restored from the private
backup with their recorded hashes. Windows setup and signing can adopt the same
boundary later, but is intentionally not specified here.

## Core Commands

Check current AAX availability:

```bash
pulp status
pulp doctor
```

Build with AAX enabled:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DPULP_ENABLE_AAX=ON \
  -DPULP_AAX_SDK_DIR="$PULP_AAX_SDK_DIR"
tools/ci/governed-build.sh cmake --build build --target MyPlugin_AAX
```

Validate built plugins:

```bash
pulp validate
pulp validate --all
```

Notes:

- `pulp validate` uses the faster AAX describe-validation path when the validator is installed.
- `pulp validate --all` runs the broader AAX validator suite.
- Do not launch multiple full AAX validator runs in parallel; DigiShell can collide on local ports.

## Expected UX

- If the AAX SDK is missing, point users to the Avid sign-in page and `PULP_AAX_SDK_DIR`.
- If DigiShell/AAX Validator is missing, point users to the Avid sign-in page and `PULP_AAX_VALIDATOR_DIR`.
- On Linux or Ubuntu, explain that AAX is unsupported and remove `AAX` from `FORMATS`.
- If validation reports that a bundle exists but the plugin binary is missing, build the target before validating it.

## Gotchas

### MIDI sysex accumulator

AAX splits multi-byte sysex (F0 … F7) across sequential `AAX_CMidiPacket`
entries — the first packet carries the F0 status byte, continuation
packets can appear with no status byte, and the final packet carries F7.
Each packet's `mData` field is at most 4 bytes. A single-packet decoder
will silently drop everything after the first 4 sysex bytes, and the
host will never see the real message.

The correct shape is a per-node accumulator:

```cpp
std::vector<uint8_t> sysex_buffer;
bool sysex_in_progress = false;
int32_t sysex_start_offset = 0;
// for each packet in the node's buffer:
//   if byte == 0xF0 → start accumulator, capture start_offset from mTimestamp
//   while sysex_in_progress → append every payload byte (no status-byte requirement)
//   if byte == 0xF7 → flush accumulator as one MidiEvent, reset
```

This matches the shape used for CLAP/VST3/AU/CoreMIDI/ALSA sysex. The state
machine itself is **SDK-free and unit-tested** in
`core/format/include/pulp/format/aax_midi_packets.hpp`
(`decode_midi_packets()` for input, `fragment_sysex()` for output) — because
`aax_runtime.cpp` is gated behind the developer-supplied SDK and is **not
compiled in stock CI**, the only way to test the reassembly/fragmentation is to
keep it out of the `AAX_*`-typed translation unit. `decode_midi_node` /
`encode_midi_node` now live in `core/format/src/aax_midi_node.cpp` (declared in
`core/format/include/pulp/format/aax_midi_node.hpp`, called from
`aax_runtime.cpp`); they just translate `AAX_CMidiPacket` <-> `MidiPacketBytes`
and delegate, so the tested code is the shipping code. When you change either
path, change `aax_midi_packets.hpp` (and its tests in `test/test_aax_midi.cpp`,
which run in default CI), not a copy inside the runtime.

The thin SDK glue itself has a test but no lane that builds it:
`test/test_aax_midi_node.cpp` is SDK-gated (built only when `PULP_HAS_AAX`) and drives
`decode_midi_node` / `encode_midi_node` through real `AAX_IMIDINode` /
`AAX_CMidiStream` / `AAX_CMidiPacket` fakes. Run it on an AAX-SDK machine
(`ctest -R aax-midi-node` after an `-DPULP_ENABLE_AAX=ON` build) to verify the
delegation; stock CI still cannot compile it.
The AAX bypass MIDI-thru helper must copy sidecar payloads with
`MidiBuffer::add_sysex_copy()`; `MidiBuffer::SysexPayload` is deliberately
not a movable raw `std::vector`.

When clearing an AAX process block's MIDI buffers, clear both the short-event
storage and the sysex sidecars. `MidiBuffer::clear()` resets short events only;
call `clear_sysex()` on both input and output buffers before decoding the next
block, or stale sidecar payloads can be re-emitted by a later block.

When adding or changing any AAX MIDI input path, exercise this against a
multi-packet sysex vector (at least one packet across the 4-byte boundary
and one terminator-only packet) in a unit test. Adapter fixes should ship with
the regression tests that prove the fixed behavior.

### Bypass audio must be latency-compensated (PDC alignment)

The AAX bypass short-circuit must NOT `memcpy` the dry input straight to the
output. When the wrapped Processor reports a non-zero latency, the host has
already delay-aligned the plugin's *wet* path by that latency (PDC), so a raw
dry copy arrives `latency` samples early — comb-filtering on parallel/mix
busses. Route the bypass pass-through through
`boundary::render_bypass_passthrough` (in `adapter_boundary.hpp`), sizing the
shared `LatencyCompensatedBypass` delay line to
`aax_reported_latency(definition.latency_samples)` in `ensure_prepared()` —
the same value `SetSignalLatency()` reports to the host. A zero latency
collapses to a straight passthrough. VST3/CLAP/AU v2/v3 use the identical
helper; a shared bit-exact fixture in `test_adapter_boundary_parity.cpp`
(`[bypass]`) covers it since the real AAX runtime can't build without the Avid
SDK.

### Bind a parameter store before asking the processor anything

`build_plugin_definition()` (`aax_model.cpp`) describes the plug-in from a
fresh `factory()` instance during registration — before Pro Tools or the AAX
validator ever prepares it. A plug-in whose `latency_samples()` (or
`descriptor()`) reads a parameter calls `state()`, which dereferences the
processor's store pointer; with no store bound that is a SIGSEGV at
registration, and the host reports the plug-in as failing its describe step
with no Pulp log. Every other adapter binds a store and calls
`define_parameters()` before these reads, so a plug-in that works in
VST3/AU/CLAP crashed only in AAX.

The definition builder now binds a scratch `StateStore` (declared before the
processor, so it outlives it) and runs `define_parameters()` immediately after
`factory()`, then reads `descriptor()` and `latency_samples()`. The registered
latency is therefore the latency at default parameter values. Any new
adapter-side code that instantiates a throwaway processor to read metadata must
do the same; `[aax][model][latency]` in `test_aax_model.cpp` covers it SDK-free
with a processor whose latency is a parameter default.

### The editor opens on the plug-in's own background

Build the host's `PluginViewHost::Options` with
`editor_host_options(bridge, gpu, size)` (`gpu_host_select.hpp`), never field
by field: it carries the plug-in's declared background
(`ViewBridge::editor_background_rgb()`), which the host paints on its backing
layer and under the tree whenever there is no document frame. A
hand-built Options silently drops it and this format opens on the framework
navy while the others open on the plug-in's colour (`view-bridge`, "The first
frame must already look like the plug-in"). After attaching and
`notify_attached()` the GUI calls `ViewBridge::prepare_first_frame(*host_)`, so
the document is mounted and presented before Pro Tools composites the editor
(`view-bridge`, "Editor open"); this path is unverified in Pro Tools itself.

### The editor's GPU surface is a SUBSCRIPTION, not a one-shot read

`aax_effect_gui.cpp` must not sample `host_->gpu_surface()` once and hand
the result to the scripted UI session. That read is only valid on hosts
which build their surface in the constructor; the Windows host — the one
Pro Tools loads — creates its Dawn surface inside
`try_attach_to_parent()`, so a read taken before that call returns
`nullptr` forever and the editor's WebGPU JS silently renders through
mocks.

Use the shared helper, BEFORE the attach:

```cpp
gpu_surface_binding_ = bind_gpu_surface(*host_, bridge_->scripted_ui(),
                                        gpu, "AAX");
if (!host_->try_attach_to_parent(parent)) { gpu_surface_binding_.reset(); ... }
```

Reset the subscription in `teardown()` before `bridge_->close()` — the
observer writes into the session that call destroys. Full contract:
the `view-bridge` skill's "GpuSurface plumbing into WidgetBridge".

### Offline render is visible only for AudioSuite instances

AAX gives an Insert no offline-bounce signal, so a Pro Tools bounce of an insert
still reports `ProcessMode::Realtime`. An AudioSuite instance does render
offline: `GenerateCoefficients()` asks `AAX_IController::GetIsAudioSuite()` and
writes the answer into the **last** parameter-packet slot
(`render_mode_packet_slot()`, after bypass at 0 and the parameters at 1..N), and
the algorithm decodes it with `process_mode_from_packet()`. The packet is
therefore `parameters + 2` floats; a change to the packet layout must keep the
render-mode slot last and update `test_aax_model.cpp`.

## Review Checklist

### Parameter semantics and declared layouts

AAX bindings consume the shared `ParamInfo::kind`, `value_labels`, and
canonical parameter text helpers. Do not infer discreteness from `range.step`
or compute AAX step counts as intervals: `param_value_count()` is the number of
host values. `PluginDescriptor::supported_bus_layouts` expands to one AAX
component per declared configuration, with a distinct derived native ID; AAX
stem-incompatible configurations are rejected by the model instead of silently
advertised. The SDK-free AAX model tests are required coverage for both.

After any AAX-related change:

1. Build with AAX disabled and confirm the repo still works normally.
2. Build with `PULP_ENABLE_AAX=ON` against a developer-supplied SDK.
3. Run `pulp validate` and `pulp validate --all` when the validator is installed.
4. Recheck the user-facing guidance in `docs/guides/aax.md` if behavior changed.

### Tracing attaches for this format now (WAH-4)

Perfetto tracing used to be wired into **VST3 only**. A capture of a Pro Tools/AAX
session recorded nothing while `Tracing`'s API described itself as
process-global — so an empty `.pftrace` looked like an environment problem
rather than a missing call.

This adapter now holds a `runtime::ScopedTracingAttachment` (`InstanceState::tracing`). Two
things follow:

- **It is RAII, not a hand-balanced attach/detach pair.** A leaked attachment
  is not benign: the `.pftrace` is only written by the FINAL detach, so one
  unbalanced instance means the capture silently produces nothing.
- **Declaration order is load-bearing.** It must outlive every span this
  instance can emit, so it is declared to destroy LAST. The final detach also
  cancels and JOINS the auto-flush timer, which is what makes plug-in module
  unload safe — a detached timer thread that wakes after `FreeLibrary` /
  `dlclose` runs freed code.

No-op unless the build is configured `PULP_TRACING=ON`.
