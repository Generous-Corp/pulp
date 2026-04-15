---
name: ara
requires:
  scripts:
    - tools/deps/audit.py
  tools: []
---

# ARA Skill

Use this when working on Pulp's optional ARA (Audio Random Access) support or
when guiding users who want to build and validate ARA-enabled plugins.

## Scope

- Companion APIs: VST3 on macOS + Windows, AU on macOS, CLAP on macOS + Windows
- Primary hosts: Logic (AU), Cubase + Studio One (VST3), Reaper (VST3 or CLAP
  with ARA extension), Bitwig (CLAP when its ARA extension ships)
- Target ARA version: **2.x** (enum value `kARAAPIGeneration_2_3_Final` = 6)
- Out of scope: ARA 1.x (obsolete), hosting arbitrary ARA plugins (that's a
  separate host workstream), audio-file chunk authoring tools

## Licensing

- ARA SDK is **Apache 2.0** — compatible with Pulp's MIT, but we still treat
  it like AAX / AudioUnitSDK / VST3 SDK: developer-supplied, opt-in,
  out-of-tree, never committed. `tools/deps/audit.py --strict` must stay
  green.
- `DEPENDENCIES.md` lists ARA SDK under **Optional Developer-Supplied Vendor
  SDKs** exactly because of the companion-API carve-out in CLAUDE.md.
- Celemony notes: "Any public release of ARA-enabled software should be based
  on a tagged public release of the ARA API." Use a tag, not `main`.

## What Users Should Download

The SDK is public on GitHub. Clone with submodules so ARA_API + ARA_Library +
ARA_Examples all resolve:

```bash
git clone --recurse-submodules --depth 1 \
    https://github.com/Celemony/ARA_SDK.git \
    external/ara-sdk
```

After cloning, verify:

```bash
ls external/ara-sdk/ARA_API/ARAInterface.h     # core C API
ls external/ara-sdk/ARA_Library/PlugIn/ARAPlug.h  # C++11 helpers
```

**Never commit `external/ara-sdk/`.** The `external/` prefix keeps it out of
`cmake --install` exports and the repo audit.

## Enabling ARA in a Pulp build

Two CMake knobs:

```bash
cmake -S . -B build \
    -DPULP_ENABLE_ARA=ON \
    -DPULP_ARA_SDK_DIR=/absolute/path/to/external/ara-sdk
```

`PULP_ENABLE_ARA=ON` without `PULP_ARA_SDK_DIR` fails fast with a clear
error. A missing / wrong path fails on `ARAInterface.h` existence check
before any compilation starts.

When enabled, `pulp-format` is built with `PULP_HAS_ARA=1` and the ARA SDK
include directory added to its PUBLIC include path. Plugin TUs that want
to implement ARA include `<ARA_API/ARAInterface.h>` directly.

## Verifying the integration

Quick smoke:

```bash
cmake --build build --target pulp-test-ara
./build/test/pulp-test-ara
# Expect: ara_sdk_compiled_in() → true, ara_sdk_generation() >= 6
```

A build with `PULP_ENABLE_ARA=OFF` (the default) must still pass the same
tests — `ara_sdk_compiled_in()` returns false and `ara_sdk_generation()`
returns 0 without failing.

## Writing an ARA-aware plugin

1. Override `Processor::create_ara_document_controller()` to return a
   concrete `AraDocumentController` subclass.
2. Use the Pulp-side types from `<pulp/format/ara/types.hpp>`
   (`AraAudioSource`, `AraPlaybackRegion`, etc.). These are
   SDK-independent so the plugin TU never pulls the SDK unless it wants
   to implement a content reader that inspects ARA's C structs directly.
3. Format-adapter companion factories translate between Pulp types and
   the SDK's C structs (landing in workstream 06 slices 6.3/6.4/6.5).

## Known gotchas

- **All time fields are seconds (double)**, not samples. Pulp's
  `AraPlaybackRegion` matches `ARAPlaybackRegionProperties`; adapters
  convert to samples only at explicit sample-domain APIs.
- **Threading contract**: ARA's render thread, document-editing thread,
  and main thread map to Pulp's audio / UI / main threads. Do not block
  across them; consult `docs/guides/sync-strategy.md`.
- **Host quirks are real**. Logic's ARA integration differs from
  Cubase's. Budget time for host-specific workarounds.
- `host_supports_ara()` currently returns `false` even with the SDK
  compiled in — adapter companion factories (6.3–6.5) flip that.

## Planning & issue tracking

- Spec: `planning/production-readiness/06-ara.md`
- Status: `planning/production-readiness/STATUS.md` (workstream 06 section)
- Tracking: [#219](https://github.com/danielraffel/pulp/issues/219)

## Skill maintenance

Path map: `tools/scripts/skill_path_map.json`. Add entries for
`core/format/src/ara*`, `core/format/include/pulp/format/ara*`,
`external/ara-sdk` (so non-committed work still triggers skill sync
checks on related source changes).
