# Licensing and Acknowledgements

## Pulp License

Pulp is released under the **MIT License**. You can use it for any purpose — commercial, personal, educational — with no royalties, revenue thresholds, or copyleft obligations. See [LICENSE.md](https://github.com/danielraffel/pulp/blob/main/LICENSE.md) for the full text.

## Third-Party Dependencies

Pulp builds on excellent open-source software. Every dependency that Pulp bundles, fetches automatically, or redistributes is compatible with MIT licensing — there is no copyleft in the shipped dependency chain.

### Core Dependencies (Always Used)

| Name | License | Purpose | Link |
|------|---------|---------|------|
| **Catch2** | BSL-1.0 | Unit testing framework | [github.com/catchorg/Catch2](https://github.com/catchorg/Catch2) |
| **CHOC** | ISC | JS engine, MIDI utilities, audio file I/O, WebView, networking | [github.com/Tracktion/choc](https://github.com/Tracktion/choc) |
| **nanosvg** | zlib | SVG parsing and rasterization | [github.com/memononen/nanosvg](https://github.com/memononen/nanosvg) |
| **Yoga** | MIT | Layout engine for Flexbox/Grid-style native UI | [github.com/facebook/yoga](https://github.com/facebook/yoga) |

### Plugin Format SDKs

| Name | License | Purpose | Link |
|------|---------|---------|------|
| **AudioUnitSDK** | Apache-2.0 | AU v2 plugin format adapter | [github.com/apple/AudioUnitSDK](https://github.com/apple/AudioUnitSDK) |
| **CLAP** | MIT | CLAP plugin format headers | [github.com/free-audio/clap](https://github.com/free-audio/clap) |
| **LV2** | ISC | LV2 plugin format headers | [github.com/lv2/lv2](https://github.com/lv2/lv2) |
| **VST3 SDK** | MIT | VST3 plugin format (pluginterfaces + base) | [github.com/steinbergmedia/vst3sdk](https://github.com/steinbergmedia/vst3sdk) |

## Optional Vendor SDK Integrations

Some optional integrations depend on separately licensed SDKs that Pulp does not bundle, fetch, export, or redistribute. These SDKs are outside Pulp's MIT dependency chain and are only supported through explicit opt-in local configuration.

| Name | License | Purpose | Distribution |
|------|---------|---------|--------------|
| **AAX SDK** | Separately licensed by Avid | Optional AAX plugin format support | Developer obtains it independently and points `PULP_AAX_SDK_DIR` at an out-of-tree SDK copy |
| **ASIO SDK** | Proprietary (Steinberg) | Optional ASIO device I/O support (planned) | Developer obtains it independently; never bundled or exported by Pulp |

Pulp's MIT license does not grant any rights to these vendor SDKs or to any related Avid/PACE tooling.
See [AAX Setup](../guides/aax.md) for the supported local workflow.

### GPU and Windowing

| Name | License | Purpose | Link |
|------|---------|---------|------|
| **Dawn** | BSD-3-Clause | WebGPU implementation (Metal, D3D12, Vulkan) | [dawn.googlesource.com](https://dawn.googlesource.com/dawn) |
| **SDL3** | zlib | Cross-platform windowing and input | [github.com/libsdl-org/SDL](https://github.com/libsdl-org/SDL) |
| **Skia Graphite** | BSD-3-Clause | 2D GPU rendering engine | [skia.org](https://skia.org) |
| **WebGPU-distribution** | MIT | WebGPU C API wrapper for Dawn | [github.com/eliemichel/WebGPU-distribution](https://github.com/eliemichel/WebGPU-distribution) |

### Optional Dependencies

| Name | License | Purpose | Link |
|------|---------|---------|------|
| **Emscripten** | MIT | C++ to WebAssembly compiler (for WAMv2/WebCLAP) | [emscripten.org](https://emscripten.org) |
| **node-addon-api** | MIT | Node.js bindings via Node-API | [github.com/niclamusic/node-addon-api](https://github.com/niclamusic/node-addon-api) |
| **pybind11** | BSD-3-Clause | Python bindings for HeadlessHost | [github.com/pybind/pybind11](https://github.com/pybind/pybind11) |

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
| [SignalKit](https://github.com/niclamusic/signalkit) | MIT | Real-time DSP patterns in Swift |
| [signalsmith-clap-cpp](https://github.com/geraintluff/signalsmith-clap-cpp) | MIT | WCLAP build pipeline, webview extension patterns |
| [Visage](https://github.com/VitalAudio/visage) | MIT | SDF-first rendering, dirty region tracking, shape batching, and a high bar for visual quality in audio plugin interfaces |

## Discipline

Pulp follows strict rules. Implementation is from specs, SDK documentation, and original design — never from studying proprietary or restrictively-licensed source code. For optional AAX support, that means no Avid SDK files or example sources in the repo and no copied implementation text from the SDK examples. See [CLAUDE.md](https://github.com/danielraffel/pulp/blob/main/CLAUDE.md) for the full policy.

## License Policy

Before adding any bundled dependency to Pulp:

1. **Check the license** — must be MIT, BSD, Apache 2.0, ISC, zlib, BSL-1.0, or public domain
2. **Add to DEPENDENCIES.md** — alphabetical order, with version, license, and purpose
3. **Add to NOTICE.md** — full license text, alphabetical order
4. **No copyleft** — GPL, LGPL, AGPL, SSPL are not allowed
5. **MPL-2.0** — requires case-by-case review (weak copyleft)
6. **Vendor SDKs stay separate** — optional AAX/ASIO-style SDKs must remain developer-supplied, out-of-tree, and excluded from `NOTICE.md`

Dependency update workflow is tracked in `tools/deps/manifest.json` and audited by `tools/deps/audit.py`.
