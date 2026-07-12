# Skia Pre-built Binaries

## Source
- **Repository:** https://github.com/danielraffel/skia-builder (fork of olilarkin/skia-builder)
- **Release:** chrome/m151
- **Release URL:** https://github.com/danielraffel/skia-builder/releases/tag/chrome%2Fm151
- **Downloaded:** 2026-07-07
- **Skia branch:** chrome/m151 (Skia Graphite + Dawn)

The fork tracks `olilarkin/skia-builder`'s tag pattern and additionally
publishes iOS device, iOS simulator, visionOS device, visionOS simulator,
mac-x86_64, and `Skia.xcframework` slices that upstream does not. While
upstream stays on m144, this fork is the active dependency.

The chrome/m151 release ships all platform slices, including `linux-arm64`.
The bundled build reports Dawn SHA1
`7807dcbdca245e462617c04d544706394db245ba` (`include/dawn/dawn_version.h`).

The **macOS** slices (`mac-arm64`, `mac-x86_64`, `mac-universal`) are pinned to the
`chrome/m151-minos13` re-cut, which stamps `LC_BUILD_VERSION minos 13.0`
(macOS 13 Ventura) on both Skia and Dawn. The original `chrome/m151` macOS
zips accidentally leaked the CI runner's macOS 15 SDK as the deployment
target through Dawn's separate CMake sub-build; the re-cut pins Google's
`mac_deployment_target` (13.0 at m151) explicitly. These 13.0 stamps are
recorded in `tools/deps/min_os.json`; Pulp's own floor lands slightly higher
(macOS 13.3) because Apple's libc++ gates `std::to_chars(float)` — reached via
`std::format` in the logging path — at 13.3. The non-macOS slices remain the
original `chrome/m151` assets (digests below unchanged).

## Bundled Text and GPU Pins

These revisions are read from Skia's `DEPS` file at the chrome/m151 tip
the build was cut from. Pulp ports against the m150 API surface (unchanged
through m151):

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

Native slices (mac / win / linux / ios / visionos):
- Graphite GPU backend: enabled
- Dawn (WebGPU): enabled
- Metal: enabled (macOS/iOS)
- ICU Unicode: enabled
- SVG module: enabled
- Skottie (Lottie): enabled
- Paragraph/text shaping: enabled
- Build type: Release (optimized)

The **`wasm-gpu`** slice is the exception, and the difference is load-bearing:
- GPU backend: **Ganesh on WebGL2** (`SK_GANESH` + `SK_GL`) — **not** Graphite.
- Dawn (WebGPU): **absent**. The zip ships no `libdawn_combined.a` and zero
  `wgpu` symbols, so `SK_GRAPHITE` / `SK_DAWN` must not be defined for
  Emscripten. `FindSkia.cmake`'s Emscripten arm encodes this, and
  `tools/scripts/verify_wasm_skia_slice.py` asserts it in CI.
- Skottie / sksg: **absent in practice** — `libskottie.a` leaves `skjson::*`
  undefined and the zip ships no jsonreader/skresources archive, so
  `PULP_LOTTIE` cannot be enabled on wasm.
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
| `win-gpu/` | Windows | x64 | |
| `linux-gpu/` | Linux | x64, arm64 | both slices published on the chrome/m151 release |
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
| `skia-build-ios-device-arm64-gpu-release.zip` | `d82341a075cf63ce70c659828017a928b3b5cc41bed74795dde61edce9c8f29b` |
| `skia-build-ios-simulator-arm64-x86_64-gpu-release.zip` | `2c98b79b4f4c20282ce665ba79bb040124ee6424f16b087c2c6c01c1acb177a2` |
| `skia-build-linux-arm64-gpu-release.zip` | `2420eed074e041384973338f9d8a41364b9ff444ffa0eb1857cb1ebdbd8781e9` |
| `skia-build-linux-x64-gpu-release.zip` | `518b74ee7f0b245c209349023e58a2891a83a7ab776504d7d8a23d1e76fbf4de` |
| `skia-build-mac-arm64-gpu-release.zip` | `648250f9ee625f0c6c73c521b5a2de7cf46812b06aa2300e4bec8b2bb6d4081b` |
| `skia-build-mac-universal-gpu-release.zip` | `284964fda380a2cc5ff4f885ae557ef04dab5987ebd94fc01354b95878ad85cf` |
| `skia-build-mac-x86_64-gpu-release.zip` | `f1734e9f41c0d01700d446282550725a7b6b42ebc906ba295f1f563112831f17` |
| `skia-build-wasm-wasm32-gpu-release.zip` | `8a5a24368866d210fe47bac2ea03b67e63c429d1c9ea5ee11dc06d9db831def9` |

## Libraries Per Platform

Each platform includes the following (see the wasm carve-out under **Build
Configuration** — the `wasm-gpu` slice ships no `libdawn_combined.a`, and its
`libskia.a` is Ganesh/WebGL2 rather than Graphite):
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
