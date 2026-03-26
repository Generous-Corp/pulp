# Licensing and Acknowledgements

## Pulp License

Pulp is released under the **MIT License**. You can use it for any purpose — commercial, personal, educational — with no royalties, revenue thresholds, or copyleft obligations. See [LICENSE.md](https://github.com/danielraffel/pulp/blob/main/LICENSE.md) for the full text.

## Third-Party Dependencies

Pulp builds on excellent open-source software. Every dependency is compatible with MIT licensing — no copyleft in the dependency chain.

### Core Dependencies (Always Used)

| Name | License | Purpose | Link |
|------|---------|---------|------|
| **CHOC** | ISC | JS engine, MIDI utilities, audio file I/O, WebView, networking | [github.com/Tracktion/choc](https://github.com/Tracktion/choc) |
| **Catch2** | BSL-1.0 | Unit testing framework | [github.com/catchorg/Catch2](https://github.com/catchorg/Catch2) |
| **nanosvg** | zlib | SVG parsing and rasterization | [github.com/memononen/nanosvg](https://github.com/memononen/nanosvg) |

### Plugin Format SDKs

| Name | License | Purpose | Link |
|------|---------|---------|------|
| **CLAP** | MIT | CLAP plugin format headers | [github.com/free-audio/clap](https://github.com/free-audio/clap) |
| **VST3 SDK** | MIT | VST3 plugin format (pluginterfaces + base) | [github.com/steinbergmedia/vst3sdk](https://github.com/steinbergmedia/vst3sdk) |
| **AudioUnitSDK** | Apache-2.0 | AU v2 plugin format adapter | [github.com/apple/AudioUnitSDK](https://github.com/apple/AudioUnitSDK) |
| **LV2** | ISC | LV2 plugin format headers | [lv2plug.in](https://lv2plug.in) |

### GPU and Windowing

| Name | License | Purpose | Link |
|------|---------|---------|------|
| **Dawn** | BSD-3-Clause | WebGPU implementation (Metal, D3D12, Vulkan) | [dawn.googlesource.com](https://dawn.googlesource.com/dawn) |
| **Skia Graphite** | BSD-3-Clause | 2D GPU rendering engine | [skia.org](https://skia.org) |
| **SDL3** | zlib | Cross-platform windowing and input | [github.com/libsdl-org/SDL](https://github.com/libsdl-org/SDL) |
| **WebGPU-distribution** | MIT | WebGPU C API wrapper for Dawn | [github.com/niclamusic/webgpu-distribution](https://github.com/niclamusic/webgpu-distribution) |

### Optional Dependencies

| Name | License | Purpose | Link |
|------|---------|---------|------|
| **pybind11** | BSD-3-Clause | Python bindings for HeadlessHost | [github.com/pybind/pybind11](https://github.com/pybind/pybind11) |
| **node-addon-api** | MIT | Node.js bindings via Node-API | [github.com/niclamusic/node-addon-api](https://github.com/niclamusic/node-addon-api) |
| **Emscripten** | MIT | C++ to WebAssembly compiler (for WAMv2/WebCLAP) | [emscripten.org](https://emscripten.org) |

## Standards and Specifications

Pulp implements or builds on these open standards:

| Standard | Organization | Purpose |
|----------|-------------|---------|
| [CLAP](https://cleveraudio.org) | Clever Audio | Plugin format specification |
| [VST3](https://steinbergmedia.github.io/vst3_dev_portal/) | Steinberg | Plugin format specification |
| [Audio Unit](https://developer.apple.com/documentation/audiounit) | Apple | Plugin format specification |
| [LV2](https://lv2plug.in) | LV2 Community | Plugin format specification |
| [WAMv2](https://www.webaudiomodules.com) | Web Audio Modules | Web plugin standard |
| [WebCLAP](https://github.com/WebCLAP) | WebCLAP | Portable CLAP plugins via WebAssembly |
| [Web Audio API](https://www.w3.org/TR/webaudio/) | W3C | Browser audio processing |
| [Web MIDI API](https://www.w3.org/TR/webmidi/) | W3C | Browser MIDI access |
| [WebGPU](https://www.w3.org/TR/webgpu/) | W3C | GPU rendering API |
| [WASI](https://wasi.dev) | WebAssembly CG | System interface for WebAssembly |
| [OSC 1.0](https://opensoundcontrol.stanford.edu) | CNMAT | Open Sound Control messaging |
| [MIDI 2.0 UMP](https://www.midi.org/specifications) | MIDI Association | Universal MIDI Packet format |

## Projects That Inspired Pulp

These projects were studied for architecture and patterns (not code). Pulp's implementation is original.

| Project | License | What We Learned |
|---------|---------|-----------------|
| [iPlug2](https://github.com/iPlug2/iPlug2) | zlib-like | Multi-format adapter architecture, graphics abstraction |
| [AudioKit](https://github.com/AudioKit/AudioKit) | MIT | Swift audio patterns, Apple platform integration |
| [SignalKit](https://github.com/niclamusic/signalkit) | MIT | Real-time DSP patterns in Swift |
| [signalsmith-clap-cpp](https://github.com/geraintluff/signalsmith-clap-cpp) | MIT | WCLAP build pipeline, webview extension patterns |

## Clean-Room Discipline

Pulp follows strict clean-room rules. Implementation is from specs, SDK documentation, and original design — never from studying proprietary or restrictively-licensed source code. See [CLAUDE.md](https://github.com/danielraffel/pulp/blob/main/CLAUDE.md) for the full clean-room policy.

## License Policy

Before adding any dependency to Pulp:

1. **Check the license** — must be MIT, BSD, Apache 2.0, ISC, zlib, BSL-1.0, or public domain
2. **Add to DEPENDENCIES.md** — alphabetical order, with version, license, and purpose
3. **Add to NOTICE.md** — full license text, alphabetical order
4. **No copyleft** — GPL, LGPL, AGPL, SSPL are not allowed
5. **MPL-2.0** — requires case-by-case review (weak copyleft)
