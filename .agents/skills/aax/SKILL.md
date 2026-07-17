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
- Current scope: AAX Native
- Out of scope: bundling Avid assets, DSP/AudioSuite support, PACE release automation


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

The thin SDK glue itself is covered by `test/test_aax_midi_node.cpp` — an
SDK-gated runtime test (built only when `PULP_HAS_AAX`) that drives
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
