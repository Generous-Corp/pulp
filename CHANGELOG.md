# Changelog

All notable changes to Pulp are documented here.

## [0.9.0]

## [0.5.0]

## [0.8.0]

## [0.7.0]

## [0.4.0]

## [0.6.0]

## [0.5.0]

## [0.3.0]

## [0.4.0]

## [0.2.2] — 2026-04-09

### Fixed
- **Installed `pulp` CLI now runs without crashing** (#74): added `INSTALL_RPATH` to the `pulp-cli` target so the installed binary resolves `libwgpu_native.dylib` / `libwgpu_native.so` from `<prefix>/lib`. Previously every installed CLI crashed immediately on launch with `dyld: Library not loaded: @rpath/libwgpu_native.dylib`.
- **Windows SDK now ships the wgpu_native import library under both names** (#94, #104 follow-up): PulpConfig.cmake now looks for `wgpu_native.dll.lib` (the modern MSVC IMPLIB convention used by wgpu-native v0.19+) in addition to `wgpu_native.lib` and the legacy prefixed name. The fail-fast error message lists every name it tried.
- **Bare/app standalone templates now link `Pulp::standalone` explicitly** (#104 follow-up): scaffolded plugins built with `--type bare` or `--type app` failed to link on Windows MSVC with `LNK2019: unresolved external symbol StandaloneApp::~StandaloneApp` because the templates only linked `Pulp::format`, not `Pulp::standalone`. Both templates now link both targets.

### Added
- **SDF glyph atlas: real SkFont rasterizer** (#76 follow-up): replaces the placeholder circle rasterizer with `SkFont::drawSimpleText` into an offscreen `SkBitmap` when `PULP_HAS_SKIA` is defined. Falls back to the placeholder for builds without Skia so the host-only test suite still passes.

## [0.2.1] — 2026-04-09

### Fixed
- **Windows SDK**: ship `wgpu_native.lib` import library (#94). v0.2.0 silently dropped this file from the install, breaking `find_package(Pulp)` on Windows for any downstream plugin. Existing v0.2.0 Windows installs are broken and must be reinstalled.
- **Windows ARM64 build** (#92): move `windows.h` and `shellapi.h` includes outside the `pulp::view` namespace in `core/view/src/file_browser.cpp` so winbase.h `_Interlocked*` intrinsics no longer leak into the namespace and break compilation.
- **Visual Studio multi-config generator**: add `CMAKE_MAP_IMPORTED_CONFIG_*=Release` to PulpConfig.cmake so plugins built in Debug / MinSizeRel / RelWithDebInfo can resolve Pulp's Release-only imported targets.

### Added
- **Android MIDI I/O Phase 1** (#86): lock-free SPSC FIFO between the Kotlin MIDI receiver thread and the audio thread, plus `PulpMidiManager` instantiation in `PulpApplication.onCreate`. USB MIDI bytestream input now flows from Android `MidiManager` into the audio thread's `MidiBuffer`. BLE MIDI and virtual ports remain Phase 2/3.
- **Android TalkBack accessibility bridge** (#87): JNI exports that walk the C++ view tree, expose `AccessibilityValueInterface` / `AccessibilityTableInterface` data to Android, and route `ACTION_CLICK` / `ACTION_SCROLL_FORWARD` / `ACTION_SCROLL_BACKWARD` back into Pulp widgets. `PulpAccessibilityDelegate` is now attached to `PulpSurfaceView` automatically.
- **Out-of-line accessibility interface vtables** (`core/view/src/accessibility.cpp`): anchor the typeinfo for `AccessibilityValueInterface` / `AccessibilityTextInterface` / `AccessibilityTableInterface` / `AccessibilityCellInterface` so `dynamic_cast` links cleanly under the Android NDK's `-Wl,--no-undefined`. Adds the missing default `get_value_string()` implementation.
- **Nested-clip text rendering regression test** (#75): guards the existing per-glyph SkTextBlob workaround so a future refactor cannot re-introduce SkShaper for the widget paint path without first solving the underlying Graphite ghost issue.
- **SDF glyph atlas exploration prototype** (#76, `explore/sdf-text` foundation): clean public API, Felzenszwalb-Huttenlocher Euclidean distance transform, and 54 assertions of test coverage. Foundation only — GPU shader and canvas integration follow.

### Re-enabled
- `windows-arm64` row in the `release-cli.yml` matrix, now that #92 is fixed.

## [0.2.0] — 2026-04-09

### Added
- **Android platform**: Full GPU rendering pipeline + interactive Pulp widgets (#53)
- **`pulp ship notarize`**: Apple notarization with submit, poll, and staple (#79)
- **`pulp ship appcast`**: Sparkle-compatible XML update feed generation with EdDSA signing (#79)
- **`pulp ship android`**: APK/AAB packaging via Gradle with keystore signing, ABI selection, Android SDK discovery (#79)
- **Ship config fallback**: CLI flags → env vars → `~/.pulp/config.toml` for all signing credentials (#79)
- **`config.example.toml`**: Developer configuration template for signing identities, keystores, and appcast keys
- **Inspector polish**: cursor balance, selection persistence, highlight stability, scrollbar, positioning (#71)
- **Modular CLI**: `pulp_cli.cpp` refactored into per-command `cmd_*.cpp` files (#61)

### Fixed
- `modules.md` code examples now match the actual APIs (#70)
- Android emulator CI job disabled until infrastructure is ready (#77, #78)

### Changed
- Skill Maintenance Rule added to CLAUDE.md (#72)
- Ship CLI command manifest and `/ship` slash command updated with notarize/appcast/android subcommands

## [0.1.1] — 2026-04-06

### Added
- `pulp install` command for standalone SDK download without project scaffolding
- Auto-labeling workflow for new GitHub issues
- JavaScript engine architecture documentation (QuickJS, V8, JavaScriptCore)
- Audit tests for license and vendor checks
- Private audit hook — `tools/audit.py` auto-runs `.private/audit-naming.py` when available

### Fixed
- `pulp cache fetch skia` now downloads from the correct SDK release URL
- Docs link rewriting for anchored `.md` references (e.g., `modules.md#format`)
- AAX docs opening paragraph rewritten for clarity
- macOS platform support corrected to ARM64 only (not x86_64)
- Test count updated across all docs (1622+)
- DSL support status updated from "experimental" to shipped

### Changed
- Capabilities page now shows all three JS engines as available
- License audit workflow renamed for clarity
- Audit tool focused on license and vendor checks with extensible private hooks

## [0.1.0] — 2026-04-06

Initial release.

- 6 plugin formats: VST3, AU v2, AUv3, CLAP, LV2, Standalone (+ WAM/WebCLAP experimental)
- 5 platform builds: macOS ARM64, Linux x64, Linux ARM64, Windows x64, Windows ARM64
- 30+ DSP signal processors
- GPU rendering via Dawn + Skia Graphite
- JS-scripted UI with hot-reload (QuickJS default, V8/JSC available)
- Three.js native GPU demo
- FAUST, Cmajor, JSFX DSL integration
- Design import from Figma, Pencil, Stitch
- Full CLI: create, build, test, validate, ship, install, doctor, inspect, design
- MCP server with 11 tools
- Local + cloud CI (Namespace, GitHub Actions)
- Code signing, notarization, DMG/PKG packaging
- 1622+ automated tests
- Claude Code plugin with slash commands and skills
