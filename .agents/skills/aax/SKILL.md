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
cmake --build build --target MyPlugin_AAX -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
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

This matches the shape used for CLAP/VST3/AU/CoreMIDI/ALSA sysex (#239).
See `core/format/src/aax_runtime.cpp::decode_midi_node` for the canonical
implementation landed in #408.

When adding or changing any AAX MIDI input path, exercise this against a
multi-packet sysex vector (at least one packet across the 4-byte boundary
and one terminator-only packet) in a unit test. Shipping without the test
is the #290 "tests ship with fixes" rule.

## Review Checklist

After any AAX-related change:

1. Build with AAX disabled and confirm the repo still works normally.
2. Build with `PULP_ENABLE_AAX=ON` against a developer-supplied SDK.
3. Run `pulp validate` and `pulp validate --all` when the validator is installed.
5. Recheck the user-facing guidance in `docs/guides/aax.md` if behavior changed.
