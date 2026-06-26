# ARA Integration

ARA (Audio Random Access) is the standard for time-linked, clip-aware plugin
workflows — pitch correction (Melodyne), spectral editing (SpectraLayers),
stem-aware processors, and any plugin that needs to see more of the session
than the current block of audio.

Pulp's ARA support is being built out under production-readiness workstream 06.
This page documents what ships today, how to enable what's available, and what
to expect as the workstream lands.

## Current state

- `core/format/include/pulp/format/ara.hpp` defines the plugin-side interface
  (`AraDocumentController`, `AraRole`, `host_supports_ara()`).
- `core/format/src/ara.cpp` keeps non-SDK builds conservative:
  `host_supports_ara()` returns false and `ara_companion_factory_for()` returns
  `nullptr` when `PULP_HAS_ARA` is absent.
- `core/format/src/ara_factory.cpp` builds an ABI-valid ARA factory when
  `PULP_ENABLE_ARA=ON` and `PULP_ARA_SDK_DIR` points at a valid SDK, but its
  document-controller callbacks are scaffold no-ops today.
- Format adapter hooks exist for CLAP (`kClapAraFactoryExtension`), AUv3
  (`audioUnitARAFactory`), and VST3 (`kVst3AraFactoryContextKey`). The CLAP
  path is the exercised smoke-test path used by `examples/ara-pitch-tracker/`.
- Status in `docs/status/support-matrix.yaml` → `plugin_extensions.ara`:
  `experimental`.

Do not build production ARA plugins on Pulp today. The SDK-gated adapter hooks
are present for smoke testing, but real controller behavior, content readers,
playback/edit rendering, and per-plugin factory/controller ownership are still
workstream 06 follow-up.

## Scope for v1 (when it ships)

**Target specification:** ARA 2.x. ARA 1.x is effectively obsolete, and every
current DAW (Logic, Cubase, Studio One, Reaper via extension, Presonus) speaks
2.x. Pulp does not plan to implement 1.x backward compatibility.

**In v1 scope:**

- Roles: Playback Renderer, Edit Renderer, Editor View.
- Document controller with audio source, audio modification, musical context,
  region sequence, and playback region abstractions.
- Companion-factory integration across VST3, AU, and CLAP.
- Content reader types for notes, tempo, tuning, and key signature.

**Out of scope for v1:**

- ARA 1.x compat.
- Additional content reader types (chords, bar signatures) beyond the core four.
- Pulp-as-ARA-host. Building a host that consumes ARA plugins is a separate,
  later workstream; v1 is Pulp-as-ARA-plugin.

## SDK acquisition

Celemony's ARA SDK is free (requires Celemony developer registration) under a
permissive license that allows redistributing the plugins that use it. The
SDK itself is not bundled with Pulp — it is developer-supplied, the same way
the AAX SDK and AudioUnitSDK are.

Expected install, once the build gate lands:

```bash
# Mirror URL; Celemony may also publish tags under different names.
git clone --depth 1 --branch releases/ARA_SDK_2.2.0 \
    https://github.com/Celemony/ARA_SDK ~/SDKs/celemony/ara-sdk/current
```

```bash
export PULP_ARA_SDK_DIR=~/SDKs/celemony/ara-sdk/current
cmake -S . -B build -DPULP_ENABLE_ARA=ON
```

`PULP_ENABLE_ARA` defaults to `OFF`. When `ON`, Pulp's CMake validates that
`PULP_ARA_SDK_DIR` points at a checkout containing the expected header
directory before compiling any ARA-dependent translation unit.

## What to expect as the workstream lands

Work merges in small, named slices on `develop/prod-readiness` feature
branches. The progression:

1. Scaffolding — SDK gate, build flag, dependency/license docs, and the
   plugin-side `AraDocumentController` interface.
2. Document controller — concrete `AraDocumentController` implementation with
   source, modification, context, region, and region-sequence management.
3. Format adapters — companion-factory hooks now exist for VST3, AUv3, and
   CLAP; remaining work is live host validation and per-plugin controller
   ownership.
4. Roles — Playback Renderer, then Editor Renderer, then Editor View.
5. Content readers — notes, tempo, tuning, key.
6. Validation — Logic + Cubase + Studio One smoke against `PulpGain` and a
   small ARA-aware example plugin such as `examples/ara-pitch-tracker`.

Each slice is tracked in `planning/production-readiness/06-ara.md` (private
submodule) and lands via PR with a `Workstream 06 slice <N.M>` title.

## Legal / license posture

- ARA SDK: Celemony license; registration required; redistribution of
  compiled plugins permitted.
- Pulp: MIT. No bundling of ARA SDK source or binaries in the Pulp repo.
- `DEPENDENCIES.md` records the ARA SDK as developer-supplied and out-of-tree.

## Related

- Current scaffold: `core/format/include/pulp/format/ara.hpp`,
  `core/format/src/ara.cpp`, `core/format/src/ara_factory.cpp`.
- Workstream spec: `planning/production-readiness/06-ara.md`.
- Matrix entry: `docs/status/support-matrix.yaml` →
  `plugin_extensions.ara`.
- Format-adapter limitations: `docs/reference/capabilities.md` →
  "Known Limitations (plugin formats)".
