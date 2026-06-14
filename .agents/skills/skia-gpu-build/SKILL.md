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

`tools/build-skia.sh` builds the chrome/m150 Skia+Dawn from source — slow
(tens of minutes). Prefer the release zip from
`danielraffel/skia-builder` (chrome/m150, see `external/skia-build/VERSION.md`)
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

## When to reach for this

Any time GPU rendering "isn't working", a window looks CPU-ish (no aspect-lock,
dark fill past the design surface), a `skia`-backend screenshot looks identical
to CoreGraphics, or you need a live GPU window of native UI (e.g. the
`ink-signal-showcase` / `gpu-demo` examples).
