# Dependencies

All code that Pulp bundles, fetches automatically, exports via `cmake --install`, or redistributes in public releases must be compatible with MIT licensing. No copyleft (GPL, LGPL, AGPL) or proprietary code may be committed, vendored, auto-downloaded into the source tree, or shipped as part of Pulp.

## Policy

- **Allowed for bundled or redistributed code:** MIT, BSD-2-Clause, BSD-3-Clause, Apache-2.0, ISC, zlib, BSL-1.0, Unlicense, public domain
- **Not allowed in the repo or shipped artifacts:** GPL, LGPL, AGPL, SSPL, proprietary, any copyleft
- **Review required:** MPL-2.0 (weak copyleft, case-by-case evaluation)
- **Optional vendor SDK carve-out:** AAX, ASIO, and similar SDKs may be supported only when they are off by default, separately obtained by the developer, kept outside the source tree, never committed, never exported by `cmake --install`, omitted from `NOTICE.md`, and not required by public CI.

## Current Dependencies

| Name | Version | License | How Used | Subsystem | Added |
|------|---------|---------|----------|-----------|-------|
| AudioUnitSDK | HEAD | Apache-2.0 | AU v2 plugin format adapter (AUEffectBase, factory) | pulp-format | 2026-03-24 |
| Catch2 | 3.7.1 | BSL-1.0 | Unit testing framework | test | 2026-03-24 |
| CHOC | f0f5cdf5a938 | ISC | JS engine abstraction, MIDI utilities, audio helpers | multiple | 2026-03-24 |
| CLAP | 1.2.2 | MIT | CLAP plugin format headers | pulp-format | 2026-03-24 |
| cpp-httplib | master | MIT | HTTP client (GET/POST/download) | pulp-runtime | 2026-04-07 |
| dr_libs | master | Public domain | FLAC, MP3, WAV decode (dr_flac, dr_mp3, dr_wav) | pulp-audio | 2026-04-07 |
| Dawn | chrome/m144 | BSD-3-Clause | WebGPU implementation used by the GPU render path (via pre-built Skia toolchain) | pulp-render | 2026-03-30 |
| Highway | 1.2.0 | Apache-2.0 | Portable SIMD abstraction (SSE/NEON/AVX) | pulp-runtime | 2026-04-06 |
| LV2 | 1.18.10 | ISC | LV2 plugin format headers | pulp-format | 2026-03-30 |
| miniz | 3.0.2 | MIT | ZIP/GZIP compression | pulp-runtime | 2026-04-07 |
| msdfgen | 1.12 | MIT | Multi-channel SDF glyph generation (planned, FetchContent) | pulp-canvas | 2026-04-12 |
| nanosvg | master | zlib | SVG parsing and rasterization | pulp-canvas | 2026-03-25 |
| Oboe | 1.9.0 | Apache-2.0 | Android audio backend (AAudio/OpenSL ES abstraction) | pulp-audio | 2026-04-07 |
| pugixml | 1.14 | MIT | XML parsing and generation | pulp-runtime | 2026-04-07 |
| node-addon-api | 8.x | MIT | Node.js bindings via Node-API (optional, npm install) | bindings/nodejs | 2026-03-25 |
| pybind11 | 2.13.6 | BSD-3-Clause | Python bindings for HeadlessHost (optional, FetchContent) | bindings/python | 2026-03-25 |
| SDL3 | 3.2.12 | zlib | Cross-platform windowing, input, GPU context | pulp-view | 2026-03-25 |
| Skia | chrome/m144 | BSD-3-Clause | GPU 2D rendering engine (pre-built via skia-builder) | pulp-canvas, pulp-render | 2026-03-25 |
| VST3 SDK | v3.7.12_build_20 | MIT | VST3 plugin format (pluginterfaces + base) | pulp-format | 2026-03-24 |
| WebGPU-distribution | 17dcd42a7683 | MIT | WebGPU C API wrapper for Dawn (FetchContent) | pulp-render | 2026-03-25 |
| Yoga | 3.2.1 | MIT | CSS Flexbox/Grid layout engine (Meta, FetchContent) | pulp-view | 2026-03-29 |

## Open-Source Format SDKs Pulp Can Bootstrap

These SDKs are permissively licensed and may be cloned, fetched, or installed as part of normal Pulp workflows:

| SDK | License | Required For | Bundled? |
|-----|---------|-------------|----------|
| VST3 SDK | MIT | VST3 format | Cloned at configure time (`git clone --depth 1`) |
| AudioUnit SDK | Apache-2.0 | AU v2 format | Cloned at configure time (`git clone --depth 1`) |
| CLAP | MIT | CLAP format | FetchContent (automatic) |
| LV2 SDK | ISC | LV2 format | FetchContent (automatic) |

## Optional Developer-Supplied Vendor SDKs

These SDKs are not part of Pulp's redistributed dependency chain. Pulp may integrate with them only through explicit opt-in local configuration.

| SDK | License | Used For | Pulp Boundary |
|-----|---------|----------|---------------|
| AAX SDK | Separately licensed by Avid | Optional AAX format integration | Developer obtains independently, keeps it out-of-tree, and points `PULP_AAX_SDK_DIR` at it |
| ARA SDK | MIT-compatible (Celemony) | Optional ARA 2.x integration (pitch correction, spectral editing, clip-aware workflows) | Developer obtains independently (https://github.com/Celemony/ARA_SDK), keeps it out-of-tree, points `PULP_ARA_SDK_DIR` at it, and sets `PULP_ENABLE_ARA=ON`. Never bundled. |
| ASIO SDK | Proprietary (Steinberg) | ASIO audio I/O (planned) | Developer obtains independently; never bundled or exported by Pulp |
