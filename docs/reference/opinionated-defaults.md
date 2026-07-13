# Opinionated Defaults

Pulp makes deliberate choices so the common path is the right path. This page
collects those defaults in one place — what they are, why, and how to change
them when your project genuinely needs the other option. None of these are
accidents of history; each is a considered default with an escape hatch.

## Plugin packaging

**Default: one plugin per binary (`single`).**

`pulp_add_plugin` builds one plugin per format binary. A suite that wants many
plugins in one binary opts into a **bundle** via `pulp_add_plugin_bundle`
(CLAP/VST3 today). The centralized knob records intent:

```cmake
set(PULP_PLUGIN_PACKAGING single)   # or: bundle
```

Both models are fully supported regardless of the knob — the choice is which
helper a target calls. See the [plugin-bundles guide](../guides/plugin-bundles.md)
and [`pulp_add_plugin_bundle`](cmake.md#pulp_add_plugin_bundle).

*Why:* most plugins ship alone; one-per-binary keeps scanning, signing, and
mental models simple. Bundling is a real need for suites but shouldn't be forced
on everyone.

## Build type

**Default: `Release`.**

`pulp build` pins `-DCMAKE_BUILD_TYPE=Release` and refuses to silently flip. A
Debug build of a JS-scripted GPU UI is dramatically slower (no `-O3`, no
`NDEBUG`, live asserts, no inlining of canvas/Skia/Yoga/QuickJS) — slow enough
that a perceived UX regression in Debug is almost always the build type, not the
code. Flip to Debug only to step in a debugger, capture fresh traces, or run a
sanitizer; restore Release immediately after.

## Build parallelism

**Default: bounded.** Every build command Pulp emits carries an explicit job
count; a bare `--parallel` / unbounded `-j` is rejected repo-wide. The CLI
bounds parallelism to `min(cores, RAM_budget / 1.5 GiB)` so a build can never
oversubscribe a shared machine. Override the RAM axis with
`PULP_BUILD_MEM_BUDGET_MB`.

## Layout engine

**Default (and ceiling): Flexbox + Grid only.** The layout engine is Yoga, so
the primitives are exactly CSS Flexbox + CSS Grid. Block flow, table layout,
multi-column, floats, and print pagination are out of scope by design — for
React Native parity, a single-pass GPU render pipeline, and alignment with
modern design tools that output flex/grid. See
[layout-model](layout-model.md).

## Processing model

**Default: `Processor`.** Author DSP as one `Processor` per plugin, composing
internal stages from `pulp::signal::*`. Reach for `SignalGraph` only when the
routing itself is runtime-dynamic (hosting external plugins, a user-editable
rack, a saved `.pulpgraph`). Polyphony, sidechain, oversampling, and parallel
internal chains are all `Processor` concerns. See
[processing-models](processing-models.md).

## Utility layer

**Default: CHOC first.** Before hand-rolling a common C++ utility (JSON, MIDI
messages, audio-file I/O, lock-free FIFOs, string helpers, a JS engine), use the
vendored [CHOC](https://github.com/Tracktion/choc) equivalent. Hand-roll only
when CHOC doesn't cover the need or performance demands a specialized
implementation (e.g. SIMD FFT).

## Text shaping

**Default: ON when GPU is enabled** (`PULP_TEXT_SHAPING`). SkParagraph
(HarfBuzz/ICU via Skia) does measure-once / reflow-forever layout. Falls back to
character-width estimation when Skia is unavailable.

## Design-import subsystem

**Default: ON for dev/examples, OFF for release/ship**
(`PULP_ENABLE_DESIGN_IMPORT`). Release builds strip the importer/codegen cluster
for a smaller binary and attack surface; the runtime W3C token pair stays
compiled regardless.

## Licensing

**Default: MIT-compatible only.** Allowed for distributed code: MIT, BSD
(2/3-clause), Apache 2.0, ISC, zlib, BSL-1.0, public domain. Not allowed: GPL /
LGPL / AGPL / SSPL / proprietary / any copyleft (MPL-2.0 is case-by-case).
Vendor SDKs like AAX/ASIO are an opt-in, developer-supplied, never-committed
carve-out. Every dependency is tracked in `DEPENDENCIES.md` / `NOTICE.md`.

## Shipping

**Default: `shipyard pr`.** One command runs the version-bump and skill-sync
gates, pushes, opens the PR, validates across platforms, and merges on green.
Direct `gh pr create` is an emergency bypass only.
