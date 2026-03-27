# iPlug3 vs Pulp — Comprehensive Comparison and Gap Analysis

Date: 2026-03-24

## Executive Summary

iPlug3 and Pulp have **remarkably convergent visions** — both target the same rendering stack (Dawn/Skia Graphite/QuickJS), both embrace AI-first development, both prioritize permissive licensing. The key differences are in scope, current maturity, and specific architectural choices. This analysis identifies **material gaps** in Pulp's plans that iPlug3's feature list reveals.

---

## 1. Technology Stack Comparison

| Component | Pulp (Planned) | iPlug3 (Announced) | Match? |
|-----------|---------------|-------------------|--------|
| GPU API | WebGPU via Dawn | WebGPU via Dawn | ✅ Same |
| 2D Renderer | Skia Graphite | Skia Graphite | ✅ Same |
| JS Engine | QuickJS/V8/JSC (adaptive) | QuickJS/V8/JSC (adaptive) | ✅ Same |
| Windowing | Native per-platform | **SDL3** | ❌ Different |
| MIDI Library | Custom (CoreMIDI, etc.) | **libremidi** | ❌ Different |
| Audio I/O | Custom (CoreAudio, etc.) | Not documented (likely custom) | ? |
| Build System | CMake | CMake | ✅ Same |
| Utilities | CHOC (ISC) | CHOC (ISC) | ✅ Same |
| C++ Standard | C++20 | C++20 | ✅ Same |
| Plugin Interface | Virtual (Processor base class) | **C++20 Concepts** | ❌ Different |

### Key Technology Divergences

**SDL3 vs Native Windowing:** iPlug3 uses SDL3 for cross-platform windowing/input/events. Pulp plans native per-platform code (NSView, HWND, X11). SDL3 is a pragmatic choice that eliminates platform windowing code (~80% of the effort), but adds a ~1MB dependency. **This is worth reconsidering** — SDL3 would dramatically reduce Pulp's platform windowing workload.

**libremidi vs Custom MIDI:** iPlug3 uses libremidi (cross-platform MIDI library with MIDI2/UMP support). Pulp currently has custom CoreMIDI code. **This is worth reconsidering** — libremidi provides MIDI 2.0/UMP for free and is MIT licensed.

**C++20 Concepts vs Virtual Inheritance:** iPlug3 uses concepts for the plugin interface (zero-overhead). Pulp uses a virtual `Processor` base class. Our review doc already flagged this. **Low priority** — the virtual call overhead is negligible for audio callbacks, and the current approach works.

---

## 2. Feature Gap Analysis

### Features iPlug3 Has That Pulp PLANS But Hasn't Built

| Feature | Pulp Status | Priority |
|---------|------------|----------|
| GPU rendering (Dawn/Skia) | Placeholder | Phase 6 — critical |
| JS scripted UI | Placeholder | Phase 6 — critical |
| Widget library | Placeholder | Phase 6 — critical |
| Design tokens/theming | Planned | Phase 6 |
| Hot-reload | Planned | Phase 6 |
| Component inspector | Placeholder | Phase 6 |
| Post-processing effects | Planned | Phase 6 |
| Audio visualization | Planned | Phase 6 |
| AUv3 | Planned | Phase 5 deferred |
| CLI tool | Planned | Phase 7 |
| MCP server | Planned | Phase 7 |
| Signing/notarization | Planned | Phase 8 |
| Web/WASM | Planned | Phase 9 |
| DSP library | Placeholder | Phase 9 |
| Windows/Linux backends | Planned | Phase 4 expansion |

### Features iPlug3 Has That Pulp DOESN'T Plan For (GAPS)

| Feature | iPlug3 | Pulp | Impact | Recommendation |
|---------|--------|------|--------|----------------|
| **SDL3 windowing** | Yes | No (native per-platform) | HIGH — massive workload reduction | **Adopt SDL3** for windowing/input |
| **libremidi** | Yes | No (custom MIDI) | MEDIUM — MIDI2/UMP for free | **Adopt libremidi** |
| **Python/Node.js bindings** | Yes | No | MEDIUM — server-side audio, testing | Add to Phase 9+ |
| **DSL support (FAUST/Cmajor/JSFX)** | Yes, hot-reload | No | MEDIUM — rapid DSP prototyping | Add to Phase 9+ |
| **WebCLAP format** | Yes | No | LOW — emerging format | Add when spec stabilizes |
| **WAMv2 format** | Yes | No | LOW — web-only | Add to Phase 9 |
| **Offline video rendering** | Yes | No | LOW — niche | Consider Phase 9+ |
| **Screenshot capture for LLM** | Yes, built-in | No (planned inspectror) | MEDIUM — AI validation | Add to Phase 7 |
| **Compute shaders for audio** | Yes, documented | Mentioned in VISION.md | MEDIUM — exploration | Keep in Phase 9 |
| **Multi-bus architecture** | Yes | No | **HIGH** — missing from Pulp | **Add to Phase 5** |
| **MIDI2/UMP native** | Yes, via libremidi | No | MEDIUM — future standard | Adopt via libremidi |
| **MPE support** | Yes, built-in synth engine | No | MEDIUM — growing adoption | Add to Phase 9 |
| **REAPER extension format** | Yes | No | LOW — niche | Skip |
| **Sanitizer integration** | Yes (ASan, TSan, RTSan) | No built-in | MEDIUM — code quality | Add to CI |
| **visionOS support** | Yes | No | LOW — tiny market | Skip for now |

---

## 3. Critical Gaps That Need Immediate Attention

### 3.1 Multi-Bus Architecture (HIGH PRIORITY)

**The Gap:** Pulp currently assumes a single stereo input + single stereo output. iPlug3 supports main, auxiliary, and sidechain buses with optional activation. This is **essential for real-world plugins** — sidechaining is used in every compressor, ducker, and vocoder.

**Impact on existing work:** The `Processor` interface and all format adapters need to support multiple buses. The `PluginDescriptor` needs bus configuration. VST3 `setBusArrangements`, CLAP `audio_ports`, and AU bus arrays all support this natively.

**Recommendation:** Extend `PluginDescriptor` and `Processor` to support multi-bus before Phase 6. This is a foundational change that gets harder to retrofit later.

### 3.2 SDL3 for Windowing (HIGH PRIORITY for Phase 6)

**The Gap:** Pulp plans native per-platform windowing code. iPlug3 uses SDL3.

**Impact:** Writing native windowing code for macOS (NSView), Windows (HWND), Linux (X11/Wayland), iOS (UIView), and web (Emscripten) is an enormous amount of work — easily 2-3 months of engineering. SDL3 provides all of this for free, is cross-platform, handles HiDPI, keyboard/mouse/touch input, clipboard, file dialogs, and more.

**Recommendation:** Use SDL3 for windowing/input in the rendering engine. This is the single biggest workload reduction possible for Phase 6.

### 3.3 libremidi for MIDI (MEDIUM PRIORITY)

**The Gap:** Pulp has custom CoreMIDI code. iPlug3 uses libremidi which provides MIDI 1.0, MIDI 2.0/UMP, and cross-platform support.

**Impact:** Pulp would need to write MIDI backends for Windows (Win32 MIDI), Linux (ALSA), and eventually MIDI 2.0 support. libremidi provides all of this.

**Recommendation:** Replace custom MIDI code with libremidi. It's MIT licensed and actively maintained.

---

## 4. Areas Where Pulp's Plans Are More Ambitious

| Feature | Pulp | iPlug3 |
|---------|------|--------|
| **Swift-native Apple layer** | Planned (AUv3 in Swift, SwiftUI UI) | No Swift support |
| **Android support** | Planned from day one | "TBC" (not committed) |
| **Documentation-first** | Comprehensive docs planned | "Learn from examples" |
| **Open development** | Public repo | Private development |
| **License clarity** | MIT from day one | "TBD" |
| **Tiered UI options** | Headless + native + GPU | GPU only |
| **Community design** | Multi-contributor | Single maintainer |

---

## 5. Comparison Against JUCE Audit Capabilities

### What JUCE Has That Both Pulp and iPlug3 Should Match

| JUCE Capability | Pulp Status | iPlug3 Status | Gap? |
|----------------|-------------|--------------|------|
| Audio file R/W (WAV, AIFF, FLAC) | Not implemented | Not announced | Yes |
| Comprehensive DSP library | Placeholder | Via CHOC/custom | Yes |
| Audio processor chain | Not implemented | Not announced | Yes |
| Multi-bus support | Not implemented | Yes | **Pulp gap** |
| AU validation (auval) | ✅ Working | Not verified | No |
| VST3 validation (pluginval) | ✅ Working | Not verified | No |
| Undo/redo system | Not implemented | Not announced | Future |
| Preset management | Not implemented | Not announced | Phase 9 |
| Plugin host (AudioPluginHost) | Not planned | Not announced | Skip |
| OSC support | Not planned | Not announced | Skip |
| Box2D / physics | Not planned | Not announced | Skip |

---

## 6. Recommendations Summary

### Must Do (Material Impact on Architecture)

1. **Add multi-bus support** to `Processor`/`PluginDescriptor` — sidechain inputs are essential
2. **Evaluate SDL3** for Phase 6 windowing — massive workload reduction
3. **Evaluate libremidi** to replace custom MIDI — MIDI2/UMP + cross-platform for free

### Should Do (Good Ideas from iPlug3)

4. Add **screenshot capture** to the inspector/testing system for LLM validation
5. Add **sanitizer support** (ASan, TSan, RTSan) to CI workflow
6. Plan for **Python/Node.js bindings** in Phase 9+
7. Plan for **DSL integration** (FAUST, Cmajor) in Phase 9+

### Nice to Have (Lower Priority)

8. WebCLAP/WAMv2 format support (when specs stabilize)
9. Offline video rendering mode
10. visionOS support
11. C++20 concepts for plugin interface (current virtual approach is fine)

### Keep as Differentiators (Pulp's Advantages)

12. Swift-native Apple layer (unique vs iPlug3)
13. Tiered UI options (headless + native + GPU)
14. Open development and clear licensing
15. Documentation-first culture
16. Android commitment

---

## 7. Impact on Current Work

### Changes to Existing Code Required

**Multi-bus support** requires changes to:
- `PluginDescriptor` — add bus configuration
- `Processor::process()` — support multiple input/output buses
- All format adapters (VST3, CLAP, AU) — map bus configurations
- Existing examples — update to use bus API (even if single bus)

**This should be done before Phase 6** because the format adapters are already working and tested — adding multi-bus now is easier than retrofitting later with a UI on top.

### No Changes Needed

- The rendering strategy (Dawn/Skia/QuickJS) is confirmed correct — iPlug3 chose the same stack
- The build system approach (CMake) is confirmed correct
- The CHOC dependency is confirmed correct (iPlug3 also uses it)
- The entry macro approach is solid
- The headless-first architecture is correct
- The state/parameter system is compatible with multi-bus addition

### Potential Changes to Plans

If SDL3 is adopted for windowing:
- Remove native windowing plans from Phase 6
- Add SDL3 as a dependency
- Simplify the `pulp-render` architecture
- Still use Dawn for GPU, Skia for 2D, JS for scripting — just SDL3 for the window

If libremidi is adopted:
- Replace `core/midi/platform/mac/coremidi_device.mm` with libremidi wrapper
- Gain Windows, Linux, MIDI2/UMP support immediately
- Simplify Phase 4 cross-platform expansion
