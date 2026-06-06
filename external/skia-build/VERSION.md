# Skia Pre-built Binaries

## Source
- **Repository:** https://github.com/danielraffel/skia-builder (fork of olilarkin/skia-builder)
- **Release:** chrome/m150
- **Release URL:** https://github.com/danielraffel/skia-builder/releases/tag/chrome%2Fm150
- **Downloaded:** 2026-06-05
- **Skia branch:** chrome/m150 (Skia Graphite + Dawn)

The fork tracks `olilarkin/skia-builder`'s tag pattern and additionally
publishes iOS device, iOS simulator, visionOS device, visionOS simulator,
mac-x86_64, and `Skia.xcframework` slices that upstream does not. While
upstream stays on m144, this fork is the active dependency.

The chrome/m150 release does NOT ship a `linux-arm64` slice (m149 did).
Linux arm64 stays on the m149 asset or rebuilds from source via
`tools/build-skia.sh` until the fork republishes that slice.

## Bundled Text and GPU Pins

These revisions are read from Skia's `DEPS` file at the chrome/m150 tip
the build was cut from. Pulp ports against the m150 API surface:

- Gradient construction migrated from `SkGradientShader::Make*` to the
  `SkShaders::*` namespace with the `SkGradient` data class.
- `skia::textlayout::ParagraphBuilder::make()` now takes a third
  `sk_sp<SkUnicode>` argument (see `core/canvas/src/skia_unicode.hpp`
  for the shared singleton).
- `SkSerialImageProc` callbacks return `sk_sp<const SkData>`.
- `SkRegion::setRects` takes an `SkSpan<const SkIRect>` instead of a
  raw pointer + count pair.
- The `SkStrikeRef` accessor used by the text shaper changed; see
  `core/canvas/src/text_shaper.cpp`.

The B.0 visual harness pin (`skia-python==144.0.post2`) intentionally
trails the C++ surface because Python bindings ship one milestone
behind; the C++ raster path is the source of truth for goldens, and
the Python smoke is an optional fallback when libskia.a is absent in
a fresh worktree.

## Build Configuration
- Graphite GPU backend: enabled
- Dawn (WebGPU): enabled
- Metal: enabled (macOS/iOS)
- ICU Unicode: enabled
- SVG module: enabled
- Skottie (Lottie): enabled
- Paragraph/text shaping: enabled
- Build type: Release (optimized)

## Platforms Included

| Directory | Platform | Architectures | Notes |
|-----------|----------|--------------|-------|
| `mac-gpu/` | macOS | arm64, x86_64, universal | mac-x86_64 only in the fork |
| `win-gpu/` | Windows | x64 | |
| `linux-gpu/` | Linux | x64 | arm64 not published on the chrome/m150 release; rebuild from source or stay on the m149 slice |
| `ios-gpu/` | iOS device + simulator | arm64, arm64+x86_64 | fork-only slices |
| `visionos-gpu/` | visionOS device + simulator | arm64 | fork-only slices |
| `wasm-gpu/` | WebAssembly | wasm32 | |

`Skia.xcframework.zip` is also available from the fork as a single
multi-platform Apple distribution if a downstream consumer prefers it
over the per-slice zips.

## To Update

1. Check for new releases: https://github.com/danielraffel/skia-builder/releases
2. Download the new platform zips
3. Verify each zip SHA-256 against the release asset digest
4. Extract to this directory (replacing `build/`) — `tools/scripts/fetch_skia_for_release.py`
   handles this end-to-end and writes the asset stamp at
   `external/skia-build/.skia-asset-sha256`
5. Update this VERSION.md, `tools/deps/manifest.json`, `DEPENDENCIES.md`,
   and any harness README references with the new release tag, asset
   digests, and dated metadata
6. Regenerate raster PNG goldens if the Skia release, bundled text
   pins, font files, sampler settings, color type, alpha type, DPI,
   or backend changes
7. Commit (binaries are NOT under Git LFS in this checkout; the fetch
   script populates them at configure time from the manifest)

Or run: `./tools/build-skia.sh <platform>` to build from source.

## Release Asset Digests

| Asset | SHA-256 |
|-------|---------|
| `skia-build-ios-device-arm64-gpu-release.zip` | `0b0a0dbb0224e94ea61d0f9d0107afb7ae063eae3ddb3ea0590bcd0f05fd0c44` |
| `skia-build-ios-simulator-arm64-x86_64-gpu-release.zip` | `86a60cf963b41da8d30a46e98bb69d24a8bc7f102d8c0908087c9a8be3aa6a75` |
| `skia-build-linux-x64-gpu-release.zip` | `bb4d3a868a72560b25e467952bb7792d4af2bc9dcab1f77afca208b7b1f0d07b` |
| `skia-build-mac-arm64-gpu-release.zip` | `13b0e9818c3b05db661af85cb1e2bf2ef10e30d468b81351dd90295237d17734` |
| `skia-build-mac-universal-gpu-release.zip` | `f27908b847a6a130828073f65a02d052bb1672c999bc9d26384348719c315035` |

## Libraries Per Platform

Each platform includes:
- `libskia.a` — Core Skia + Graphite GPU backend
- `libdawn_combined.a` — Dawn WebGPU implementation
- `libskshaper.a` — Text shaping (HarfBuzz)
- `libskparagraph.a` — Paragraph layout
- `libskottie.a` — Lottie animation
- `libsksg.a` — Scene graph
- `libsvg.a` — SVG rendering
- `libskunicode_icu.a` — Unicode support
- `libskunicode_core.a` — Unicode core

## License
Skia: BSD-3-Clause (Google)
Dawn: BSD-3-Clause (Google)
