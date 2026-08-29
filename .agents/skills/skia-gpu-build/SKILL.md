---
name: skia-gpu-build
description: Enable a Skia + Dawn GPU build of Pulp (MacGpuWindowHost, Skia Graphite). Covers the prebuilt skia-builder binaries, the headers-only fresh-worktree trap, reusing another checkout's cached libs via SKIA_DIR, FindSkia layouts, verifying PULP_HAS_SKIA / MacGpuWindowHost, and the raster-fallback + GPU-wedge gotchas. Use whenever GPU rendering "doesn't work" or a build silently came up CPU-only.
requires:
  - tools/cmake/FindSkia.cmake
  - tools/build-skia.sh
---

# Building Pulp with Skia + GPU

Pulp's GPU path is **Skia Graphite over Dawn (WebGPU)**. `PULP_ENABLE_GPU` is
**ON by default**, but GPU only actually turns on when CMake can *find the
prebuilt Skia libraries*. If it can't, the build silently comes up **CPU-only**
(CoreGraphics raster, `MacWindowHost`) with no hard error — that's the #1
"GPU doesn't work" cause.

## Start with installed GPU health evidence

Before inspecting libraries or opening a window, run:

```bash
pulp doctor gpu --json
```

This performs bounded render/readback and compute/map work through the installed
Pulp paths. Exit 0 means all required real-work proofs passed and at least one
required probe has authentic identity; exit 1 is a completed measured failure, and
exit 2 means requested evidence is unavailable or unverified. Use
`pulp doctor gpu --no-render --json` for inventory/preflight without acquiring
a GPU device; it returns unverified, never passed. The `pulp_gpu_doctor` MCP tool
returns the same typed evidence and both installed surfaces work independently
of the current directory.

Record adapter identity exactly as Dawn reports it. A backend label such as
Metal, D3D12, or Vulkan does not prove that the adapter is hardware or discrete,
and null/software adapters are never hardware passes. Probe identities are
independent; do not infer that separately acquired
Renderer3D, HeadlessSurface, and GpuCompute devices are the same device.
Continue with the manual
bundle and symbol checks below when the result is unavailable/unverified or
when a completed probe fails.

## The fresh-worktree trap (most common)

`external/skia-build/` in a fresh worktree often contains **only headers** —
`include/`, `modules/`, `VERSION.md` — and **no compiled libs**. FindSkia needs
`external/skia-build/build/<platform>-gpu/lib/Release/*.a` (e.g.
`libskia.a`, `libdawn_combined.a`, `libskparagraph.a`, `libskunicode_icu.a`).
Headers-only ⇒ `-- Skia: SKIA_DIR not set — Skia rendering disabled` (or "not
found") ⇒ `PULP_HAS_SKIA` undefined ⇒ CPU-only build.

```bash
# Are the compiled libs actually present in THIS worktree?
ls external/skia-build/build/*/lib/Release/libskia.a 2>/dev/null || echo "headers-only — no GPU"
```

## Fastest fix: materialize the pinned immutable cache generation

Do not rebuild Skia and do not borrow mutable bytes from another checkout.
Populate the manifest-selected, platform-plus-asset-SHA generation through the
release fetcher, then point `SKIA_DIR` at that exact stamped generation:

```bash
SKIA_CACHE_ROOT="${PULP_SKIA_CACHE_ROOT:-$HOME/.cache/pulp/skia}"
python3 tools/scripts/fetch_skia_for_release.py darwin-arm64 \
  --cache-root "$SKIA_CACHE_ROOT"
SKIA_DIR="$(python3 tools/scripts/fetch_skia_for_release.py darwin-arm64 \
  --cache-root "$SKIA_CACHE_ROOT" --print-cache-dest)"

# Configure a SEPARATE GPU build dir (keep the CPU build/ for deterministic
# CoreGraphics goldens — see "Don't mix" below).
cmake -S . -B build-gpu -DCMAKE_BUILD_TYPE=Release \
  -DSKIA_DIR="$SKIA_DIR" \
  -DPULP_ENABLE_GPU=ON
```

`SKIA_DIR` may be an env var or a `-D` cache entry; FindSkia also auto-discovers
`external/skia-build` and `$SKIA_DIR`. SKIA_DIR must point at the dir that
*contains* `build/<platform>-gpu/lib/Release` + `build/include` (skia-builder
layout); flat `mac/lib` + `include` layouts are also accepted (see
`tools/cmake/FindSkia.cmake`).

A valid cached generation keeps headers and libraries together and carries both
the verified archive stamp and extracted-file digest receipt. An arbitrary
sibling checkout is not provenance: never use its mutable
`external/skia-build` directory as `SKIA_DIR`, and never mix one generation's
headers with another's libraries.

## Building from source (only when no cached libs anywhere)

`tools/build-skia.sh` builds the currently pinned chrome/m153 Skia+Dawn from source — slow
(tens of minutes). Prefer the release zip from
`danielraffel/skia-builder` (chrome/m153, see `external/skia-build/VERSION.md`)
or the immutable fetcher-owned cache generation above.

For m153, prove the provider exposes both new integration surfaces before a
product build:

```bash
python3 tools/scripts/verify_skia_m153_capabilities.py \
  --platform darwin-arm64 --skia-dir "$SKIA_DIR"
```

The directory must be the fetcher's exact stamped manifest generation. This
compile/links/runs `SkLogHandler` and
`skgpu::graphite::ContextOptions::fExecutor`; headers or unstamped local
libraries alone are not a pass.
`SkLogHandler` is process-global and first-install-wins, while the executor must
outlive the Graphite context. Generic installation and executor policy are
Vellum-owned; a Pulp build should consume those contracts rather than install a
second global handler or duplicate context policy.

For a universal macOS generation, use `--platform darwin-universal --result
<receipt.json>` on an Apple-Silicon host. That is deliberately a dual-slice
gate: arm64 runs natively and x86_64 runs through explicit Rosetta, and the
single result binds both compile/link/run records to the exact universal asset,
generation receipt, probe source, and checkout SHA. A successful universal
product link alone does not prove these currently-unused m153 symbols exist in
both slices.

## Verify it's REALLY a GPU build (necessary AND sufficient)

Configure log must show BOTH:

```
-- Pulp: WebGPU (Dawn) enabled
-- Skia: found at <dir> (mac)
```

Then check the binary actually contains the GPU host (the CLAUDE.md GPU-host
gotcha — a CPU-only binary returns the non-overriding `MacWindowHost`, so
`set_design_viewport` / aspect-lock silently no-op):

```bash
nm build-gpu/examples/<app>/<app> 2>/dev/null | grep -q MacGpuWindowHost \
  && echo "OK: GPU host" || echo "FAIL: CPU-only"
# or for a packaged .app:  strings .../MyApp-Bin | grep -F "[gpu-host]"
```

A headless `render_to_png(root, w, h, scale, ScreenshotBackend::skia)` (or an
example's `--screenshot`) is the cheapest proof the Skia pipeline links + runs
without opening a window.

For terminal A4 native DPR measurements, a normal GPU build is insufficient:
configure a separate Release build with both `PULP_BENCHMARK=ON` and
`PULP_TRACING=ON`, then build only
`pulp-gpu-dpr-native-measurement` through `tools/ci/governed-build.sh`. The
producer opts into GPU timing on its public editor surface, refuses non-hardware
Dawn adapters, and reports incomplete when timestamp queries or authentic
counter evidence are unavailable. Its empirical timer floor is derived from
positive samples and must distinguish five baseline trials from five trials
with eight times the known GPU work; a quantized nonzero timer with no detectable
control is not valid measurement. Do not reuse this dev tracing build as a
shipping artifact.

## Gotchas (each cost real time)

- **Release Skia archives require `SK_RELEASE` in every consumer, including
  Debug Pulp builds.** The published skia-builder libraries live under
  `lib/Release` and compile Skia's inline ref-counting code with release
  semantics. `FindSkia.cmake` therefore exports `SK_RELEASE` from the
  `skia::skia` imported target for every backend. Without it, a Debug Pulp
  translation unit can compile a derived `SkRefCnt` destructor with
  `SK_DEBUG`, while the archive's `internal_dispose()` uses release behavior;
  teardown then traps at `SkRefCnt.h:41` (`fRefCnt was 0`) even though ownership
  is correct. If a Graphite `Recorder` teardown hits that assertion, inspect
  the target's `INTERFACE_COMPILE_DEFINITIONS` before changing `sk_sp`
  ownership or destruction order. `tools/scripts/test_findskia_arch_assert.py`
  guards this imported-target ABI contract.
- **Graphite drops any raster `SkImage` it is handed — it never uploads one on
  its own.** When Graphite meets a non-GPU-backed image while building a paint
  key it asks `Recorder::clientImageProvider()->findOrCreate()`, and Skia's
  default provider returns nothing; the draw is discarded, the draw call still
  reports success, and the only trace is
  `[skia] WARNING - Couldn't convert SkImage to a Graphite-backed
  representation`. That is why `SkiaSurface` installs
  `GraphiteImageProvider` (`core/render/src/graphite_image_provider.hpp`) on
  every Recorder it creates. `SkiaCanvas::ensure_gpu_image` pre-uploads at
  Pulp's own draw entry points, but it can only reach images Pulp constructs —
  a Skia module that decodes internally (`SkSVGDOM` turning
  `<image href="data:...">` into a raster image) bypasses it entirely. **If you
  add a code path that hands Graphite an image, do not add another manual
  pre-upload — the provider already covers it.** Raster is the control here: an
  asset that composites on the raster backend and vanishes on the GPU one is
  this class of bug, not a bad asset. Covered by
  `test/test_headless_surface.cpp` (`[headless-surface][image]` /
  `[headless-surface][svg]`), which asserts the asset's own pixels, since
  asserting the draw call returned true is exactly what missed it.
- **`SkPathBuilder::setFillType()` does nothing — pass the fill type to the
  CONSTRUCTOR.** The pinned bundle's headers and its compiled `libskia.a`
  disagree on `SkPathBuilder`'s member layout. `setFillType` and `fillType()`
  are defined *inline in the header*, so they touch `fFillType` at the offset
  the header computes (measured: byte 400), while `snapshot`/`detach` are
  compiled inside the library and read the offset it computes (byte 96). The
  setter is therefore accepted, the builder's own getter agrees with it, and the
  produced `SkPath` still comes out `kWinding` — every even-odd fill renders as
  a solid nonzero one with nothing logged. Use `SkPathBuilder b(fillType)` (or
  `SkPath::makeFillType` on an already-built path); both are library-compiled
  and write the field the library reads. `SkPath::setFillType` is a different
  method and is unaffected. The library object is the *smaller* of the two (it
  touches 115 bytes against the header's 424), so this drops writes rather than
  overrunning memory — but treat **any** header-inline `SkPathBuilder` accessor
  that touches a member (`fillType`, `setIsVolatile`, `isEmpty`) as unreliable
  until the bundle's headers and libraries are re-cut from one revision.
  `external/skia-build/VERSION.md` notes the macOS slices come from a separate
  a milestone-specific min-OS re-cut while `include/` is the shared drop, which is the
  likely origin. To re-measure after a Skia bump: construct at one fixed address
  via placement new (two `SkPathBuilder`s at different addresses differ at byte
  0 because `STArray` holds a pointer into its own inline buffer, which
  confounds a naive diff).
- **Skia raster FALLS BACK to CoreGraphics when libs are absent.** In a CPU-only
  build, `render_to_png(..., ScreenshotBackend::skia)` produces **byte-identical
  output to CoreGraphics** (silent fallback). So a "Skia re-render" proves
  nothing on a headers-only checkout, and any widget that relies on a Skia/GPU
  shader (e.g. `Canvas::draw_waveform`'s area fill) shows nothing on CPU — draw
  such fills with raster primitives (`fill_rect`/strokes) if they must render on
  both paths. Confirm Skia is real (`MacGpuWindowHost` present) before trusting a
  `skia`-backend render.
- **Don't mix GPU and CPU build dirs.** Keep `build/` CPU for the deterministic
  CoreGraphics visual-regression goldens (gallery / per-component) and a
  separate `build-gpu/` for GPU work. Reconfiguring `build/` with `SKIA_DIR`
  flips the default screenshot backend and will drift those goldens.
- **Reconfigure after adding a target.** A new example/subdirectory needs
  `cmake -S . -B build-gpu …` again before `--build --target <new>` ("No rule to
  make target").
- **Never launch-then-kill a headless GPU window.** Killing a headless
  Metal/Dawn process wedges the GPU for the rest of the session (see the
  `verify-gpu-ui-via-skia-raster` memory). Verify with `render_to_png(skia)` /
  `--screenshot` and the `MacGpuWindowHost` symbol check; let a human launch the
  live window.
- **First GPU link is slow-ish** (~45s here for the view lib + Skia/Dawn link),
  but subsequent incremental builds are fast.
- **Linux release-path `RawPtrBackupRefImpl` / `PartitionAddressSpace::setup_`
  link failures mean the Skia bundle omitted Chromium PartitionAlloc support.**
  Current milestone Linux archives can reference Chromium BackupRefPtr /
  PartitionAlloc symbols from `libskia.a(libskia.SkSLParser.o)` even though the
  standalone `skia-builder` bundle does not ship a `partition_alloc` archive.
  `FindSkia.cmake` inspects `libskia.a` with `CMAKE_NM`; when those symbols are
  unresolved and no bundled Skia archive defines them, it appends
  `pulp-skia-chromium-raw-ptr-compat` after the Skia archive group. The source is
  `core/canvas/src/skia_chromium_raw_ptr_compat.cpp` in-tree and is installed to
  `src/pulp/canvas/` for SDK consumers. If the failure returns, confirm the
  install-layout regression `cmake-pulp-install-skia-compat-source` passes and
  inspect the release asset with `nm -uC libskia.a | rg
  'RawPtrBackupRefImpl|PartitionAddressSpace::setup_'`.
- **The `external/skia-build/build` symlink loop → Shipyard tree-drift.** This
  path is materialized per-machine (a symlink into the shared
  `~/.cache/pulp/skia-build` cache) and is **untracked + `.gitignore`d as of PR
  #5588** (`4cd76c0f5`). Before that fix it was *tracked* with a machine-specific
  absolute target that formed a two-way self-referential loop across worktrees;
  at CMake configure, `PULP_SKIA_AUTOFETCH` deletes the looped/dangling symlink to
  repopulate, which `shipyard run/pr/ship` sees as `working tree changed during
  shipyard run (stage=configure)` / `D external/skia-build/build` and fails
  validation — so the local macOS lane never posts its required `macos` status and
  the PR stays BLOCKED. Symptoms if you still hit it on a **pre-#5588 checkout**
  (the stale tracked symlink lingers until you pull main + let autofetch
  re-materialize): repoint the PRIMARY checkout's `external/skia-build/build` at a
  real cache — `ln -sfn ~/.cache/pulp/skia-build/build external/skia-build/build` —
  every worktree's symlink chains through the primary, so that one fix resolves all
  of them. `--allow-tree-drift` exists only on `shipyard run` (not `pr`/`ship`), so
  fixing the symlink is the durable answer, not suppressing the guard.

## Emscripten / wasm: the slice is **Ganesh on WebGL2**, not Graphite/Dawn

The one platform where the sentence at the top of this skill ("Pulp's GPU path
is Skia Graphite over Dawn") is **false**. The `wasm-gpu` slice published by
skia-builder contains **zero** `wgpu` symbols and no `libdawn_combined.a`, so
`SK_GRAPHITE` / `SK_DAWN` must NOT be defined there — `FindSkia.cmake`'s
Emscripten arm defines `SK_GANESH` + `SK_GL` instead, and
`tools/scripts/verify_wasm_skia_slice.py` asserts that invariant so a future
slice that quietly changes backends fails the CI lane instead of a demo page.
Everything Ganesh-specific is confined to `core/render/src/skia_surface_ganesh.cpp`;
nothing above the render boundary knows the backend.

Consequences worth knowing before you debug for an hour:

- **WebGL2 has no compute shaders.** There is no GPU-compute path in wasm at
  all. GPU *audio* is not, and cannot be, in the browser on this slice — it
  would need WebGPU (emdawnwebgpu) in a worker. Never describe the browser lane
  as GPU-accelerated DSP; it is a GPU-rendered UI over CPU DSP.
- **No skottie/sksg in the wasm slice** — its `libskottie.a` leaves `skjson::*`
  undefined and the zip ships no jsonreader/skresources archive. `PULP_LOTTIE`
  cannot be enabled for wasm.
- **Emscripten also sets `UNIX=1`.** The `elseif(EMSCRIPTEN)` arm in
  `FindSkia.cmake` MUST precede the `UNIX` arm, or the probe looks for
  `build/linux-gpu/` and reports Skia missing — which, per the top of this
  skill, silently degrades to a CPU-only build rather than erroring. Same
  ordering hazard applies to any new platform arm.

### Landmine: no `SK_TRIVIAL_ABI` → a silently **trapping** link

The wasm slice is built by gn with `is_trivial_abi=true`. That flag changes the
calling convention of `sk_sp<T>` (and friends) — the callee, not the caller,
destroys the argument. If Pulp's TUs compile **without** `SK_TRIVIAL_ABI`, the
two sides disagree about who runs the destructor, and `wasm-ld` does not error:
it links a **trapping stub** for the cross-boundary call. The failure surfaces at
runtime as the first frame dying with a bare, context-free:

```
RuntimeError: unreachable
```

No symbol, no stack, nothing to grep. `SK_TRIVIAL_ABI` is now an INTERFACE
define on the `skia::skia` target for the Emscripten arm. If you ever see a bare
`RuntimeError: unreachable` on the first paint of a wasm build, check that define
**before** you suspect your own code. Any new ABI-affecting gn flag in the slice
needs the same treatment.

### Landmine: `SkFontMgr_New_Custom_Empty` returns a **non-null, glyph-less** fontmgr

Emscripten's font manager is the pathological case for every "do we have fonts?"
guard you would naturally write:

- it is **not null**, so a null-check passes;
- it reports **1 family with 1 face**, so a `countFamilies() > 0` guard passes;
- and that face has **no glyphs**, so every string measures at **zero width**.

The result is text that silently lays out to nothing — no error, no warning, just
an empty UI that looks like a layout bug. **Do not probe font-DB usability by
counting families or null-checking the manager.** Probe it by asking for an
actual glyph:

```cpp
// Usable iff a real typeface maps a real character to a real glyph id.
const bool usable = typeface && typeface->unicharToGlyph('A') != 0;
```

`core/canvas/src/text_font_context.cpp` does this now. The same trap applies to
any host that hands you an "empty" custom fontmgr, not just wasm — bundled fonts
must be registered and then *proven* to draw.

## GPU bundles MUST be relocatable (the libwgpu_native.dylib rpath footgun)

## Start debugging from the installed recipe catalog

Use `pulp gpu recipes list --json` or an exact `--symptom` filter before
choosing a probe. The catalog always explains all four canonical workflows,
while `callable` is derived from the matched native registry. In particular,
Three.js metadata can be visible in a QuickJS release without claiming that
the V8-only recipe can run. `pulp_gpu_recipes` is the read-only MCP equivalent;
use CLI `recipes scaffold` only when an explicit local evidence workspace is
wanted. Run two baselines and the seeded negative control before treating a
pass as useful localization evidence.

The Catch2 native-recipe suite deliberately skips when no real adapter is
available; this keeps GPU-less build lanes viable and is not terminal evidence.
For fail-closed real-work proof, build and run the EXCLUDE_FROM_ALL
`pulp-gpu-probe-native-acceptance` and
`pulp-gpu-probe-stft-native-acceptance` executables on a suitable host, then
validate the digest-bound receipt with `gpu-probe-current-acceptance`. Those
surfaces reject unavailable evidence. There is no compile-time
`PULP_GPU_PROBE_REQUIRE_WORK` policy branch in the portable unit suite.

A GPU plugin/app links `libwgpu_native.dylib`. The upstream WebGPU FetchContent
copies the dylib INTO the bundle's `Contents/MacOS` but rpaths the binary only at
the **build cache** (`~/Library/Caches/Pulp/fetchcontent-src/.../lib`). On the
build machine that path exists, so the build, codesign, notarize, `auval`,
`pluginval`, and even loading in a *local* DAW all PASS — a **false pass**.
Copied to any other Mac (or after the cache is cleared) the dylib isn't found:

- standalone app crashes at launch — `Library not loaded: @rpath/libwgpu_native.dylib`
- AU/VST3/CLAP show no editor / "couldn't load" in the DAW

Pulp's `@loader_path`-adding override only runs on the installed-SDK path, so
**source-built example/plugin bundles do NOT get it automatically.** Fix + guard
every distributable GPU bundle target with `PulpBundleRelocatable.cmake`:

```cmake
include(${CMAKE_SOURCE_DIR}/tools/cmake/PulpBundleRelocatable.cmake)
pulp_make_bundle_relocatable(MyPlugin_CLAP)      # bakes @loader_path (BUILD_WITH_INSTALL_RPATH)
pulp_validate_bundle_relocatable(MyPlugin_CLAP)  # POST_BUILD: FAILS the build if not self-contained
```

`tools/cmake/scripts/check_bundle_relocatable.py <bundle> --strict` is the
standalone validator (reads the Mach-O dependency graph — stronger than the
string-based `check_portable_binary.py`). It rejects every non-system dependency
outside the bundle closure, not only unresolved `@rpath` entries:
`@loader_path` and `@executable_path` must normalize inside the bundle and name
an existing file, while absolute external paths fail. Any copied runtime closure
must also be signed, hash-pinned, and included in the immutable launch snapshot.
Wire the validator into `pulp ship` / CI too.

`pulp_add_plugin(... FORMATS AU)` now adds `@loader_path` to the AU target
without replacing any engine-specific build rpaths, then runs the strict
relocatability validator after linking. Sanitizer builds are the one deliberate
exception: their compiler-injected `libclang_rt.*_dynamic.dylib` remains in the
Xcode toolchain and is accepted only when CMake detects `PULP_SANITIZER` and
passes `--allow-toolchain-runtime`. Do not pass that flag to release/package
validation; the validator's default must continue to reject the same external
runtime.

**Definitive manual proof** that a bundle is self-contained — hide the build
cache and confirm it still loads (this is what a string/auval check can't tell
you):

```bash
CACHE=~/Library/Caches/Pulp/fetchcontent-src/wgpu-macos-aarch64-*/lib/libwgpu_native.dylib
mv "$CACHE" "$CACHE.hidden"
python3 -c "import ctypes; ctypes.CDLL('.../MyPlugin.clap/Contents/MacOS/MyPlugin')"  # loads?
mv "$CACHE.hidden" "$CACHE"
```

`otool -l <binary> | grep -A2 LC_RPATH` should show `@loader_path`, NOT a
`/Users/.../Caches/...` path. Caveat for V8/other-dylib plugins: prefer an
additive `install_name_tool -add_rpath @loader_path` over
`BUILD_WITH_INSTALL_RPATH` (which drops ALL auto build rpaths) — see the note in
`PulpBundleRelocatable.cmake`.

## macOS x86_64 + universal builds (G3)

Pulp builds macOS **arm64, x86_64, or universal (`arm64;x86_64`)**. Select with
`-DCMAKE_OSX_ARCHITECTURES` — the whole toolchain keys off the **TARGET** arch,
never the host `CMAKE_SYSTEM_PROCESSOR` (which reports the build machine and is
wrong for a cross/Intel build on Apple Silicon).

- **Three Skia slices, per-slice caches.** skia-builder publishes `mac-arm64`,
  `mac-x86_64`, and `mac-universal` (all three pinned in
  `tools/deps/manifest.json` → Skia `release_assets`; the old
  `PulpDependencies.cmake` comment "the only published mac asset" was false).
  Each slice's `libskia.a` flattens to the SAME path
  (`build/mac-gpu/lib/Release/libskia.a`), so the autofetch uses **per-slice
  immutable cache generations** under the configured cache root, keyed as
  `<matrix-platform>-<asset-sha256>-<receipt-schema>` — a mutable shared directory would
  silently reuse a wrong-arch archive after an arch switch.
  `fetch_skia_for_release.py` matrix keys: `darwin-arm64` / `darwin-x64` /
  `darwin-universal`. Legacy `~/.cache/pulp/skia-build*` directories are not
  valid inputs unless repopulated through the current fetcher into a stamped,
  receipt-bound generation.
- **FindSkia fails LOUD on an arch mismatch.** If `SKIA_DIR` points at the wrong
  slice, `FindSkia.cmake` FATALs at configure time (`lipo -archs` vs
  `CMAKE_OSX_ARCHITECTURES`) with "architecture mismatch … missing: <arch>" —
  one actionable error instead of a wall of ld64 "building for macOS-x86_64 but
  linking arm64" warnings + hundreds of undefined symbols.
- **FindSkia also fails LOUD when an archive requires a newer macOS than the
  consumer target.** A matching architecture is insufficient: ld64 only warns
  when an object inside `libskia.a` or `libdawn_combined.a` carries a newer
  `LC_BUILD_VERSION`, leaving a deceptively successful bundle. Configure now
  runs `pulp_assert_macos_archive_floor` over every archive member and rejects
  a floor above `CMAKE_OSX_DEPLOYMENT_TARGET`. If this fires, select/rebuild the
  correct published slice or raise the product floor deliberately; do not
  suppress the check or trust the link warning. The focused regression is
  `test/cmake/test_macos_archive_floor.cmake`.
- **wgpu-native has NO universal dylib.** For a universal build, Pulp fetches
  BOTH pinned mac wgpu zips, `lipo -create`s them, and — **REQUIRED** —
  `codesign -f -s -` re-signs the fat dylib (`PulpWgpuUniversal.cmake`, wired
  from `PulpDependencies.cmake`). The re-sign is not optional: a raw `lipo`
  output fails `codesign --verify` ("code object is not signed at all") because
  the per-slice adhoc linker signatures don't merge, and an unsigned arm64
  dylib is **killed at load**. `LC_ID_DYLIB` is already `@rpath/…` and the dylib
  has no `LC_RPATH`, so the `@loader_path` relocatable contract above still holds.
- **No universal libv8.** `PULP_JS_ENGINE=v8` + a universal target is a hard
  `FindV8.cmake` FATAL (v8-builder ships only thin mac slices). Use quickjs/jsc
  for universal, or lipo two thin V8 builds yourself.
- **Ship the arch gate on distributable bundles.** Alongside
  `pulp_validate_bundle_relocatable`, add
  `pulp_validate_bundle_architectures(<target> [ARCHS "arm64;x86_64"])`
  (`PulpBundleRelocatable.cmake`). POST_BUILD it asserts `lipo -archs` == the
  requested set AND `codesign --verify` on the main binary AND **every embedded
  dylib** (`libwgpu_native.dylib`, `libv8.dylib`) — catching a thin embedded
  dylib in an otherwise-universal bundle (crashes on the missing arch) and an
  unsigned fat dylib. Standalone validator:
  `tools/scripts/check_bundle_architectures.py <bundle> --archs arm64,x86_64 --strict`.
- **Min-OS floor is the MAX across requested arches** (`PulpMinOs.cmake`, no
  longer hardcoded to `macos-arm64`). Both arm64 and x86_64 slices stamp
  Skia/Dawn minos 13.0; the real cross-SDK floor is libc++ 13.4 (the macOS 15.4
  SDK gates the floating-point `std::to_chars` overloads reached by
  `std::format`), arch-independent — so arm64, x86_64, and universal all pin
  **13.4**
  (`tools/deps/min_os.json` `macos-arm64` + `macos-x64`, MEASURED with
  `measure_min_os.py --measure`, never hardcoded).
- **Ships `experimental`.** GitHub VMs have no representative Intel GPU and
  Rosetta caps SIMD at SSE4.2/AVX2, so Metal-on-Intel-GPU and AVX3 dispatch are
  unverified until a real Intel smoke — the support-matrix `experimental` note.

## Embedding Pulp as a submodule (standalone plugin repos)

When Pulp is consumed via `add_subdirectory(pulp)` from another repo (a
standalone plugin like pulp-gpu-nam that pins Pulp as a git submodule),
`CMAKE_SOURCE_DIR` is the **consumer's** root, not Pulp's. Anything that resolves
Pulp-relative paths off `CMAKE_SOURCE_DIR` breaks — including `FindSkia.cmake`'s
`external/skia-build` autodiscovery, which would look under the consumer repo and
silently fall back to no-Skia (CPU-only host, no GPU). Pulp now keys these off
`PULP_ROOT_DIR` (a `CACHE INTERNAL` set to Pulp's own source dir in the root
`CMakeLists.txt`) so submodule builds find the prebuilt Skia libs. If a submodule
GPU build comes out CPU-only, confirm `PULP_ROOT_DIR` points at the Pulp checkout
and that `external/skia-build/*-gpu/lib/Release` (or `SKIA_DIR` env) is populated
there — a headers-only submodule checkout hits the same locked-raster trap as the
in-tree case above.

## Lottie / skottie in the bundle

The Skia bundle links `skottie` + `sksg` + `svg` via `FindSkia.cmake`'s glob, but
skottie also needs `SkJSON` (`libjsonreader.a`) + `skresources` to actually link.
Bundles before Skia chrome/m151 shipped skottie's headers/archive but omitted
those, so the opt-in `PULP_LOTTIE` try-link in `core/canvas/CMakeLists.txt`
auto-disabled (LottieView degraded to a no-op). **chrome/m151 onward ships
`libjsonreader.a` (SkJSON) + `libskresources.a`**, so the try-link now succeeds
and `LottieAnimation` composites real frames. `PULP_LOTTIE` stays default OFF
(it retains skottie in the binary); the macOS CI lane builds it ON for coverage
(`test_canvas.cpp` pixel-tests the render, `test_lottie_view.cpp` the playback).
If Lottie silently no-ops, check the configure line for
`Pulp: PULP_LOTTIE requested but this Skia bundle cannot link skottie` — that
means a pre-m151 (or otherwise skjson-less) bundle.

## When to reach for this

Any time GPU rendering "isn't working", a window looks CPU-ish (no aspect-lock,
dark fill past the design surface), a `skia`-backend screenshot looks identical
to CoreGraphics, you need a live GPU window of native UI (e.g. the
`ink-signal-showcase` / `gpu-demo` examples), or a GPU plugin/app loads on the
build machine but crashes / shows no UI on another Mac (the dylib rpath footgun
above).

## A3 first-visible evidence

GPU startup health has two independent axes. `dropped_event_count` and
`truncated` describe capture integrity; `missing_trace_categories` describes
instrumentation coverage. Never turn a missing Vellum event into loss or a
zero-valued duration. Visible Standalone/DAW/Forge campaigns bind
`native-compositor-presentation`; only the constrained headless campaign binds
`headless-capture-complete`. The closed A3 verifier permits nullable causal
fields only for passing no-change or a budget-miss investigation with every
missing event, argument, interval, and transferred route named.

Run real roles through `tools/scripts/gpu_first_visible_a3_campaign.py
run-role`. Its adapter request fixes the role endpoint and requires 10 cold plus
10 warm trials with explicit lifecycle, process, and cache provenance; it never
derives cold/warm from elapsed time. The runner snapshots the adapter and
ratified budget, validates product/host/health/trace artifacts, and preserves
timeout, SKIP, or INCONCLUSIVE as nonterminal. Use `--require-controls` on one
real role to bind the blank negative and external audio-thread exclusion. The
existing Standalone product test remains a one-observation wiring preflight,
not a 20-trial campaign. When the adapter owns the controls, use the two
receipt-producing focused test invocations in
`docs/validation/gpu-first-visible-a3-acceptance.md`; setting a seed alone is
not durable evidence.

The checked-in `gpu_first_visible_a3_external_adapter.py` is the reusable
Pulp-owned envelope for those roles. Configure the exact role producer with
`PULP_A3_CAMPAIGN_PRODUCER`; for the one controls run, also configure the two
focused built test binaries documented in the acceptance guide. The envelope
pins all three executables and fails closed on missing configuration or
protocol drift. It does not provide generic present/cache instrumentation and
must not be used to relabel capture completion as native presentation.
Select the checked-in standalone, constrained-headless, REAPER, or Forge role
producer rather than an undocumented producer path. The adapter pins the role
entry point and its checked-in support from the exact Pulp source root. Each
role entry point pins the configured product, host, lifecycle driver,
source-build driver, sealed source-bound trace analyzer, embedded-build
verifier, and supplemental build attestation/receipt. The source-build driver
receives clean exact-revision roots and a fresh output directory, never the
measured product path; rebuilt executable and DAW/Forge bundle digests must
match the measurement. Every lifecycle row answers a producer nonce while the
exact host is alive, named replay selects that challenged trace-host PID, and
all reported processes must then be gone before replay and PASS. The lifecycle
driver remains an explicit
external prerequisite wherever the current Dawn/AppKit/host seam cannot expose
native presentation without Vellum-owned work.

Terminal A3 also requires the four-state product trace-producer overhead control
from the acceptance guide. Compare the exact pre-producer parent with final-head
compile-out, compiled-in idle, and active 128 MiB capture using one host,
workload, build family, and source-bound driver. The compiled-in idle and active
states use identical executable bytes; every state requires zero xruns and
audio-thread trace events. A passing role campaign or offline A2T no-producer
classification does not waive this control.

Use the acceptance guide's `collect-state` command for every row. It wraps the
source-bound product driver with 55 independent live-process challenges, keeps
compiled-in idle and active executable bytes identical, and replays active
binary Perfetto traces with the exact pinned processor. Acceptance binds both
the health-first-visible producer and the `b4ba22…` macOS
input/acquire/submit/present package; raw driver JSON or Chrome trace cannot
stand in for the collector receipt.

If a validated B4 disposition routes follow-up to Vellum, test a bounded
Graphite `PipelineManager` `SkExecutor` supplied through `ContextOptions` before
designing custom prewarm. Trace pipeline queued/start/end, cache hit/miss,
signature, and render wait over the exact 10-cold/10-warm workload; keep work
off the audio thread and bound executor ownership, lifetime, and shutdown. Ship
only a causal, material improvement, otherwise record `no-change`. A generic
Vellum-installed `SkLogHandler` is a later diagnostic producer, not Horizon A.
