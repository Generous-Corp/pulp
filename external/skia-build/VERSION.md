# Skia Pre-built Binaries

## Source
- **Repository:** https://github.com/olilarkin/skia-builder
- **Release:** chrome/m144
- **Release URL:** https://github.com/olilarkin/skia-builder/releases/tag/chrome%2Fm144
- **Downloaded:** 2026-03-25
- **Skia branch:** chrome/m144 (Skia Graphite + Dawn)
- **Skia commit:** `cd0c5f445516ea4e90e02b5f634cbc5ca23b5a44`
- **skia-builder ref:** `7eecb8abf1f77b2a8bac2e81c38e20708cb79c24`

## Bundled Text and GPU Pins

These revisions are read from Skia's `DEPS` file at the Skia commit above:

| Dependency | Repository | Commit |
|------------|------------|--------|
| Dawn | `https://dawn.googlesource.com/dawn.git` | `6acf6ef3fe237cd4be7b825389602c93a1f16f2f` |
| HarfBuzz | `https://chromium.googlesource.com/external/github.com/harfbuzz/harfbuzz.git` | `08b52ae2e44931eef163dbad71697f911fadc323` |
| ICU | `https://chromium.googlesource.com/chromium/deps/icu.git` | `364118a1d9da24bb5b770ac3d762ac144d6da5a4` |

The B.0 visual harness uses `skia-python==144.0.post2` for the optional
Python SkPicture smoke test when the C++ static libraries are not installed in
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

| Directory | Platform | Architectures | Size |
|-----------|----------|--------------|------|
| `mac-gpu/` | macOS | universal (arm64 + x86_64) | ~143 MB |
| `win-gpu/` | Windows | x64 | ~39 MB |
| `linux-gpu/` | Linux | x64 | ~39 MB |
| `ios-gpu/` | iOS device + simulator | arm64, arm64+x86_64 | ~104 MB |
| `wasm-gpu/` | WebAssembly | wasm32 | ~20 MB |

## To Update

1. Check for new releases: https://github.com/olilarkin/skia-builder/releases
2. Download the new platform zips
3. Verify each zip SHA-256 against the release asset digest
4. Extract to this directory (replacing `build/`)
5. Update this VERSION.md with the new release tag, Skia commit,
   skia-builder ref, Dawn/HarfBuzz/ICU DEPS pins, and asset digests
6. Regenerate any PNG goldens if the Skia release, bundled text pins, font
   files, sampler settings, color type, alpha type, DPI, or backend changes
7. Commit via Git LFS

Or run: `./tools/build-skia.sh <platform>` to build from source.

## Release Asset Digests

| Asset | SHA-256 |
|-------|---------|
| `skia-build-linux-x64-gpu-release.zip` | `bde9977efb2c796e22a71b0086e2b15961b0ccc7dc4de8559b369229e922ce34` |
| `skia-build-mac-arm64-gpu-release.zip` | `d495bc4b77fba29e055d9275852401d02998dd5fda0e628050fb72c4c9fce87a` |
| `skia-build-mac-universal-gpu-release.zip` | `7990695504abf0e4b5b05d42616812845d9e72d41b243f347f261ab5fe8cb478` |

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
