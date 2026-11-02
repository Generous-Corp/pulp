# Skia Pre-built Binaries

## Source
- **Repository:** https://github.com/danielraffel/skia-builder (fork of olilarkin/skia-builder)
- **Release:** chrome/m158
- **Release URL:** https://github.com/danielraffel/skia-builder/releases/tag/chrome%2Fm158
- **Downloaded:** 2026-11-01
- **Skia branch:** chrome/m158 (Skia Graphite + Dawn)
- **skia-builder ref:** `312f05e24a5d5a45d4a3e70ca70ff3ccff1b4d97`

The fork tracks `olilarkin/skia-builder`'s tag pattern and additionally
publishes iOS device, iOS simulator, visionOS device, visionOS simulator,
mac-x86_64, and `Skia.xcframework` slices that upstream does not. While
upstream stays on m144, this fork is the active dependency.

The chrome/m158 release ships all platform slices, including `linux-arm64`,
Windows x64, and native Windows ARM64. Its Skia branch tip is
`be7d7c3e996e567eda7dc9774274f7a0103cdb8c` at the start of the publishing
workflow (`build-skia.yml` run `40626447857`, attempt 1). Every slice except
Linux, including Windows ARM64 (built natively on `windows-11-arm`), comes from
that one run and revision. The two Linux archives were re-cut from the same
revision by run `41466672481` (skia-builder `a43f061b`) with
`skia_use_partition_alloc = false`, because the first Linux archives referenced
PartitionAlloc's `RawPtrBackupRefImpl` with no definition; the release notes'
replacement ledger records both digests. The re-cut changed no shipped header.
Across all of them, the shipped headers of every archive are identical once
line endings are normalised, except that both Windows archives carry
skia-builder's ARM64EC guard in `include/private/SkFeatures.h`
(`&& !defined(_M_ARM64EC)` on the x86 CPU test, from
`patches/fix_windows_arm64ec_archive_flags.patch`), which changes nothing for
an x64 or ARM64 consumer. Neither Windows archive ships an `arm64ec/` or
`arm64x/` library slice, so ARM64EC/ARM64X consumers stay fail-closed.
The bundled build reports Dawn SHA1
`1716e88cc9e58398fab5d4473f7f2e858667b677` (`include/dawn/dawn_version.h`).

The `chrome/m158` branch has moved past the built revision since the release
was cut: Chromium's M158 DEPS (and the matched V8 release) record Skia
`759bfdeaae370993f1ddc5f2168145770936288d`, six cherry-picks later. The
published archives, and therefore Pulp, use `be7d7c3e`; the matched V8 release
records both and binds its provider pair to `be7d7c3e`.

The **macOS** slices (`mac-arm64`, `mac-x86_64`, `mac-universal`) are pinned to the
`chrome/m158` release, which stamps `LC_BUILD_VERSION minos 13.0`
(macOS 13 Ventura) on both Skia and Dawn. This preserves the corrected floor
from the m151 re-cut, whose original zips accidentally leaked the CI runner's
macOS 15 SDK through Dawn's separate CMake sub-build. These 13.0 stamps are
recorded in `tools/deps/min_os.json`; Pulp's own floor lands slightly higher
(macOS 13.4) because Apple's libc++ gates `std::to_chars(float)` — reached via
`std::format` in the logging path — at 13.3. The non-macOS slices remain the
`chrome/m158` assets.

## Bundled Text and GPU Pins

These revisions are read from Skia's `DEPS` file at the chrome/m158 tip
the build was cut from. Pulp ports against the m158 API surface. m153
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
  RTTI, and the executor base is instantiated inside Skia, so an RTTI-enabled
  subclass fails to link against typeinfo the archive does not contain. Since
  m158 `SkExecutor` derives from `SkExecutorBase<std::function<void()>>`
  (aliased `SkFunctionalExecutor`); Graphite takes that type unless
  `SK_GRAPHITE_USE_CALLBACK_EXECUTOR` selects `SkCallbackExecutor`, which Pulp
  does not define.

m154 through m158 change nothing Pulp calls; `rg` over `core/`, `apple/`,
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
- m158: `SkExecutor` became a template (see above). The PNG, JPEG, Rust PNG
  and WebP encoder options take `const skhdr::Metadata* fHdrMetadata` (PNG's
  was a value), and `SkWebpEncoder::Options` gained `fMethod`; Pulp sets
  neither. No `SK_LEGACY_*` guard and no shipped header was removed between
  the m157 and m158 archives; `include/private/SkGPUContext.h` and
  `SkOnlyStackAllocatable.h` were added. Dawn dropped the
  `WGPUDawnLoadCacheDataFunction`/`WGPUDawnStoreCacheDataFunction` typedefs,
  which Pulp never used.

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
| `linux-gpu/` | Linux | x64, arm64 | both slices published on the chrome/m158 release |
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
| `skia-build-ios-device-arm64-gpu-release.zip` | `56a76f22b30a2a045e7dc72bf645221b403408a2d8aaf01081ffe05937e052ca` |
| `skia-build-ios-simulator-arm64-x86_64-gpu-release.zip` | `e446cbfa3302026ce055dd9eae980f466cac89f8f9ad50b07e5b2aa4989d1488` |
| `skia-build-linux-arm64-gpu-release.zip` | `053833562a4ec4eeb97d71515fdf437cad7b6e8d668ca7e7d8b486f65528aaf9` |
| `skia-build-linux-x64-gpu-release.zip` | `74c31d020cd18f1f9e544b1d8440bcc293e82982e50a9777a3940d84e2ccd666` |
| `manifest-key-mac-arm64--skia-build-mac-universal-gpu-release.zip` | `dca73c0eee5b0f6d129fe357d0754f7165c6b436051e0de947aeedd3e1bde250` |
| `skia-build-mac-universal-gpu-release.zip` | `dca73c0eee5b0f6d129fe357d0754f7165c6b436051e0de947aeedd3e1bde250` |
| `skia-build-mac-x86_64-gpu-release.zip` | `2a795f54bb22feb46a499d56740f98a15b6e95bfdf5923b82b222ef95edb2f47` |
| `skia-build-wasm-wasm32-gpu-release.zip` | `a3382b3a53d13780667f7db9d9969c42d531451e55de35faaadf4e62f64ce357` |
| `skia-build-win-arm64-gpu-release.zip` | `d2586d3ae1d3a1103aa87dda5afecfed2ced7be82d0276f6b9a15fd034d9a5c7` |
| `skia-build-win-x64-gpu-release.zip` | `a23b3fc4f75eda3420ec7de70cbc68307be6f4fed37d004f43e9bb1ea81e5854` |

The manifest's `mac-arm64` selector uses the universal archive, so its cache
oracle row above intentionally carries the universal digest. The release also
publishes these non-fetcher assets, whose GitHub asset digests were audited:

| Published asset | SHA-256 |
|-----------------|---------|
| skia-build-mac-arm64-gpu-release.zip | `20735644dc7c65c71522a4d2b303f19fcd236c2e24468759daa86c7829c39edb` |
| skia-build-visionos-device-arm64-gpu-release.zip | `6eaee60b606254890209ac73f73e22479e9d6877a28ffdcc99e69aa05e4ab751` |
| skia-build-visionos-simulator-arm64-gpu-release.zip | `b506044cfe7a8500727fe787ad00b309fd512ad65f6d35de161608a4c0e4b1f0` |
| skia-build-win-x64-gpu-debug.zip | `d7b6255332b92167b9ec6a00cbab020f7518f1fa88a768b07c21ce17fbb06340` |
| Skia.xcframework.zip | `621ebb6b29f20fca34996eea725c477bc12467818895bd0f4e850b1d0101da26` |

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
`fetch_skia_for_release.py windows-arm64` checks this receipt before the
archive enters a cache, on cold fetches and warm hits alike. It refuses an
archive whose receipt is missing, does not name native ARM64, or leaves the
toolset, SDK or compiler fields empty or reading "unknown".

The m153 Windows ARM64 archive was re-published in place from a later Skia
revision and its receipt read "unknown"; m158 has neither problem, because
every slice was built in the one publishing run named above. The m158 Windows
receipts name the same toolsets as m157. GitHub release
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
