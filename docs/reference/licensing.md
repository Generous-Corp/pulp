# Licensing and Acknowledgements

## Pulp License

Pulp is released under the **MIT License**. You can use it for any purpose — commercial, personal, educational — with no royalties, revenue thresholds, or copyleft obligations. See [LICENSE.md](https://github.com/danielraffel/pulp/blob/main/LICENSE.md) for the full text.

## Third-Party Dependencies

Pulp builds on excellent open-source software. Every dependency that Pulp bundles, fetches automatically, or redistributes is compatible with MIT licensing — there is no copyleft in the shipped dependency chain.

Tables are sorted alphabetically (case-insensitive) by name. Entries here must stay in sync with [DEPENDENCIES.md](https://github.com/danielraffel/pulp/blob/main/DEPENDENCIES.md), [NOTICE.md](https://github.com/danielraffel/pulp/blob/main/NOTICE.md), and [tools/deps/manifest.json](https://github.com/danielraffel/pulp/blob/main/tools/deps/manifest.json) (the machine-readable source of truth).

### Core Dependencies (Always Used)

| Name | License | Purpose | Link |
|------|---------|---------|------|
| **Catch2** | BSL-1.0 | Unit testing framework | [github.com/catchorg/Catch2](https://github.com/catchorg/Catch2) |
| **CHOC** | ISC | JS engine, MIDI utilities, audio file I/O, WebView, networking | [github.com/Tracktion/choc](https://github.com/Tracktion/choc) |
| **cpp-httplib** | MIT | HTTP client (GET/POST/download) used by `pulp-runtime` | [github.com/yhirose/cpp-httplib](https://github.com/yhirose/cpp-httplib) |
| **dr_libs** | Public domain (Unlicense) / MIT-0 | FLAC, MP3, WAV decode (`dr_flac`, `dr_mp3`, `dr_wav`) | [github.com/mackron/dr_libs](https://github.com/mackron/dr_libs) |
| **Highway** | Apache-2.0 | Portable SIMD abstraction (SSE/NEON/AVX) | [github.com/google/highway](https://github.com/google/highway) |
| **Mbed TLS** | Apache-2.0 (dual-licensed; Pulp uses the Apache-2.0 option) | Cryptographic primitives (SHA-256, RSA, AES) | [github.com/Mbed-TLS/mbedtls](https://github.com/Mbed-TLS/mbedtls) |
| **miniz** | MIT | ZIP/GZIP compression | [github.com/richgel999/miniz](https://github.com/richgel999/miniz) |
| **msdfgen** | MIT | Multi-channel SDF glyph generation (planned) | [github.com/Chlumsky/msdfgen](https://github.com/Chlumsky/msdfgen) |
| **nanosvg** | zlib | SVG parsing and rasterization | [github.com/memononen/nanosvg](https://github.com/memononen/nanosvg) |
| **pugixml** | MIT | XML parsing and generation | [github.com/zeux/pugixml](https://github.com/zeux/pugixml) |
| **yaml-cpp** | MIT | YAML frontmatter parser for the DESIGN.md import pipeline (gated to `--from designmd`) | [github.com/jbeder/yaml-cpp](https://github.com/jbeder/yaml-cpp) |
| **Yoga** | MIT | Layout engine for Flexbox/Grid-style native UI | [github.com/facebook/yoga](https://github.com/facebook/yoga) |

### Design Formats and Test Fixtures

Pulp adopts Google's [DESIGN.md](https://github.com/google-labs-code/design.md) format as a first-class import source (`pulp import-design --from designmd`). The format specification is reimplemented in C++ (no upstream code vendored); the YAML frontmatter is parsed with `yaml-cpp` (see Core Dependencies above). One example file from upstream `examples/paws-and-paths/` is redistributed verbatim as the test fixture at `test/fixtures/imports/designmd/alpha/DESIGN.md`.

| Name | License | Purpose | Link |
|------|---------|---------|------|
| **DESIGN.md format spec + `paws-and-paths` fixture** | Apache-2.0 | Design-system interchange format consumed by `pulp import-design --from designmd`; one upstream example file redistributed verbatim as a Pulp test fixture. Pinned at tag `0.1.1`. | [github.com/google-labs-code/design.md @ 0.1.1](https://github.com/google-labs-code/design.md/releases/tag/0.1.1) |

### Embedded Fonts

Embedded at build time for deterministic text rendering. Both fonts are redistributed under the SIL OFL 1.1, which explicitly permits bundling in software.

| Name | License | Purpose | Link |
|------|---------|---------|------|
| **Inter** | SIL OFL 1.1 | Embedded UI font (`Inter-Regular.ttf`, version `4.001;git-9221beed3`) | [github.com/rsms/inter](https://github.com/rsms/inter) |
| **JetBrains Mono** | SIL OFL 1.1 | Embedded monospace font (`JetBrainsMono-Regular.ttf`, version `2.304`) | [github.com/JetBrains/JetBrainsMono](https://github.com/JetBrains/JetBrainsMono) |
| **Noto Color Emoji** | SIL OFL 1.1 | Embedded cross-platform color-emoji typeface (`NotoColorEmoji.ttf`, COLRv1, `noto-emoji main @ 2026-05-17`). Gated by `PULP_BUNDLE_NOTO_COLOR_EMOJI` (defaults ON for Linux/Android/headless, OFF for macOS/Windows where the platform color-emoji typeface is preferred). | [github.com/googlefonts/noto-emoji](https://github.com/googlefonts/noto-emoji) |

### Plugin Format SDKs

| Name | License | Purpose | Link |
|------|---------|---------|------|
| **AudioUnitSDK** | Apache-2.0 | AU v2 plugin format adapter | [github.com/apple/AudioUnitSDK](https://github.com/apple/AudioUnitSDK) |
| **CLAP** | MIT | CLAP plugin format headers | [github.com/free-audio/clap](https://github.com/free-audio/clap) |
| **LV2** | ISC | LV2 plugin format headers | [github.com/lv2/lv2](https://github.com/lv2/lv2) |
| **VST3 SDK** | MIT | VST3 plugin format (pluginterfaces + base) | [github.com/steinbergmedia/vst3sdk](https://github.com/steinbergmedia/vst3sdk) |

### Platform-Specific Dependencies

| Name | License | Purpose | Link |
|------|---------|---------|------|
| **Oboe** | Apache-2.0 | Android audio backend (AAudio/OpenSL ES abstraction) | [github.com/google/oboe](https://github.com/google/oboe) |

### Docs Build & Site Assets

The public docs site (generouscorp.com/pulp/) is generated by MkDocs Material. The following Python packages and their bundled CSS/JS/SVG assets are redistributed as part of the generated site.

| Name | License | Purpose | Link |
|------|---------|---------|------|
| **Material Design Icons** | Apache-2.0 | Icon set bundled inside mkdocs-material; redistributed as SVG in the generated docs site | [github.com/Templarian/MaterialDesign](https://github.com/Templarian/MaterialDesign) |
| **mkdocs** | BSD-2-Clause | Static site generator (transitive via mkdocs-material); theme assets redistributed in the generated docs site | [github.com/mkdocs/mkdocs](https://github.com/mkdocs/mkdocs) |
| **mkdocs-awesome-pages-plugin** | MIT | MkDocs plugin for navigation ordering (build-time) | [github.com/lukasgeiter/mkdocs-awesome-pages-plugin](https://github.com/lukasgeiter/mkdocs-awesome-pages-plugin) |
| **mkdocs-git-revision-date-localized-plugin** | MIT | MkDocs plugin for git-revision dates (build-time) | [github.com/timvink/mkdocs-git-revision-date-localized-plugin](https://github.com/timvink/mkdocs-git-revision-date-localized-plugin) |
| **mkdocs-material** | MIT | Material theme — CSS, JS, SVG icons, and search worker redistributed in the generated docs site | [github.com/squidfunk/mkdocs-material](https://github.com/squidfunk/mkdocs-material) |
| **Pygments** | BSD-2-Clause | Syntax highlighter used by `pymdownx.highlight`; redistributed as inline HTML/CSS | [github.com/pygments/pygments](https://github.com/pygments/pygments) |
| **pymdown-extensions** | MIT | Markdown extension bundle used by mkdocs-material (admonitions, tabs, superfences, highlight, emoji) | [github.com/facelessuser/pymdown-extensions](https://github.com/facelessuser/pymdown-extensions) |

## Optional Vendor SDK Integrations

Some optional integrations depend on separately licensed SDKs that Pulp does not bundle, fetch, export, or redistribute. These SDKs are outside Pulp's MIT dependency chain and are only supported through explicit opt-in local configuration.

| Name | License | Purpose | Distribution |
|------|---------|---------|--------------|
| **AAX SDK** | Separately licensed by Avid | Optional AAX plugin format support | Developer obtains it independently and points `PULP_AAX_SDK_DIR` at an out-of-tree SDK copy |
| **ARA SDK** | MIT-compatible (Celemony) | Optional ARA 2.x integration (pitch correction, spectral editing, clip-aware workflows) | Developer obtains it independently (`https://github.com/Celemony/ARA_SDK`), keeps it out-of-tree, points `PULP_ARA_SDK_DIR` at it, and sets `PULP_ENABLE_ARA=ON`. Never bundled. |
| **ASIO SDK** | Proprietary (Steinberg) | Optional ASIO device I/O support (planned) | Developer obtains it independently; never bundled or exported by Pulp |

Pulp's MIT license does not grant any rights to these vendor SDKs or to any related Avid/PACE tooling.
See [AAX Setup](../guides/aax.md) for the supported local workflow.

### GPU and Windowing

| Name | License | Purpose | Link |
|------|---------|---------|------|
| **Dawn** | BSD-3-Clause | WebGPU implementation (Metal, D3D12, Vulkan), pinned via Skia `chrome/m144` DEPS at `6acf6ef3fe23` | [dawn.googlesource.com](https://dawn.googlesource.com/dawn) |
| **SDL3** | zlib | Cross-platform windowing and input | [github.com/libsdl-org/SDL](https://github.com/libsdl-org/SDL) |
| **Skia** | BSD-3-Clause | 2D GPU rendering engine (Graphite backend), pinned to `chrome/m144 @ cd0c5f445516` with bundled HarfBuzz and ICU DEPS revisions locked for deterministic text shaping | [skia.org](https://skia.org) |
| **WebGPU-distribution** | MIT | WebGPU C API wrapper for Dawn | [github.com/eliemichel/WebGPU-distribution](https://github.com/eliemichel/WebGPU-distribution) |

### Optional Dependencies

Fetched only when the corresponding CMake option or platform gate is enabled.

| Name | License | Purpose | Link |
|------|---------|---------|------|
| **DRACO** | Apache-2.0 | Optional glTF mesh decompression; fetched only when `PULP_ENABLE_DRACO=ON` | [github.com/google/draco](https://github.com/google/draco) |
| **Emscripten** | MIT | C++ to WebAssembly compiler (for WAMv2/WebCLAP) | [emscripten.org](https://emscripten.org) |
| **node-addon-api** | MIT | Node.js bindings via Node-API | [github.com/nodejs/node-addon-api](https://github.com/nodejs/node-addon-api) |
| **pybind11** | BSD-3-Clause | Python bindings for HeadlessHost | [github.com/pybind/pybind11](https://github.com/pybind/pybind11) |
| **react** | MIT | Peer dependency of `@pulp/react` (packages/pulp-react); npm-installed by plugin authors, never bundled into Pulp itself | [github.com/facebook/react](https://github.com/facebook/react) |
| **react-reconciler** | MIT | Reconciler runtime wrapped by `@pulp/react` to drive `pulp::view::WidgetBridge`; npm-installed alongside the package | [github.com/facebook/react](https://github.com/facebook/react) |
| **scheduler** | MIT | Cooperative-scheduling runtime pulled in transitively by `react-reconciler` for `@pulp/react`; npm-installed | [github.com/facebook/react](https://github.com/facebook/react) |
| **three.js** | MIT | Native WebGPU bridge demos and tests; fetched only when `PULP_BUILD_TESTS` and `PULP_ENABLE_GPU` are ON | [github.com/mrdoob/three.js](https://github.com/mrdoob/three.js) |

## Standards and Specifications

Pulp implements or builds on these open standards:

| Standard | Organization | Purpose |
|----------|-------------|---------|
| [Audio Unit](https://developer.apple.com/documentation/audiounit) | Apple | Plugin format specification |
| [CLAP](https://cleveraudio.org) | Clever Audio | Plugin format specification |
| [LV2](https://lv2plug.in) | LV2 Community | Plugin format specification |
| [MIDI 2.0 UMP](https://www.midi.org/specifications) | MIDI Association | Universal MIDI Packet format |
| [OSC 1.0](https://opensoundcontrol.stanford.edu) | CNMAT | Open Sound Control messaging |
| [VST3](https://steinbergmedia.github.io/vst3_dev_portal/) | Steinberg | Plugin format specification |
| [WAMv2](https://www.webaudiomodules.com) | Web Audio Modules | Web plugin standard |
| [WASI](https://wasi.dev) | WebAssembly CG | System interface for WebAssembly |
| [Web Audio API](https://www.w3.org/TR/webaudio/) | W3C | Browser audio processing |
| [Web MIDI API](https://www.w3.org/TR/webmidi/) | W3C | Browser MIDI access |
| [WebCLAP](https://github.com/WebCLAP) | WebCLAP | Portable CLAP plugins via WebAssembly |
| [WebGPU](https://www.w3.org/TR/webgpu/) | W3C | GPU rendering API |

## Projects That Inspired Pulp

| Project | License | What We Learned |
|---------|---------|-----------------|
| [Astral (uv, Ruff, ty)](https://astral.sh/) | Apache-2.0 / MIT | CI/CD security baseline, supply chain hardening, and release process discipline. Pulp adopts several practices from [their open-source security post](https://astral.sh/blog/open-source-security-at-astral). **Already in place:** branch protection on `main`, tag protection on `v*`, default read-only workflow tokens, immutable release identity via tag protection. **In progress / planned:** action SHA pinning via Renovate, immutable release artifacts (UI checkbox), Sigstore release attestations, `zizmor` workflow lint in CI. Tracked in the private `pulp-planning` submodule. |
| [AudioKit](https://github.com/AudioKit/AudioKit) | MIT | Swift audio patterns, Apple platform integration |
| [iPlug2](https://github.com/iPlug2/iPlug2) | zlib-like | Multi-format adapter architecture, graphics abstraction |
| [MotionEyes](https://github.com/edwardsanchez/MotionEyes) | MIT | Agent-first motion observability shape behind `pulp::view::motion`: the `Trace.value` / `Trace.geometry` / `Trace.scrollGeometry` DSL, FrameClock-driven change-only sampling with epsilon/precision, the SwiftUI `GeometryReader` probe pattern, and the Start/End burst framing that lets agents read animation behavior from logs instead of guessing from source. Pulp re-implements all of this from scratch in cross-platform C++20 with Swift, Kotlin (Android), and JS facades — no MotionEyes source code, headers, or fixtures are copied or vendored. We learned the *shape*; the implementation is independent. |
| [SignalKit](https://github.com/niclamusic/signalkit) | MIT | Real-time DSP patterns in Swift |
| [signalsmith-clap-cpp](https://github.com/geraintluff/signalsmith-clap-cpp) | MIT | WCLAP build pipeline, webview extension patterns |
| [Visage](https://github.com/VitalAudio/visage) | MIT | SDF-first rendering, dirty region tracking, shape batching, and a high bar for visual quality in audio plugin interfaces |

## Discipline

Pulp follows strict rules. Implementation is from specs, SDK documentation, and original design — never from studying proprietary or restrictively-licensed source code. For optional AAX support, that means no Avid SDK files or example sources in the repo and no copied implementation text from the SDK examples. See [CLAUDE.md](https://github.com/danielraffel/pulp/blob/main/CLAUDE.md) for the full policy.

## License Policy

Before adding any bundled dependency to Pulp:

1. **Check the license** — must be MIT, BSD, Apache 2.0, ISC, zlib, BSL-1.0, SIL OFL 1.1 (fonts), or public domain
2. **Add to DEPENDENCIES.md** — alphabetical order, with version, license, and purpose
3. **Add to NOTICE.md** — full license text, alphabetical order
4. **Add to docs/reference/licensing.md** — same table row, matching license string
5. **Update tools/deps/manifest.json** — machine-readable inventory consumed by `tools/deps/audit.py`
6. **No copyleft** — GPL, LGPL, AGPL, SSPL are not allowed
7. **MPL-2.0** — requires case-by-case review (weak copyleft)
8. **Vendor SDKs stay separate** — optional AAX/ASIO-style SDKs must remain developer-supplied, out-of-tree, and excluded from `NOTICE.md`

Dependency update workflow is tracked in `tools/deps/manifest.json` and audited by `tools/deps/audit.py`. Run `python3 tools/deps/audit.py --strict` before proposing a dependency change; the audit script will also run in CI (tracked alongside the skill-sync and version-bump gates).
