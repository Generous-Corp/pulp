# Skia Pre-built Binaries

## Source
- **Repository:** https://github.com/danielraffel/skia-builder (fork of olilarkin/skia-builder)
- **Release:** chrome/m153
- **Release URL:** https://github.com/danielraffel/skia-builder/releases/tag/chrome%2Fm153
- **Downloaded:** 2026-08-29
- **Skia branch:** chrome/m153 (Skia Graphite + Dawn)
- **skia-builder ref:** `1f8c8d2c343f360a653bce92d11f8ded9a515208`

The fork tracks `olilarkin/skia-builder`'s tag pattern and additionally
publishes iOS device, iOS simulator, visionOS device, visionOS simulator,
mac-x86_64, and `Skia.xcframework` slices that upstream does not. While
upstream stays on m144, this fork is the active dependency.

The chrome/m153 release ships all platform slices, including `linux-arm64` and
Windows x64. Its Skia branch tip is
`8b8c3872fbc03f025855db96ce683f34ec98a815` at the start of the publishing
workflow (run `32126649056`).
The bundled build reports Dawn SHA1
`f91da75afe31d4d6f47a6da307e1fbabd1b1691a` (`include/dawn/dawn_version.h`).

The **macOS** slices (`mac-arm64`, `mac-x86_64`, `mac-universal`) are pinned to the
`chrome/m153` release, which stamps `LC_BUILD_VERSION minos 13.0`
(macOS 13 Ventura) on both Skia and Dawn. This preserves the corrected floor
from the m151 re-cut, whose original zips accidentally leaked the CI runner's
macOS 15 SDK through Dawn's separate CMake sub-build. These 13.0 stamps are
recorded in `tools/deps/min_os.json`; Pulp's own floor lands slightly higher
(macOS 13.4) because Apple's libc++ gates `std::to_chars(float)` — reached via
`std::format` in the logging path — at 13.3. The non-macOS slices remain the
`chrome/m153` assets.

## Bundled Text and GPU Pins

These revisions are read from Skia's `DEPS` file at the chrome/m153 tip
the build was cut from. Pulp ports against the m153 API surface. In addition to
the existing compatibility changes below, m153 introduces two GPU-diagnostics
and latency APIs that Pulp validates before adoption:

- `SkLogHandler` is a process-global, first-install-wins callback for Skia logs.
  Its generic installation policy belongs to Vellum; Pulp may consume a stable,
  bounded diagnostic envelope after that contract transfers.
- `skgpu::graphite::ContextOptions::fExecutor` allows Graphite pipeline
  compilation to use a client-owned `SkExecutor`. Its context-bound lifetime,
  shutdown behavior, and first-visible-frame impact must be measured before it
  becomes policy.

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
| `linux-gpu/` | Linux | x64, arm64 | both slices published on the chrome/m153 release |
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
| `skia-build-ios-device-arm64-gpu-release.zip` | `0edf6728aec9986508c5f99255188bad7889276b4aa286478a5d2947bc37c39a` |
| `skia-build-ios-simulator-arm64-x86_64-gpu-release.zip` | `428750198cca64307914ebec5adac41be5ae7e07c3ab3c2ddcba047838cbdc3b` |
| `skia-build-linux-arm64-gpu-release.zip` | `a829984ce35141ac1e8f608e29496f69ad24bd2e5215f2899a071dc6c2e0ed0e` |
| `skia-build-linux-x64-gpu-release.zip` | `b132db47979f116a2b35720c6e4e8c7128505499e52b218cc64546f87b0bb363` |
| `skia-build-mac-arm64-gpu-release.zip` | `0ebfe03a209ceefe47edfeae70c3cc6c499583b74f35a26140ea55bad7f1e5a9` |
| `skia-build-mac-universal-gpu-release.zip` | `0ebfe03a209ceefe47edfeae70c3cc6c499583b74f35a26140ea55bad7f1e5a9` |
| `skia-build-mac-x86_64-gpu-release.zip` | `0aeb3a4879d59bf42bb4a42a21cda292b6c5401fa24377b241623a600664471d` |
| `skia-build-wasm-wasm32-gpu-release.zip` | `a5218b84266b0d79dd9c1ce514be6d06f8898085aaa9309a9830442f29ca4887` |
| `skia-build-win-x64-gpu-release.zip` | `9480972c67f07d0762183e962ec3483210eb446ff10ca4e9ddf83ad188f0d11b` |

The manifest's `mac-arm64` selector uses the universal archive, so its cache
oracle row above intentionally carries the universal digest. The release also
publishes these non-fetcher assets, whose GitHub asset digests were audited:

| Published asset | SHA-256 |
|-----------------|---------|
| skia-build-mac-arm64-gpu-release.zip | `9d72be97044edb1db0bc7e110679cc76e78638a5c86dee32463cbbbf9049f5cd` |
| skia-build-visionos-device-arm64-gpu-release.zip | `c5aeb9616d51b0fb03392ba203b485b12fa06d6f4afb30b5415857449e3892e1` |
| skia-build-visionos-simulator-arm64-gpu-release.zip | `451d4499a85fd669ef8911a7f900a0c10b634553e868bb2e98d7f8815de21b55` |
| skia-build-win-x64-gpu-debug.zip | `3524436e4a5f88b55e6d775ad89ef791edb796b938aa2deed26a151109899062` |
| Skia.xcframework.zip | `f1ed8fe8843a5d25b81dee55e261a29337e0dd46afcf25b8cab4fd859a7811e4` |

The `mac-arm64` manifest key intentionally selects the mac-universal archive,
whose arm64 Skia/Dawn slices measure macOS 13.0. This preserves the verified
universal provider used by Pulp and keeps arm64/universal cache generations
identical; the standalone asset remains recorded above for release coverage.

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
