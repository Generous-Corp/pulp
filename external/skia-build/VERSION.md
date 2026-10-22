# Skia Pre-built Binaries

## Source
- **Repository:** https://github.com/danielraffel/skia-builder (fork of olilarkin/skia-builder)
- **Release:** chrome/m157
- **Release URL:** https://github.com/danielraffel/skia-builder/releases/tag/chrome%2Fm157
- **Downloaded:** 2026-10-22
- **Skia branch:** chrome/m157 (Skia Graphite + Dawn)
- **skia-builder ref:** `f710b2834eaf37dbf1bd39408f9361d10d85eda2`

The fork tracks `olilarkin/skia-builder`'s tag pattern and additionally
publishes iOS device, iOS simulator, visionOS device, visionOS simulator,
mac-x86_64, and `Skia.xcframework` slices that upstream does not. While
upstream stays on m144, this fork is the active dependency.

The chrome/m157 release ships all platform slices, including `linux-arm64`,
Windows x64, and native Windows ARM64. Its Skia branch tip is
`b94213a11b3a2edadfae89f0c57a7f4ab72772c9` at the start of the publishing
workflow (run `38756369724`). Every slice, including Windows ARM64 (built
natively on `windows-11-arm`), comes from that one run and revision: the
shipped headers of every archive are identical once line endings are
normalised.
The bundled build reports Dawn SHA1
`ada567700e13aecf7989d78b21f7d330b522ea80` (`include/dawn/dawn_version.h`).

The **macOS** slices (`mac-arm64`, `mac-x86_64`, `mac-universal`) are pinned to the
`chrome/m157` release, which stamps `LC_BUILD_VERSION minos 13.0`
(macOS 13 Ventura) on both Skia and Dawn. This preserves the corrected floor
from the m151 re-cut, whose original zips accidentally leaked the CI runner's
macOS 15 SDK through Dawn's separate CMake sub-build. These 13.0 stamps are
recorded in `tools/deps/min_os.json`; Pulp's own floor lands slightly higher
(macOS 13.4) because Apple's libc++ gates `std::to_chars(float)` — reached via
`std::format` in the logging path — at 13.3. The non-macOS slices remain the
`chrome/m157` assets.

## Bundled Text and GPU Pins

These revisions are read from Skia's `DEPS` file at the chrome/m157 tip
the build was cut from. Pulp ports against the m157 API surface. m153
introduced two GPU-diagnostics and latency APIs that Pulp validates before
adoption:

- `SkLogHandler` is a process-global, first-install-wins callback for Skia logs.
  Its generic installation policy belongs to Vellum; Pulp may consume a stable,
  bounded diagnostic envelope after that contract transfers.
- `skgpu::graphite::ContextOptions::fExecutor` allows Graphite pipeline
  compilation to use a client-owned `SkExecutor`. Its context-bound lifetime,
  shutdown behavior, and first-visible-frame impact must be measured before it
  becomes policy. Pulp leaves it null today, so pipelines compile inline on the
  render thread. Since m157 Skia ships no default executor (the
  `SkExecutor::MakeFIFOThreadPool`/`MakeLIFOThreadPool` factories are gone);
  an integration supplies its own subclass. Compile that subclass without RTTI
  (`-fno-rtti`, or `/GR-` with MSVC): skia-builder's official builds disable
  RTTI, `~SkExecutor` is the key function, and an RTTI-enabled subclass fails
  to link with an undefined `typeinfo for SkExecutor`.

m154 through m157 change nothing Pulp calls; `rg` over `core/`, `apple/`,
`examples/`, `tools/` and `test/` finds no caller of a removed API:

- m155: `skcpu::Context`, `skcpu::Recorder` and both `makeCPURecorder()` were
  removed; Ganesh flushes return `FlushResult` (implicitly convertible to
  `GrSemaphoresSubmitted`). Pulp renders through Graphite and calls neither.
- m157: Graphite requires iOS 14.0. Pulp's iOS floor is already 16.0 and the
  Skia/Dawn iOS slices were already stamped 14.0 for Dawn.
- m157: `SkYUVAInfo::PlaneConfig::kYUYV` (packed 4:2:2) is compiled only with
  `SK_ENABLE_YUVA_PACKED_422`, off by default; Pulp does not opt in.
- m157: `SK_ASSUME_SINGLE_GANESH_CONTEXT` is an optional Ganesh-only define;
  Pulp's Graphite path does not use it.

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
| `win-gpu/` | Windows | x64, arm64 | release assets consumed by the CLI/SDK release matrix |
| `linux-gpu/` | Linux | x64, arm64 | both slices published on the chrome/m157 release |
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
| `skia-build-ios-device-arm64-gpu-release.zip` | `7c9986329c1d85321ae5331851d6166e7036825739451c11ffa097c61e478e86` |
| `skia-build-ios-simulator-arm64-x86_64-gpu-release.zip` | `c80ce9aff94b0be79306d44271b539fab601b7fe0d3917652db57e6e5602764c` |
| `skia-build-linux-arm64-gpu-release.zip` | `e591268124be249b2a2f56c5042e1e7865bd04556d509c6e4937a04251964435` |
| `skia-build-linux-x64-gpu-release.zip` | `bf74ae1bfd5abd9f49e57c3c6ae6d94c580bbbfe575256fa5077272e1ce0c187` |
| `manifest-key-mac-arm64--skia-build-mac-universal-gpu-release.zip` | `bb24c74f05699fa084df6d68ecb9a427802c3df49ce85d8a7dc696aeb9e19a86` |
| `skia-build-mac-universal-gpu-release.zip` | `bb24c74f05699fa084df6d68ecb9a427802c3df49ce85d8a7dc696aeb9e19a86` |
| `skia-build-mac-x86_64-gpu-release.zip` | `67e801d2e1ca0c8e5a3fc3b26e253fd608348ceae0d0964efe4d7d6d342649e6` |
| `skia-build-wasm-wasm32-gpu-release.zip` | `400e04a3fb71358fa83bbb7be97c2f17a6b5749f434c4ee7c4edca9dfa5d30c1` |
| `skia-build-win-arm64-gpu-release.zip` | `7424b8225f553b3e4dcf292141b49f4e5a6a4506e0c78e6c932350dc5ceedda1` |
| `skia-build-win-x64-gpu-release.zip` | `b744f5583ea2351d6b0607785da8e4ce5a95c669e8bd4a01990e05f32b48a57b` |

The manifest's `mac-arm64` selector uses the universal archive, so its cache
oracle row above intentionally carries the universal digest. The release also
publishes these non-fetcher assets, whose GitHub asset digests were audited:

| Published asset | SHA-256 |
|-----------------|---------|
| skia-build-mac-arm64-gpu-release.zip | `aceaac61d20a8b7e7966ea4bd490e2948a64ea87c101dc70153e7e84f6e68e02` |
| skia-build-visionos-device-arm64-gpu-release.zip | `22760b34de47ee0725d6d783ae3f9c18a682ea09c784bf2c205ac5899111d19e` |
| skia-build-visionos-simulator-arm64-gpu-release.zip | `bae0a268adcfb8ea5c7878b922e770a629a12d4795d20476f3d58112a1f58cd8` |
| skia-build-win-x64-gpu-debug.zip | `2c35a698676759f2978dc1bfeafcc1fda42be55a3c12eb7d65d3277225c203e5` |
| Skia.xcframework.zip | `0e16520af05bcc6cfa05f0c2890730e1a8d00366ac97e64026d4c3c57321ef09` |

The `mac-arm64` manifest key intentionally selects the mac-universal archive,
whose arm64 Skia/Dawn slices measure macOS 13.0. This preserves the verified
universal provider used by Pulp and keeps arm64/universal cache generations
identical; the standalone asset remains recorded above for release coverage.

### Windows asset provenance

Both Windows archives carry `build/win-gpu/msvc-toolchain.json`, a complete
receipt of the static `/MT` MSVC runtime ABI they were built against:

| Slice | Runner | MSVC toolset | Windows SDK | Consumer requirement |
|-------|--------|--------------|-------------|----------------------|
| `win-x64` | `windows-2022` | `14.44.35207` (cl 19.44.35229) | `10.0.26100.0` | MSVC STL/CRT toolset >= 14.44.35207 |
| `win-arm64` | `windows-11-arm` (native ARM64 host) | `14.51.36231` (cl 19.51.36260, VS 18) | `10.0.26100.0` | MSVC STL/CRT toolset >= 14.51.36231 |

The static libraries intentionally leave MSVC STL `__std_*` helpers
unresolved for the consumer's own runtime to supply, so a Windows ARM64
consumer must link with MSVC 14.51 or newer (Visual Studio 18). A VS 2022
(14.44) ARM64 toolchain fails at link with unresolved `__std_*` symbols.

The m153 Windows ARM64 archive was re-published in place from a later Skia
revision and its receipt read "unknown"; m157 has neither problem, because
every slice was built in the one publishing run named above. GitHub release
tags are mutable, so any future change to an asset digest under the same tag
needs the same content comparison before the pin moves.

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
