# Dependencies

All code that Pulp bundles, fetches automatically, exports via `cmake --install`, or redistributes in public releases must be compatible with MIT licensing. No copyleft (GPL, LGPL, AGPL) or proprietary code may be committed, vendored, auto-downloaded into the source tree, or shipped as part of Pulp.

## Policy

- **Allowed for bundled or redistributed code:** MIT, BSD-2-Clause, BSD-3-Clause, Apache-2.0, ISC, zlib, BSL-1.0, Unlicense, public domain, SIL OFL 1.1 (fonts)
- **Not allowed in the repo or shipped artifacts:** GPL, LGPL, AGPL, SSPL, proprietary, any copyleft
- **Review required:** MPL-2.0 (weak copyleft, case-by-case evaluation)
- **Optional vendor SDK carve-out:** AAX, ASIO, and similar SDKs may be supported only when they are off by default, separately obtained by the developer, kept outside the source tree, never committed, never exported by `cmake --install`, omitted from `NOTICE.md`, and not required by public CI.

## Current Dependencies

Entries are sorted alphabetically (case-insensitive) by name.

| Name | Version | License | How Used | Subsystem | Added |
|------|---------|---------|----------|-----------|-------|
| AudioUnitSDK | HEAD | Apache-2.0 | AU v2 plugin format adapter (AUEffectBase, factory) | pulp-format | 2026-03-24 |
| Catch2 | 3.7.1 | BSL-1.0 | Unit testing framework | test | 2026-03-24 |
| CHOC | f0f5cdf5a938 | ISC | JS engine abstraction, MIDI utilities, audio helpers | multiple | 2026-03-24 |
| CLAP | 1.2.2 | MIT | CLAP plugin format headers | pulp-format | 2026-03-24 |
| cpp-httplib | vendored-snapshot | MIT | HTTP client (GET/POST/download) | pulp-runtime | 2026-04-07 |
| Dawn | chrome/m149 deps (bundled in Skia toolchain) | BSD-3-Clause | WebGPU implementation used by the GPU render path (via pre-built Skia toolchain) | pulp-render | 2026-05-23 |
| DRACO | 1.5.7 | Apache-2.0 | Optional glTF mesh decompression; fetched via FetchContent only when `PULP_ENABLE_DRACO=ON` (default OFF) | pulp-render | 2026-04-21 |
| dr_libs | vendored-snapshot | Public domain (Unlicense) / MIT-0 | FLAC, MP3, WAV decode (dr_flac, dr_mp3, dr_wav) | pulp-audio | 2026-04-07 |
| Highway | 1.2.0 | Apache-2.0 | Portable SIMD abstraction (SSE/NEON/AVX) | pulp-runtime | 2026-04-06 |
| Inter | 4.001;git-9221beed3 | SIL OFL 1.1 | Embedded UI font (Inter-Regular.ttf, SHA-256 `40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82`) | pulp-view | 2026-04-21 |
| JetBrains Mono | 2.304 | SIL OFL 1.1 | Embedded monospace font (JetBrainsMono-Regular.ttf, SHA-256 `a0bf60ef0f83c5ed4d7a75d45838548b1f6873372dfac88f71804491898d138f`) | pulp-view | 2026-04-21 |
| lcov_cobertura | 2.1.2 | Apache-2.0 | LCOV→Cobertura converter; vendored as `tools/scripts/lcov_cobertura.py` for the coverage pipeline (single-file Python script, no pip dependency in CI) | tooling | 2026-04-21 |
| LV2 | 1.18.10 | ISC | LV2 plugin format headers | pulp-format | 2026-03-30 |
| Material Design Icons | bundled-with-mkdocs-material | Apache-2.0 | Icon set bundled inside mkdocs-material and redistributed as SVG assets in the generated docs site | docs | 2026-04-22 |
| Mbed TLS | 3.6.2 | Apache-2.0 | Cryptographic primitives (SHA-256, RSA, AES) | pulp-runtime | 2026-04-21 |
| miniz | 3.0.2 | MIT | ZIP/GZIP compression | pulp-runtime | 2026-04-07 |
| mkdocs | transitive | BSD-2-Clause | Static site generator (transitive via mkdocs-material); theme assets (search JS, sitemap) redistributed in the generated docs site | docs | 2026-04-22 |
| mkdocs-awesome-pages-plugin | >=2.9,<3 | MIT | MkDocs plugin for nav ordering; build-time only | docs | 2026-04-22 |
| mkdocs-git-revision-date-localized-plugin | >=1.2,<2 | MIT | MkDocs plugin for git-revision dates; build-time only | docs | 2026-04-22 |
| mkdocs-material | >=9.5,<10 | MIT | MkDocs Material theme. CSS, JS, SVG icons, and search worker from this theme are redistributed in the generated docs site (generouscorp.com/pulp/) | docs | 2026-04-22 |
| msdfgen | 1.12 | MIT | Multi-channel SDF glyph generation (planned, FetchContent) | pulp-canvas | 2026-04-12 |
| nanosvg | vendored-snapshot | zlib | SVG parsing and rasterization | pulp-canvas | 2026-03-25 |
| node-addon-api | 8.x | MIT | Node.js bindings via Node-API (optional, npm install) | bindings/nodejs | 2026-03-25 |
| Noto Color Emoji | noto-emoji main @ 2026-05-17 | SIL OFL 1.1 | Embedded color-emoji typeface (NotoColorEmoji.ttf, SHA-256 `0ae57fe58645638523ba35f388d93739d292539a9acb84df5700c81b1e1a28d2`). Bundled only when `PULP_BUNDLE_NOTO_COLOR_EMOJI=ON` (defaults ON for Linux/Android/headless, OFF on macOS/Windows where the platform color-emoji typeface is preferred). Provides the cross-platform fallback for emoji clusters in Canvas2D text. | pulp-canvas | 2026-05-17 |
| Oboe | 1.9.0 | Apache-2.0 | Android audio backend (AAudio/OpenSL ES abstraction) | pulp-audio | 2026-04-07 |
| pugixml | 1.14 | MIT | XML parsing and generation | pulp-runtime | 2026-04-07 |
| pybind11 | 2.13.6 | BSD-3-Clause | Python bindings for HeadlessHost (optional, FetchContent) | bindings/python | 2026-03-25 |
| Pygments | transitive | BSD-2-Clause | Syntax highlighter used by mkdocs + pymdownx.highlight; redistributed as inline HTML/CSS in the generated docs site | docs | 2026-04-22 |
| pymdown-extensions | >=10.7,<11 | MIT | Markdown extension bundle used by mkdocs-material (admonitions, tabs, superfences, highlight, emoji) | docs | 2026-04-22 |
| react | ^18.2.0 | MIT | Peer dependency of `@pulp/react` (packages/pulp-react); plugin authors install it themselves alongside the package — never bundled into Pulp itself | packages/pulp-react | 2026-04-29 |
| react-reconciler | ^0.29.2 | MIT | Reconciler runtime that `@pulp/react` (packages/pulp-react) wraps to drive `pulp::view::WidgetBridge`; npm-installed, not bundled into the C++ tree | packages/pulp-react | 2026-04-29 |
| scheduler | ^0.23.2 | MIT | Cooperative-scheduling runtime pulled in transitively by react-reconciler for `@pulp/react`; npm-installed, not bundled into the C++ tree | packages/pulp-react | 2026-04-29 |
| SDL3 | 3.2.12 | zlib | Cross-platform windowing, input, GPU context | pulp-view | 2026-03-25 |
| Skia | chrome/m149 | BSD-3-Clause | GPU 2D rendering engine. Pre-built via [danielraffel/skia-builder](https://github.com/danielraffel/skia-builder) fork (adds iOS device/simulator, visionOS, mac-x86_64, and `Skia.xcframework` slices that upstream `olilarkin/skia-builder` doesn't ship). The fork tracks Skia's `chrome/m149` tag and is the active dependency until upstream publishes those slices. Call sites use the m149-era `SkShaders::*` gradient namespace + the new 3-arg `ParagraphBuilder::make(..., SkUnicode)` signature. | pulp-canvas, pulp-render | 2026-05-23 |
| three.js | 077dd13c0e86 | MIT | Native WebGPU bridge demos and tests (optional, fetched only when `PULP_BUILD_TESTS` and `PULP_ENABLE_GPU` are ON) | pulp-render | 2026-04-21 |
| TweetNaCl | 20140427 | Public domain | Ed25519 sign/verify per RFC 8032 — Sparkle appcast signatures, future v2 license-key payloads | pulp-runtime | 2026-05-25 |
| VST3 SDK | v3.7.12_build_20 | MIT | VST3 plugin format (pluginterfaces + base) | pulp-format | 2026-03-24 |
| WebGPU-distribution | 17dcd42a7683 | MIT | WebGPU C API wrapper for Dawn (FetchContent) | pulp-render | 2026-03-25 |
| yaml-cpp | 0.8.0 | MIT | YAML frontmatter parser for the DESIGN.md import source (FetchContent) | pulp-view | 2026-05-13 |
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
