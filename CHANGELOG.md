# Changelog

All notable changes to Pulp are documented here.

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
