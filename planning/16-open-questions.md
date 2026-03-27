# Open Questions and Decisions Log

## Architecture Decisions Pending

### 1. MIT vs Apache 2.0 license?

- **MIT**: simpler, more familiar, maximum adoption
- **Apache 2.0**: patent grant, more protective
- **Recommendation**: MIT, but decision needed

### 2. Monorepo vs multi-repo?

- **Recommendation**: Monorepo for v1. Easier cross-subsystem changes, unified CI.
- Revisit after v1 if subsystem adoption diverges.

### 3. UI framework depth: minimal vs comprehensive?

- **Option A**: Minimal view system (hierarchy, events, layout) + rely on GPU UI or SwiftUI for rich widgets
- **Option B**: Full widget library (knobs, faders, meters, waveform displays, etc.)
- **Recommendation**: Option B for audio-specific widgets, Option A for general UI (leverage platform natives)

### 4. Skia vs platform-native 2D rendering?

- **Option A**: CoreGraphics (macOS), Direct2D (Windows), Skia (Linux) — three backends
- **Option B**: Skia everywhere — one backend, consistent rendering
- **Recommendation**: Option A for v1 (better platform integration), consider Skia as optional uniform backend later

### 5. ASIO support strategy?

- **Option A**: Don't support (WASAPI low-latency is good enough)
- **Option B**: Let developers provide ASIO SDK themselves
- **Option C**: Use PortAudio (MIT) which optionally supports ASIO
- **Recommendation**: Option B or C

### 6. Android audio strategy?

- When to prioritize? Phase 9 seems right.
- Oboe (Apache 2.0) is the obvious choice for the audio backend.
- UI on Android: native views or shared C++ UI?

### 7. ARA (Audio Random Access) support?

- Required for advanced DAW integration (audio time-stretching, pitch editing)
- Significant complexity. Defer to post-v1?
- **Recommendation**: Defer

### 8. Web/WASM target?

- iPlug3 targets web via Emscripten/WebGPU
- Should Pulp? Significant engineering effort.
- **Recommendation**: Defer to post-v1, but architect for it

### 9. VST2 support?

- Deprecated by Steinberg, no new licenses
- Still widely expected by some users
- **Recommendation**: Don't support in v1 (VST3 covers the need)

### 10. Plugin hosting (pulp-host)?

- Required for building DAW-like applications
- Significant scope
- **Recommendation**: Phase 9 or post-v1

## Technical Questions

### 11. How to handle parameter ID stability across format versions?

- VST3 requires stable unique IDs
- AU uses integer tags
- This is a known pain point in all frameworks
- **Need**: versioned parameter schema with migration support

### 12. How to handle the "Direct2D vs software" rendering gap on Windows?

- Direct2D requires Windows 10+
- Fallback strategy needed for older Windows?
- **Recommendation**: Target Windows 10+ only (Windows 7/8 are EOL)

### 13. How to handle MIDI Bluetooth on Apple platforms?

- CoreMIDI handles it transparently
- But BLE MIDI has higher latency
- **Need**: document latency implications, don't try to hide them

### 14. What test framework: Catch2 vs doctest?

- Both are header-only, BSD-licensed
- Catch2 is more widely used; doctest is faster to compile
- **Recommendation**: Catch2 (community familiarity)

### 15. How to handle plugin UI resizing across formats?

- VST3, AU, and CLAP all have different resize protocols
- This is a common source of bugs
- **Need**: careful per-format implementation with a unified API

## Ecosystem Questions

### 16. How to build community?

- Open development from day one
- GitHub Discussions for community
- Example projects as showcases
- Claude Code plugin as differentiator

### 17. Commercial support model?

- MIT license means no forced revenue
- Options: consulting, premium support, hosted CI, pre-built SDKs
- Decision deferred

### 18. Naming: is "Pulp" final?

- Check trademark availability
- Check npm/crates.io/pypi namespace
- Current name works well — short, memorable, audio-adjacent (pulp as in raw material)

### 19. When to go public?

- After Phase 3 (build tooling works) — people can see the vision
- After Phase 5 (plugins work in DAWs) — people can use it
- **Recommendation**: Public repo at Phase 1, first "useful" release at Phase 5

### 20. How to handle JUCE migration?

- Some developers will want to port existing JUCE plugins to Pulp
- Cannot provide automated migration (clean-room)
- Can provide: architecture mapping guide, "if you used X in JUCE, here's the Pulp equivalent"
- Must be careful this guide doesn't become a contamination vector
