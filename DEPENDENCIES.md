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
| Babel parser | 7.29.7 | MIT | JSX/TSX source-contract extraction for `tools/import-design/jsx-runtime/jsx-contract-audit.mjs`; npm-installed for import tooling, not bundled into the C++ runtime | import tooling | 2026-05-27 |
| Catch2 | 3.7.1 | BSL-1.0 | Unit testing framework | test | 2026-03-24 |
| CHOC | pulp-webview-dnd-poc1 (fork of f0f5cdf5a938) | ISC | JS engine abstraction, MIDI utilities, audio helpers, WebView drag-and-drop | multiple | 2026-03-24 |
| CLAP | 1.2.2 | MIT | CLAP plugin format headers | pulp-format | 2026-03-24 |
| cpp-httplib | vendored-snapshot | MIT | HTTP client (GET/POST/download) | pulp-runtime | 2026-04-07 |
| css-tree | 3.2.1 | MIT | CSS value parsing and lexer validation for JSX source-contract extraction; npm-installed for import tooling, not bundled into the C++ runtime | import tooling | 2026-05-27 |
| Dawn | chrome/m150 deps (bundled in Skia toolchain) | BSD-3-Clause | WebGPU implementation used by the GPU render path (via pre-built Skia toolchain) | pulp-render | 2026-06-05 |
| DRACO | 1.5.7 | Apache-2.0 | Optional glTF mesh decompression; fetched via FetchContent only when `PULP_ENABLE_DRACO=ON` (default OFF) | pulp-render | 2026-04-21 |
| dr_libs | vendored-snapshot | Public domain (Unlicense) / MIT-0 | FLAC, MP3, WAV decode (dr_flac, dr_mp3, dr_wav) | pulp-audio | 2026-04-07 |
| fastgltf | v0.9.0 | MIT | Optional native no-JS glTF/GLB parser; fetched only when `PULP_ENABLE_SCENE3D=ON` | pulp-scene | 2026-06-03 |
| Highway | 1.2.0 | Apache-2.0 | Portable SIMD abstraction (SSE/NEON/AVX) | pulp-runtime | 2026-04-06 |
| Inter | 4.001;git-9221beed3 | SIL OFL 1.1 | Embedded UI font (Inter-Regular.ttf, SHA-256 `40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82`) | pulp-view | 2026-04-21 |
| JetBrains Mono | 2.304 | SIL OFL 1.1 | Embedded monospace font (JetBrainsMono-Regular.ttf, SHA-256 `a0bf60ef0f83c5ed4d7a75d45838548b1f6873372dfac88f71804491898d138f`) | pulp-view | 2026-04-21 |
| Khronos Box Textured fixture | b510eca2e2ef33f62f9ed57d6e7ce2d10ebb2bdebc4a8e59d347719ba81abdf4 | LicenseRef-CC-BY-TM + LicenseRef-LegalMark-Cesium | Official glTF Sample Assets `BoxTextured.glb` redistributed as a Scene3D native loader/render test fixture | test fixture | 2026-06-03 |
| lcov_cobertura | 2.1.2 | Apache-2.0 | LCOV→Cobertura converter; vendored as `tools/scripts/lcov_cobertura.py` for the coverage pipeline (single-file Python script, no pip dependency in CI) | tooling | 2026-04-21 |
| Lucide | 0.x (Ink & Signal reference set) | ISC | Line-style icon SVGs under `assets/design-system/ink-signal/icons/`. Generic glyphs (chevrons, check, clock, copy, eye, …) are Lucide-derived; the audio glyphs (compressor, delay, distortion, eq, filter, fader, envelope, reverb, …) are original Pulp work. Design-system reference assets only — not compiled into or shipped in plugin/app binaries. | design-system | 2026-06-13 |
| LV2 | 1.18.10 | ISC | LV2 plugin format headers | pulp-format | 2026-03-30 |
| Material Design Icons | bundled-with-mkdocs-material | Apache-2.0 | Icon set bundled inside mkdocs-material and redistributed as SVG assets in the generated docs site | docs | 2026-04-22 |
| Mbed TLS | 3.6.2 | Apache-2.0 | Cryptographic primitives (SHA-256, RSA, AES) | pulp-runtime | 2026-04-21 |
| miniz | 3.0.2 | MIT | ZIP/GZIP compression | pulp-runtime | 2026-04-07 |
| mkdocs | transitive | BSD-2-Clause | Static site generator (transitive via mkdocs-material); theme assets (search JS, sitemap) redistributed in the generated docs site | docs | 2026-04-22 |
| mkdocs-awesome-pages-plugin | >=2.9,<3 | MIT | MkDocs plugin for nav ordering; build-time only | docs | 2026-04-22 |
| mkdocs-git-revision-date-localized-plugin | >=1.2,<2 | MIT | MkDocs plugin for git-revision dates; build-time only | docs | 2026-04-22 |
| mkdocs-material | >=9.5,<10 | MIT | MkDocs Material theme. CSS, JS, SVG icons, and search worker from this theme are redistributed in the generated docs site (generouscorp.com/pulp/) | docs | 2026-04-22 |
| Moonbase | 3.3.0 | MIT | License activation / copy protection (optional, opt-in). Header-only `moonbase-cpp` SDK fetched via FetchContent only when a developer runs `pulp add moonbase`; exports target `moonbase::licensing`. Not part of the default dependency chain; consumed by `examples/moonbase-activation/` | examples/moonbase-activation | 2026-06-30 |
| msdfgen | 1.12 | MIT | Reserved for multi-channel SDF glyph generation (FetchContent) | pulp-canvas | 2026-04-12 |
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
| SheenBidi | v3.0.0 | Apache-2.0 | Unicode Bidirectional Algorithm engine. Backs `core/canvas::BidiAnalyzer` for Arabic / Hebrew / mixed-direction text — provides a Pulp-owned bidi pass that does not depend on system ICU, consumed by `TextRunPlanner` as the canonical fallback for iOS / Android / headless / minimal-toolchain hosts. Item 6.8 of the 2026-05-24 macOS plugin-authoring plan. | pulp-canvas | 2026-05-26 |
| simdjson | v3.12.3 | Apache-2.0 | JSON parser used by fastgltf; fetched only when `PULP_ENABLE_SCENE3D=ON` | pulp-scene | 2026-06-03 |
| Skia | chrome/m150 | BSD-3-Clause | GPU 2D rendering engine. Pre-built via [danielraffel/skia-builder](https://github.com/danielraffel/skia-builder) fork (adds iOS device/simulator, visionOS, mac-x86_64, and `Skia.xcframework` slices that upstream `olilarkin/skia-builder` doesn't ship). The fork tracks Skia's `chrome/m150` tag and is the active dependency until upstream publishes those slices. Call sites use the `SkShaders::*` gradient namespace + the 3-arg `ParagraphBuilder::make(..., SkUnicode)` signature; m150 also moves `SkRegion::setRects` to an `SkSpan` parameter and changes the `SkStrikeRef` accessor used by the text shaper. The m150 release does not ship a linux-arm64 slice. The bundled skottie + sksg modules (same Skia repo, same BSD-3-Clause license — no separate dependency) are additionally linked only when the opt-in `PULP_LOTTIE` CMake option is enabled. | pulp-canvas, pulp-render | 2026-06-05 |
| three.js | 077dd13c0e86 | MIT | Native WebGPU bridge demos and tests (optional, fetched only when `PULP_BUILD_TESTS` and `PULP_ENABLE_GPU` are ON) | pulp-render | 2026-04-21 |
| TweetNaCl | 20140427 | Public domain | Ed25519 sign/verify per RFC 8032 — Sparkle appcast signatures and signed node/package metadata | pulp-runtime | 2026-05-25 |
| V8 | v8-15.1.27 | BSD-3-Clause | JIT JavaScript engine backend (optional, selected with `PULP_JS_ENGINE=v8`; default is QuickJS, JSC on Apple). Sealed prebuilt `libv8` (exports only `v8::`/`cppgc::`; bundles ICU/zlib/Abseil internally) via the [danielraffel/v8-builder](https://github.com/danielraffel/v8-builder) per-platform releases; fetched by `tools/scripts/fetch_v8_for_release.py` into `external/v8-build/`. iOS is JSC-only (V8 needs JIT). See NOTICE.md for bundled ICU/zlib/Abseil attribution. | pulp-view | 2026-06-06 |
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
| ASIO SDK | Proprietary (Steinberg) | Optional ASIO device I/O integration | Developer obtains independently; never bundled or exported by Pulp |

## Optional Developer Tooling (installed on demand, not bundled)

These power **developer-only** tooling — building/testing the JS packages
(`@pulp/react`, `pulp-import-ir`), the Figma plugin, the desktop video-proof
composer (`tools/local-ci`), and the Motion visual-analysis lane
(`tools/motion/visual`). They are **not part of the Pulp SDK and are never
shipped in end-user plugins**. Pulp commits only its own source; these
third-party packages are installed on demand (`npm` / `pip`) into ignored
directories and used under their own licenses. Intentionally absent from
`NOTICE.md` (which lists redistributed code only).

| Tool | License | Used For | Pulp Boundary |
|------|---------|----------|---------------|
| esbuild | MIT | Build-time JS/TS bundler for `@pulp/react`, `pulp-import-ir`, the Figma plugin, and the Three.js IIFE bundle | `npm` devDependency, installed on demand; build-time only. Never committed, bundled, exported by `cmake --install`, shipped in plugins, or required by public CI. |
| fflate | MIT | Deflate/zip in the developer-only Figma plugin (`tools/figma-plugin`) | Same boundary. |
| ffmpeg-static | Wrapper MIT; bundled FFmpeg binaries GPL/LGPL | Encodes captured frames into the MP4 used by the video-proof composer | `npm` devDependency in `tools/local-ci`; runtime also accepts a system `PULP_FFMPEG`/`PATH` ffmpeg. Same boundary. |
| json-schema-to-typescript | MIT | Generates TypeScript types from JSON Schema for the Figma plugin | Same boundary. |
| numpy | BSD-3-Clause | Array ops for the Motion visual-analysis lane (`tools/motion/visual`) | `pip`, installed on demand; analysis-only, never bundled or shipped. |
| opencv-python | Apache-2.0 (OpenCV); packaging MIT | Optional affine estimation for the Motion visual-analysis lane (graceful PIL/FFT fallback when absent) | `pip` (optional); analysis-only, never bundled or shipped. |
| Pillow | HPND (BSD-style) | Image IO for the Motion visual-analysis lane | `pip`; analysis-only, never bundled or shipped. |
| Remotion | Remotion License (source-available; free under their tier, paid company license above it) | Composes annotated proof videos (`remotion`, `@remotion/bundler`, `@remotion/renderer`; also pulls MIT `react`/`react-dom`) | `npm` devDependency in `tools/local-ci`. Companies over Remotion's free tier need a Remotion license. Same boundary. |
| scikit-image | BSD-3-Clause | Image metrics for the Motion visual-analysis lane | `pip`; analysis-only, never bundled or shipped. |
| TypeScript | Apache-2.0 | Type-checking/build for `@pulp/react`, `pulp-import-ir`, and the Figma plugin | `npm` devDependency; build-time only. Same boundary. |
| Vitest | MIT | Unit tests (with `@vitest/coverage-v8`) for `@pulp/react` and `pulp-import-ir` | `npm` devDependency; test-only. Same boundary. |

**Type definitions and prerequisites (not pinned here):** the TypeScript
packages also pull DefinitelyTyped `@types/*` and `@figma/plugin-typings`
(type-only, MIT/DefinitelyTyped, never emitted into shipped code). Developer/CI
prerequisites supplied by the environment — the Rust toolchain (`cargo`/`rustc`)
for the experimental `pulp-rs` CLI and native-component examples, the CI linters
`yamllint` (GPL-3.0) and `actionlint` (MIT), and Docker for the visual-harness
image — are likewise developer/CI-supplied, never bundled, exported, or shipped.

## System / OS-Provided Dependencies (not bundled)

These operating-system libraries, services, and frameworks are **not** part of
Pulp's redistributed dependency chain — Pulp does not bundle, vendor, fetch, or
export them. They are provided by the user's OS (or installed by the user), and
Pulp reaches them by dynamic linking, runtime `dlopen`, D-Bus IPC to a system
service, or subprocess invocation. They are intentionally absent from
`NOTICE.md` (which lists redistributed code) and are recognized here and on the
public licensing page (`docs/reference/licensing.md`).

| Name | License | How Used | Pulp Boundary | Platform |
|------|---------|----------|---------------|----------|
| ALSA (alsa-lib / libasound) | LGPL-2.1-or-later | Native audio + MIDI I/O | Dynamically linked (`find_package(ALSA)`); not redistributed | Linux |
| JACK (libjack; PipeWire JACK layer) | LGPL-2.1-or-later | Optional low-latency audio | Linked only when found at build time (`PULP_HAS_JACK`); not redistributed | Linux |
| D-Bus (libdbus-1) | AFL-2.1 OR GPL-2.0-or-later | Desktop IPC transport (portals, AT-SPI, BlueZ) | Runtime `dlopen("libdbus-1.so.3")`; no build-time link, not redistributed | Linux |
| BlueZ (org.bluez) | GPL-2.0-or-later (daemon) / LGPL-2.1-or-later (libs) | Bluetooth-LE MIDI scan/connect/GATT | D-Bus IPC to the system `org.bluez` service; not linked or redistributed | Linux |
| xdg-desktop-portal | LGPL-2.1-or-later | Native file open/save dialogs | D-Bus IPC; not linked or redistributed | Linux |
| AT-SPI2 (at-spi2-core) | LGPL-2.1-or-later | Accessibility tree export | D-Bus IPC over the a11y bus; not linked or redistributed | Linux |
| xclip / xsel / wl-clipboard | GPL-2.0-or-later / GPL-3.0-or-later | Arbitrary-MIME clipboard | Invoked as a subprocess; not linked or redistributed | Linux |
| Windows SDK / WinRT (Windows.Devices.Bluetooth, WASAPI, UI Automation, Win32) | Microsoft — OS/SDK provided | BLE MIDI, audio, accessibility, dialogs, windowing | System APIs in the base Windows SDK (links `WindowsApp`/`runtimeobject`); no out-of-band package | Windows |
| Apple system frameworks (CoreAudio, CoreMIDI, CoreBluetooth, Cocoa/UIKit) | Apple — OS provided | Audio/MIDI/Bluetooth/windowing | Linked system frameworks; OS-provided, not redistributed | macOS / iOS |
