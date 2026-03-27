# Appendices

## Appendix A: Pulp Subsystem Inventory

| Subsystem | Language | Platforms | Status | Phase |
|-----------|----------|-----------|--------|-------|
| pulp-platform | C++ | All | Planned | 2 |
| pulp-runtime | C++ | All | Planned | 2 |
| pulp-events | C++ | All | Planned | 2 |
| pulp-canvas | C++ | All | Planned | 6 |
| pulp-view | C++ | All | Planned | 6 |
| pulp-view-swift | Swift | Apple | Planned | 6 |
| pulp-audio | C++ | All | Planned | 4 |
| pulp-midi | C++ | All | Planned | 4 |
| pulp-signal | C++ | All | Planned | 9 |
| pulp-format | C++ | All | Planned | 5 |
| pulp-format-swift | Swift | Apple | Planned | 5 |
| pulp-state | C++ | All | Planned | 5 |
| pulp-host | C++ | All | Planned | 9+ |
| pulp-build | CMake | All | Planned | 3 |
| pulp-test | C++ | All | Planned | 3 |
| pulp-ship | Scripts | All | Planned | 8 |
| pulp-ci | YAML/Scripts | All | Planned | 8 |
| pulp-claude | Markdown | All | Planned | 7 |
| pulp-gpu | C++ | All | Planned | 6 |

## Appendix B: juce-dev Command Inventory

| Command | Purpose | Pulp Equivalent |
|---------|---------|-----------------|
| /juce-dev:create | Project scaffolding | /pulp:create |
| /juce-dev:build | Build wrapper | /pulp:build |
| /juce-dev:ci | CI/CD management | /pulp:ci |
| /juce-dev:port | Cross-platform porting | /pulp:port |
| /juce-dev:setup-visage | Add GPU UI | /pulp:setup-gpu |
| /juce-dev:setup-ios | Add iOS target | /pulp:setup-ios |
| /juce-dev:setup-updates | Add auto-updates | /pulp:setup-updates |
| /juce-dev:status | Project dashboard | /pulp:status |
| /juce-dev:vm | VM management | /pulp:vm |
| /juce-dev:website | Download page | /pulp:website |
| /juce-dev:theme | Theme designer | /pulp:theme |
| (none) | Test runner | /pulp:test (NEW) |
| (none) | Package/distribute | /pulp:ship (NEW) |
| (none) | Add Swift layer | /pulp:setup-swift (NEW) |

## Appendix C: Proposed Pulp Plugin Command Inventory

| Command | Purpose | Arguments |
|---------|---------|-----------|
| /pulp:create | Project scaffolding | `"<name>"` `[--gpu]` `[--swift]` `[--no-github]` |
| /pulp:build | Build wrapper | `[targets]` `[action]` `[options]` |
| /pulp:test | Run tests | `[unit\|format\|visual\|all]` `[--coverage]` |
| /pulp:ci | CI/CD management | `[status\|logs\|secrets\|trigger]` `[platform]` `[mode]` |
| /pulp:port | Cross-platform porting | `<platform>` `[--audit-only]` `[--vm]` |
| /pulp:ship | Package and distribute | `[platform]` `[--sign]` `[--notarize]` `[--release]` |
| /pulp:status | Project dashboard | (no args) |
| /pulp:setup-gpu | Add GPU rendering | (no args) |
| /pulp:setup-ios | Add iOS target | (no args) |
| /pulp:setup-updates | Add auto-updates | `[--doctor]` |
| /pulp:setup-swift | Add Swift UI layer | (no args) |
| /pulp:vm | VM management | `[add\|remove\|list\|test]` |
| /pulp:website | Download page | `[--regenerate]` |
| /pulp:theme | Theme designer | `[--open\|--generate\|--new]` |

## Appendix D: Top 20 Engineering Risks

1. Plugin format compatibility across DAWs (VST3 quirks, AU validation, AAX signing)
2. UI framework scope creep (building too much before it's needed)
3. Cross-platform build system complexity (CMake + SPM + platform toolchains)
4. Audio callback reliability on all platforms (WASAPI exclusive mode, ALSA config)
5. Parameter ID stability across plugin versions and formats
6. State serialization versioning and migration
7. GPU UI destruction ordering bugs (Metal display link timing)
8. Swift/C++ interop evolution across Swift versions
9. SwiftUI in plugin host windowing (AUv3 embedding)
10. MIDI timing accuracy (sample-accurate across formats)
11. Thread safety at audio/UI boundary
12. Windows ASIO support (licensing, driver compatibility)
13. Linux audio stack fragmentation (ALSA vs PipeWire vs JACK)
14. Notarization workflow reliability (Apple server timeouts)
15. Auto-update EdDSA key management (Keychain, CI secrets)
16. Template maintenance burden (keeping starter templates current)
17. CI matrix combinatorial explosion (3 platforms x 6 formats x multiple DAWs)
18. Plugin scan performance (directory scanning, validation caching)
19. Accessibility compliance across platforms (UIA, NSAccessibility, ATK)
20. Mobile (iOS/Android) audio latency and lifecycle management

## Appendix E: Top 20 Contamination Risks

1. CMake function naming (pulp_add_plugin vs juce_add_plugin)
2. Plugin processor base class API shape
3. Parameter system type hierarchy
4. State management tree structure (avoiding ValueTree patterns)
5. Component/View lifecycle model (paint/repaint, parent/child)
6. Event dispatch architecture (avoiding MessageManager singleton)
7. LookAndFeel to Theme redesign (must be composably different)
8. Audio device manager API surface
9. MIDI message type representation
10. AudioBuffer/SignalBlock API shape
11. DSP ProcessorChain composition model
12. Module/subsystem decomposition boundaries
13. Build system target naming patterns
14. Plugin format adapter internal architecture
15. Standalone app wrapper design
16. Test framework API (avoiding UnitTest class pattern)
17. File/path abstraction API
18. String handling approach
19. Serialization format choices (XML vs binary)
20. Error handling patterns (Result type, exceptions, error codes)

## Appendix F: Top 20 Unknowns Requiring Follow-Up

1. Visage license terms (need to verify before bundling)
2. bgfx performance on Linux with Vulkan (less tested than Metal/D3D11)
3. SwiftUI performance for complex plugin UIs with many parameters (100+)
4. Swift/C++ interop behavior with template-heavy DSP code
5. Windows ARM64 audio stack maturity
6. PipeWire adoption rate and API stability on Linux
7. CLAP format adoption in major DAWs (growing but not universal)
8. AAX SDK availability for new developers (Avid's approval process)
9. Apple's direction for Audio Units (AU vs AUv3 long-term)
10. WebGPU native (Dawn) maturity for desktop use
11. Skia Graphite readiness for production use
12. Android audio latency state with Oboe on modern devices
13. iOS AUv3 memory limits and background audio behavior
14. Trademark availability for "Pulp" in software/audio
15. Community interest level in a JUCE alternative
16. CI cost for comprehensive multi-platform testing
17. Documentation tooling decision (Doxygen, mkdocs, mdbook)
18. Package manager distribution strategy (vcpkg, Conan, SPM)
19. Windows CI runner availability for MSVC builds (GitHub-hosted vs self-hosted)
20. EdDSA key management best practices for teams (shared secrets, HSM)
