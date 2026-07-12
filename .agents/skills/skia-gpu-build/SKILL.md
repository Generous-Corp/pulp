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

## Fastest fix: reuse another checkout's cached libs via SKIA_DIR

Don't rebuild Skia (slow). Point `SKIA_DIR` at a checkout that already has the
compiled `mac-gpu` libs — usually the **primary checkout**:

```bash
ls /Users/<you>/Code/pulp/external/skia-build/build/mac-gpu/lib/Release/libskia.a   # confirm it exists

# Configure a SEPARATE GPU build dir (keep the CPU build/ for deterministic
# CoreGraphics goldens — see "Don't mix" below).
cmake -S . -B build-gpu -DCMAKE_BUILD_TYPE=Release \
  -DSKIA_DIR=/Users/<you>/Code/pulp/external/skia-build \
  -DPULP_ENABLE_GPU=ON
```

`SKIA_DIR` may be an env var or a `-D` cache entry; FindSkia also auto-discovers
`external/skia-build` and `$SKIA_DIR`. SKIA_DIR must point at the dir that
*contains* `build/<platform>-gpu/lib/Release` + `build/include` (skia-builder
layout); flat `mac/lib` + `include` layouts are also accepted (see
`tools/cmake/FindSkia.cmake`).

A SKIA_DIR you point cross-checkout uses that checkout's headers **and** libs
together (`build/include` + `build/<platform>-gpu/lib/Release`) — self-
consistent. Don't mix this worktree's headers with another's libs.

## Building from source (only when no cached libs anywhere)

`tools/build-skia.sh` builds the chrome/m151 Skia+Dawn from source — slow
(tens of minutes). Prefer the release zip from
`danielraffel/skia-builder` (chrome/m151, see `external/skia-build/VERSION.md`)
or reusing a sibling checkout's `build/`.

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

## Gotchas (each cost real time)

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
  The chrome/m151 Linux archive can reference Chromium BackupRefPtr /
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
standalone validator (reads Mach-O rpaths + `@rpath` deps — stronger than the
string-based `check_portable_binary.py`). Wire it into `pulp ship` / CI too.

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
  cache dirs** `~/.cache/pulp/skia-build{,-x64,-universal}` — one shared dir
  would silently reuse a wrong-arch archive after an arch switch. `fetch_skia_for_release.py`
  matrix keys: `darwin-arm64` / `darwin-x64` / `darwin-universal`.
- **FindSkia fails LOUD on an arch mismatch.** If `SKIA_DIR` points at the wrong
  slice, `FindSkia.cmake` FATALs at configure time (`lipo -archs` vs
  `CMAKE_OSX_ARCHITECTURES`) with "architecture mismatch … missing: <arch>" —
  one actionable error instead of a wall of ld64 "building for macOS-x86_64 but
  linking arm64" warnings + hundreds of undefined symbols.
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
  Skia/Dawn minos 13.0; the real floor is libc++ 13.3 (`std::to_chars(float)`),
  arch-independent — so arm64, x86_64, and universal all pin **13.3**
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
