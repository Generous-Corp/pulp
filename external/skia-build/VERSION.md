# Skia Pre-built Binaries

## Source
- **Repository:** https://github.com/danielraffel/skia-builder (fork of olilarkin/skia-builder)
- **Release:** chrome/m152
- **Release URL:** https://github.com/danielraffel/skia-builder/releases/tag/chrome%2Fm152
- **Downloaded:** 2026-08-08
- **Skia branch:** chrome/m152 (Skia Graphite + Dawn)

The fork tracks `olilarkin/skia-builder`'s tag pattern and additionally
publishes iOS device, iOS simulator, visionOS device, visionOS simulator,
mac-x86_64, and `Skia.xcframework` slices that upstream does not. While
upstream stays on m144, this fork is the active dependency.

The chrome/m152 release ships all platform slices, including `linux-arm64` and
Windows x64. Its Skia branch tip is
`2a9b593bab4b2fd019fa494c8d401ff1fab0b883`.
The bundled build reports Dawn SHA1
`1e897275172a23f27b0022fa6beae3084ed54a9b` (`include/dawn/dawn_version.h`).

The **macOS** slices (`mac-arm64`, `mac-x86_64`, `mac-universal`) are pinned to the
`chrome/m152` release, which stamps `LC_BUILD_VERSION minos 13.0`
(macOS 13 Ventura) on both Skia and Dawn. This preserves the corrected floor
from the m151 re-cut, whose original zips accidentally leaked the CI runner's
macOS 15 SDK through Dawn's separate CMake sub-build. These 13.0 stamps are
recorded in `tools/deps/min_os.json`; Pulp's own floor lands slightly higher
(macOS 13.4) because Apple's libc++ gates `std::to_chars(float)` — reached via
`std::format` in the logging path — at 13.3. The non-macOS slices remain the
`chrome/m152` assets.

## Bundled Text and GPU Pins

These revisions are read from Skia's `DEPS` file at the chrome/m152 tip
the build was cut from. Pulp ports against the m152 API surface:

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
- `SkPath::updateBoundsCache()` was removed after SkPath bounds became eager;
  Pulp has no callers.

Bundled dependency revisions:

- HarfBuzz: `9cb1fee51069b206effb4736e443b038d230789d`
- ICU: `d578f2e8b7bd5938e21cfb6bf15c079e0aa5b738`

The B.0 visual harness pin (`skia-python==144.0.post2`) intentionally
trails the C++ surface because Python bindings ship one milestone
behind; the C++ raster path is the source of truth for goldens, and
the Python smoke is an optional fallback when libskia.a is absent in
a fresh worktree.

## Build Configuration

Native slices (mac / win / linux / ios / visionos):
- Graphite GPU backend: enabled
- Dawn (WebGPU): enabled
- Metal: enabled (macOS/iOS)
- ICU Unicode: enabled
- SVG module: enabled
- Skottie (Lottie): native bundles ship `libskottie.a`, `libsksg.a`,
  `libjsonreader.a`, and `libskresources.a`. `core/canvas/CMakeLists.txt` still
  uses a real try-link before enabling `PULP_LOTTIE`, so a platform slice can
  fail closed if its archive set differs.
- Paragraph/text shaping: enabled
- Build type: Release (optimized)

The **`wasm-gpu`** slice is the exception, and the difference is load-bearing:
- GPU backend: **Ganesh on WebGL2** (`SK_GANESH` + `SK_GL`) — **not** Graphite.
- Dawn (WebGPU): **absent**. The zip ships no `libdawn_combined.a` and zero
  `wgpu` symbols, so `SK_GRAPHITE` / `SK_DAWN` must not be defined for
  Emscripten. `FindSkia.cmake`'s Emscripten arm encodes this, and
  `tools/scripts/verify_wasm_skia_slice.py` asserts it in CI.
- Skottie / sksg: archives are present, but `libjsonreader.a` and
  `libskresources.a` are absent. The real link probe therefore disables
  `PULP_LOTTIE`; Lottie is unavailable on wasm.
- Built with `is_trivial_abi=true`, so consumers **must** define
  `SK_TRIVIAL_ABI` or `wasm-ld` links a trapping stub for cross-boundary
  `sk_sp` calls (the failure is a bare `RuntimeError: unreachable` on the
  first frame).
- Validated against the Emscripten / wasi-sdk versions pinned in
  `tools/deps/manifest.json` → `determinism.web_toolchain`.

## Platforms Included

| Directory | Platform | Architectures | Notes |
|-----------|----------|--------------|-------|
| `mac-gpu/` | macOS | arm64, x86_64, universal | mac-x86_64 only in the fork |
| `win-gpu/` | Windows | x64 | release asset consumed by the CLI/SDK release matrix |
| `linux-gpu/` | Linux | x64, arm64 | both slices published on the chrome/m152 release |
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
| `skia-build-ios-device-arm64-gpu-release.zip` | `e67923bbce6d9a7d15b640633a300e22991815e4dfa7f9e5d4198261b87e16d9` |
| `skia-build-ios-simulator-arm64-x86_64-gpu-release.zip` | `219b50662844797428f7c66920aa33eb8790b344e6626d35e252ed3c7b3bf6cf` |
| `skia-build-linux-arm64-gpu-release.zip` | `12aa2ba8a43472461dd552f7ac28420137bd6a3175542563c3bbbf06124d7df6` |
| `skia-build-linux-x64-gpu-release.zip` | `b0114b0edd1e07d274fd37b8fb3508966590b9dda1fdd1f3ab24441c12dee4ed` |
| `skia-build-mac-arm64-gpu-release.zip` | `4bf7afda5dd2e20a41093255431a12bdb0df9eca56883ce0ba708cd471fb2a39` |
| `skia-build-mac-universal-gpu-release.zip` | `a066fd95d447fe00aa9890ae404fda1fb1db369006b1c705b401c8605f8ae244` |
| `skia-build-mac-x86_64-gpu-release.zip` | `f008bb70143142b1b9feec122c864ca7a5a24c895a8fbdeb75c9c7b6c07f3a63` |
| `skia-build-wasm-wasm32-gpu-release.zip` | `549e9aa6a6ede9c796be7866d244809c0a3f6c9d82367a6fa3f6e629e964decb` |
| `skia-build-win-x64-gpu-release.zip` | `f96c726ac7fbc32b36334eaadcc463ac9c6c5411afb97576f334a203abb99bc6` |

## Libraries Per Platform

Each platform includes the following (see the wasm carve-out under **Build
Configuration** — the `wasm-gpu` slice ships no `libdawn_combined.a`, and its
`libskia.a` is Ganesh/WebGL2 rather than Graphite):
- `libskia.a` — Core Skia + Graphite GPU backend
- `libdawn_combined.a` — Dawn WebGPU implementation
- `libskshaper.a` — Text shaping (HarfBuzz)
- `libskparagraph.a` — Paragraph layout
- `libskottie.a` — Lottie animation (ships, but cannot link — see **Build
  Configuration**; Lottie is disabled on every target)
- `libsksg.a` — Scene graph
- `libsvg.a` — SVG rendering
- `libskunicode_icu.a` — Unicode support
- `libskunicode_core.a` — Unicode core

## License
Skia: BSD-3-Clause (Google)
Dawn: BSD-3-Clause (Google)
