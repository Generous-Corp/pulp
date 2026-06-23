---
name: packages
description: Search, suggest, add, and browse third-party audio packages. Handles "suggest packages", "add a package", "what packages are available", "search for pitch detection", and package browsing.
requires:
  scripts:
    - tools/packages/registry.json
    - tools/packages/validate_registry.py
---

# Package Manager Skill

Help users discover, evaluate, and add third-party audio libraries to their Pulp projects.

## When to Use

- User asks about available packages or libraries
- User needs a capability not in Pulp's built-in signal subsystem
- User wants to add a third-party library
- User asks about pitch detection, ML inference, resampling, etc.
- User says "suggest packages", "find a library for X", "what's available for Y"

## Package Discovery

Search the registry for packages matching a need:

```bash
./build/pulp search "<query>"
./build/pulp suggest --description "<what the user needs>"
```

Or read the registry directly for full details:

```bash
cat tools/packages/registry.json | python3 -m json.tool
```

## Package Categories

| Category | Packages | What They Provide |
|----------|----------|-------------------|
| dsp | aubio, btrack, cycfi-q, daisysp, essentia, fftw, fluidsynth, freeverb, kfr, kissfft, libpd, pffft, rnnoise, rubber-band, signalsmith-dsp, signalsmith-stretch, soundtouch, speexdsp, stk, tinysoundfont | Pitch/time stretch, filters, FFT, pitch detection, physical modeling, noise reduction, synthesis |
| audio-io | alac, dr-libs, fdk-aac, lame, libflac, libsamplerate, libsndfile, libvorbis, miniaudio, oboe, opus, r8brain-free-src | File codecs, sample-rate conversion, device/audio I/O |
| ml | rtneural | Real-time neural network inference |
| ui | fontaudio | Audio icon font |
| utilities | libremidi, rtmidi | MIDI I/O helpers |

### Custom design-import controls (P8)

A package that provides a custom control for the **design importer** declares it
under the optional `design_controls` array on its registry entry
(`registry-schema.json` → `design-control`). Each entry maps a Figma identity
(`match.component_set_key` — authoritative — or `match.name_prefix` — fallback)
to a `factory_id` the package's `pulp::view::View` registers at host startup via
`register_design_control_factory` (see the `import-design` skill, P7 Tier-3). When
an imported design's node matches, the importer emits a `kind=custom` interactive
element carrying that `factory_id`, and `DesignFrameView::build_overlays` builds
the package's control; an unregistered factory renders inert + diagnoses (never a
silent knob). The TS resolver is `customControlFactoryId` in `library-registry.ts`
(merge the installed packages' fragments, then resolve a node's identity). This is
the "give-back" path's long tail: an obvious common control is promoted into core
`pulp::view` + the shipped `library-manifest.json` instead.

## Adding a Package

```bash
./build/pulp add <package-id>
```

This will:
1. Check license compatibility and require explicit review for restricted licenses
2. Check platform support against project targets
3. Warn about overlaps with Pulp built-ins
4. Generate `cmake/pulp-packages.cmake` with FetchContent declarations
5. Update `packages.lock.json`, `DEPENDENCIES.md`, `NOTICE.md`
6. Print usage instructions (target_link_libraries + #include)

## Overlap Awareness

Before suggesting a package, check if Pulp's built-in `core/signal/` already covers the need:

- **Filters**: biquad, SVF, TPT, Linkwitz-Riley, ladder, FIR — built-in
- **FFT/STFT**: built-in (use PFFFT only if benchmarks show it's needed)
- **Delay**: built-in
- **Reverb, compressor, oscillator, ADSR**: built-in
- **Pitch detection**: NOT built-in → suggest cycfi-q
- **Pitch/time stretching**: NOT built-in → suggest signalsmith-stretch
- **Neural network inference**: NOT built-in → suggest rtneural
- **Sample rate conversion**: NOT built-in → suggest libsamplerate or r8brain-free-src
- **Physical modeling**: NOT built-in → suggest daisysp

## Browsing Guides

The hand-written guide set covers the original curated packages. The registry is
larger, so use `pulp search <query>` or inspect `tools/packages/registry.json`
for the full inventory:

```
docs/guides/packages/index.md          — category overview
docs/guides/packages/<package-id>.md   — per-package guide
```

Read these to answer questions about specific packages.

## License Policy

MIT/BSD/Apache/ISC/zlib/BSL/public-domain packages install without prompts.
MPL-2.0 and other unlisted licenses require manual review. GPL/LGPL/AGPL
packages are restricted: the CLI blocks until the user explicitly accepts the
package SPDX with `--accept-license <SPDX>` or declares a commercial-license
basis with `--license-override commercial`. SSPL and proprietary packages are
blocked by default, but the current CLI also accepts
`--license-override commercial` for them, records a compliance warning, and
leaves distribution responsibility with the project. Prefer permissive
alternatives from the registry when available.

## Attribution Audit

`tools/deps/audit.py` now runs **two** invariants under `--strict`:

1. **Consistency** — every `manifest.json` entry must appear in
   `DEPENDENCIES.md`, `NOTICE.md`, and `docs/reference/licensing.md`.
2. **Completeness** — every dep declared in a real manifest source
   (`requirements-docs.txt`, `mkdocs.yml`, root + `bindings/python`
   `FetchContent_Declare`, `external/<dir>/`) must be represented in
   `manifest.json` via name or `external_names` alias.

The completeness check prevents misses where a declared dependency lands with
zero attribution coverage because the audit only verifies cross-file
consistency, not that declared deps are present.

When adding a dep, always touch all four attribution files (manifest,
DEPENDENCIES, NOTICE, licensing) plus — if the CMake / pip / vendored
alias differs from the canonical name — add the alias to
`DEFAULT_ALIASES` in `tools/deps/audit.py` or the `external_names`
list on the manifest entry.

## Bundled Toolchain Pins

Some prebuilt toolchains carry bundled third-party components that are not
separate source directories in `external/`. For example, the Skia prebuilt
toolchain used by visual tests bundles Dawn, HarfBuzz, and ICU through Skia's
`DEPS` file. Track those exact nested revisions in the parent manifest entry
with a structured field such as `determinism.bundled_pins`, and mirror the
human-readable version in `DEPENDENCIES.md`, `external/<toolchain>/VERSION.md`,
and any relevant reference doc.

Do not add a separate manifest entry for a nested toolchain component unless
Pulp fetches, vendors, or redistributes it independently. Otherwise the audit
will require standalone NOTICE/licensing rows for something whose license and
distribution boundary are already covered by the parent prebuilt.

Same rule for Skia *modules* (e.g. `skottie`/`sksg`, linked only when the opt-in
`PULP_LOTTIE` CMake option is enabled): they ship inside the existing Skia
prebuilt under the same Skia BSD-3-Clause entry — clarify their use in the Skia
row of `DEPENDENCIES.md`, do **not** add a separate dependency/NOTICE entry.

The Skia toolchain itself is pinned at `chrome/m149` via the
`danielraffel/skia-builder` fork (see `tools/deps/manifest.json` →
`determinism.skia_builder_fork`). The fork tracks upstream
`olilarkin/skia-builder`'s tag pattern and additionally publishes iOS
device, iOS simulator, visionOS device, visionOS simulator, mac-x86_64,
and `Skia.xcframework` slices upstream does not. While upstream stays on
m144, this fork is the active dependency; revisit when upstream catches up.

iOS-specific layout gotcha: unlike the mac / linux / windows slices, the iOS
zips ship libs under a per-arch subdir
(`build/ios-gpu/lib/Release/{device-arm64,simulator-arm64,simulator-x86_64}/libskia.a`)
because the device and fat-simulator zips would otherwise collide on
identical lib names when unpacked into one tree. `FindSkia.cmake` picks
the right subdir based on `CMAKE_OSX_SYSROOT` + `CMAKE_OSX_ARCHITECTURES`;
`tools/scripts/fetch_skia_for_release.py` keeps the subdir intact for
`ios-device-arm64` and `ios-simulator-arm64-x86_64` matrix slices via
`_IOS_PRESERVE_ARCH_SUBDIR`. Do not extend the flatten step to iOS, and
do not register a single combined iOS slice without per-arch handling.

Multi-arch simulator builds (`-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64`,
the default in `tools/cmake/ios.toolchain.cmake` with
`ONLY_ACTIVE_ARCH=NO`) are intentionally rejected by `FindSkia.cmake`
because each Skia archive is single-arch — silently picking
`simulator-arm64` would arch-mismatch the x86_64 link. The fix is
either to build a single arch (`-DCMAKE_OSX_ARCHITECTURES=arm64`
with `ONLY_ACTIVE_ARCH=YES`) or to pre-fatten the two simulator
archives with `lipo` into a combined `simulator/libskia.a` upstream
of `find_package(Skia)`. The default Pulp iOS smoke (`pulp-ios-sim`,
Apple Silicon dev machines) only needs `arm64`.

The **V8** toolchain follows the same prebuilt-pin pattern (added 2026-06).
It is the optional JS engine backend (`PULP_JS_ENGINE=v8`), a sealed
embeddable `libv8` pinned at tag `v8-15.1.27` via the
`danielraffel/v8-builder` fork (`tools/deps/manifest.json` → V8 entry,
`determinism.release_assets` per-platform URL + sha256). Fetched by
`tools/scripts/fetch_v8_for_release.py` into `external/v8-build/<platform>/`
and resolved by `tools/cmake/FindV8.cmake`. Like Skia, it **bundles**
third-party components internally — ICU, zlib, and Abseil — so it gets one
manifest row for V8 (BSD-3-Clause) plus the bundled-component NOTICE
attribution (ICU/zlib/Abseil), NOT separate manifest rows for the nested
libs (their distribution boundary is the parent `libv8`). Re-verify the
NOTICE list against the v8-builder artifact's own third-party manifest when
the V8 pin bumps. iOS ships a headers-only slice (`library: false`) — V8
needs JIT, forbidden on iOS, so iOS stays JSC. Full rollout/governance:
`planning/2026-06-06-v8-sealed-libv8-provider-migration-plan.md`.

## Bundled Fonts (and similar opt-in compile-time assets)

Bundled fonts under `external/fonts/*.ttf` (`Inter`, `JetBrains Mono`,
`Noto Color Emoji`) are first-class manifest entries with `category: fonts`
and a SHA-256 in the `notes` field. They are NOT the same as the
"bundled-toolchain" pattern above — each font is fetched / vendored
independently and Pulp redistributes it directly.

Add a new bundled font the same way as any other dep:

1. Vendor the file under `external/fonts/<Name>.ttf` (use `git add -f`
   if `.gitignore` covers `/external/*/`).
2. Add a row in `external/fonts/README.md` with the SHA-256, source URL,
   and any gating notes (e.g. `Noto Color Emoji` is gated by
   `PULP_BUNDLE_NOTO_COLOR_EMOJI` and ships in its own
   `pulp_add_binary_data` static lib so macOS/Windows release builds
   can drop the payload).
3. Insert alphabetically into `DEPENDENCIES.md`, `NOTICE.md`, and
   `tools/deps/manifest.json`.
4. Wire CMake: add a `pulp_add_binary_data(...)` block in
   `core/canvas/CMakeLists.txt`, register the resulting target in
   `PULP_SDK_TARGETS` (parent `CMakeLists.txt`), and add the
   accompanying C++ TU that calls
   `pulp::canvas::register_font(...)` or
   `pulp::canvas::register_emoji_fallback(...)` at startup.

Gate large bundles (≥ 1 MB) behind a CMake option that defaults `OFF`
where the platform provides a usable equivalent (e.g. emoji typefaces
on macOS / Windows) and `ON` for headless / CI / Linux / Android.

Synthetic missing-dep test: `tools/deps/test_audit.py::
ManifestSourceScannerTests::test_uncovered_detection_catches_missing_pip_dep`
— don't delete it. If the completeness gate regresses, this is the
regression test that catches it.

## Importer tool-registry fields (pulp import)

`tools/packages/tool-registry.json` `ToolDescriptor` carries optional importer
fields — `frameworks[]`, `spi_min`/`spi_max`, `sdk_min`/`sdk_max`,
`capabilities[]`, `health_check` — for `category: "importer"` tools (the
framework→Pulp project importers resolved by `pulp import`). They're parsed by
`tools/cli/tool_registry.cpp` and ignored for non-importer tools. The importers
themselves install via the tool lane (`pulp tool install <importer>`), never as
project `packages.lock.json` dependencies.
