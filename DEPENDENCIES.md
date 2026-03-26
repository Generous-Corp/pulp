# Dependencies

All dependencies must be compatible with MIT licensing. No copyleft (GPL, LGPL, AGPL) dependencies in any subsystem.

## Policy

- **Allowed licenses:** MIT, BSD-2-Clause, BSD-3-Clause, Apache-2.0, ISC, zlib, BSL-1.0, Unlicense, public domain
- **Not allowed:** GPL, LGPL, AGPL, SSPL, proprietary, any copyleft
- **Review required:** MPL-2.0 (weak copyleft, case-by-case evaluation)

## Current Dependencies

| Name | Version | License | How Used | Subsystem | Added |
|------|---------|---------|----------|-----------|-------|
| AudioUnitSDK | 1.4.0 | Apache-2.0 | AU v2 plugin format adapter (AUEffectBase, factory) | pulp-format | 2026-03-24 |
| Catch2 | 3.7.1 | BSL-1.0 | Unit testing framework | test | 2026-03-24 |
| CHOC | main | ISC | JS engine abstraction, MIDI utilities, audio helpers | multiple | 2026-03-24 |
| CLAP | 1.2.2 | MIT | CLAP plugin format headers | pulp-format | 2026-03-24 |
| nanosvg | master | zlib | SVG parsing and rasterization | pulp-canvas | 2026-03-25 |
| pybind11 | 2.13.6 | BSD-3-Clause | Python bindings for HeadlessHost (optional, FetchContent) | bindings/python | 2026-03-25 |
| SDL3 | 3.2.12 | zlib | Cross-platform windowing, input, GPU context | pulp-view | 2026-03-25 |
| Skia Graphite + Dawn | m144 | BSD-3-Clause | GPU 2D rendering + WebGPU (pre-built via skia-builder) | pulp-canvas, pulp-render | 2026-03-25 |
| VST3 SDK | 3.7.12 | MIT | VST3 plugin format (pluginterfaces + base) | pulp-format | 2026-03-24 |
| WebGPU-distribution | 0.2.1 | MIT | WebGPU C API wrapper for Dawn (FetchContent) | pulp-render | 2026-03-25 |

## Format SDKs (Obtained by Developers)

These SDKs are not bundled but are required for specific plugin format support:

| SDK | License | Required For | Bundled? |
|-----|---------|-------------|----------|
| VST3 SDK | MIT | VST3 format | Cloned at configure time (`git clone --depth 1`) |
| AudioUnit SDK | Apache-2.0 | AU v2 format | Cloned at configure time (`git clone --depth 1`) |
| CLAP | MIT | CLAP format | FetchContent (automatic) |
| LV2 SDK | ISC | LV2 format (planned) | Not yet used |
| AAX SDK | Proprietary (Avid) | AAX format (planned) | Developer obtains independently |
| ASIO SDK | Proprietary (Steinberg) | ASIO audio I/O (planned) | Developer obtains independently |
