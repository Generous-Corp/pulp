"You are building the Pulp cross-platform audio plugin and application framework.

GOVERNING RULES:
- The file ~/Code/pulp/planning/STATUS.md defines the phased plan and post-v1 roadmap. Read it at the start of EVERY iteration.
- The file ~/Code/pulp/CLAUDE.md defines development methodology, clean-room rules, and testing requirements. Read it at the start of EVERY iteration.
- The file ~/Code/pulp/VISION.md defines what we're building and why. Reference it for architectural decisions.
- The file ~/Code/pulp/AGENTS.md defines docs maintenance rules. Follow them after every code change.
- All work happens in git worktrees, never directly on main.
- Branch names: phase/<name> for planned work, explore/<topic> for uncertain work, fix/<name> for bug fixes.
- You can access Windows via SSH using ssh win2; no Linux VM is set up yet, so skip that for now.

SOURCE OF TRUTH:
- ~/Code/pulp/planning/STATUS.md — tracks progress, phases 0-10 (complete), phases 11-20 (post-v1 roadmap)
- ~/Code/pulp/planning/ — all spec docs (architecture, capability matrix, rendering strategy, etc.)
- ~/Code/pulp/planning/GAP-analysis-juce-vs-pulp.md — feature gaps vs industry standard (TRUE GAPs, PARTIAL GAPs, INTENTIONAL PASSes)
- ~/Code/pulp/planning/docs-industry-parity-plan.md — docs gap analysis against industry standard frameworks
- ~/Code/pulp/planning/docs-site-examples-correctness-hardening-spec.md — docs site spec (completed)
- ~/Code/pulp/planning/module-maturity-closure-plan.md — explicit path from current module statuses to mature public-ready status
- ~/Code/pulp/planning/audio-validation-audit-and-hardening-plan.md — current-state audit plus concrete hardening plan for deterministic audio tests, validators, and DAW regression coverage
- ~/Code/pulp/DEPENDENCIES.md — dependency tracking (update when adding any dependency)
- ~/Code/pulp/docs/status/ — machine-readable YAML manifests (update when behavior changes)

REFERENCE PROJECTS (read-only, for patterns and inspiration):
- ~/Code/pulp/planning/02-juce-audit.md — what the audited framework does (capabilities to match)
- ~/Code/pulp/planning/04-iplug3-review.md — architecture inspiration
- ~/Code/pulp/planning/08-architecture-spec.md — subsystem design
- ~/Code/pulp/planning/12-rendering-strategy.md — Dawn/Skia/QuickJS rendering
- Use RepoPrompt to reference code from safe-to-study projects (VST3 SDK, AU SDK, CLAP, iPlug2, AudioKit, SignalKit, Dawn, Skia, QuickJS). NEVER load JUCE source as implementation context.
- Use RepoPrompt context_builder for deep codebase analysis, architecture planning, and code review before making changes.

CURRENT STATE (v1 complete):
- 12 subsystems active (platform, runtime, events, state, audio, midi, osc, signal, format, canvas, view, render)
- 3 plugin formats working (VST3, AU v2, CLAP) + standalone + headless
- 19 DSP processors in signal library
- 7 example projects (PulpGain, PulpTone, PulpEffect, PulpCompressor, PulpSynth, PulpDrums, UI Preview)
- 265+ tests passing
- GPU rendering (Dawn/Metal/Skia Graphite) experimental
- CLI: build, test, validate, status, clean, ship, docs
- Docs: local-first system with YAML manifests, static site, CI consistency checks
- Shipping: codesign, notarization, DMG/PKG, appcast

SAMPLE PROJECTS:
- PulpGain — stereo gain effect (reference example, all formats)
- PulpTone — polyphonic oscillator synth with MIDI (all formats)
- PulpEffect — biquad filter with diverse parameters (all formats)
- PulpCompressor — sidechain compressor with multi-bus (all formats)
- PulpSynth — macro oscillator synth using signal DSP library (CLAP only)
- PulpDrums — generative drum sequencer MIDI effect (CLAP only)
- UI Preview — standalone app for view/widget rendering pipeline (macOS only)
- PulpSampler — DEFERRED (requires Waveform Editor component from Phase 14 + audio file loading UI)

POST-V1 ROADMAP (phases 11-20):

### Phase 11: Documentation Parity
- [ ] Doxygen API reference from public headers
- [ ] Format-specific guide (VST3 params, AU sandbox, CLAP modulation)
- [ ] DSP algorithm usage guide (per-processor examples)
- [ ] State/parameter system deep-dive (ParamValue API, serialization)
- [ ] Module deep-dive guides (runtime, canvas, view, render, osc)
- [ ] Platform guide: macOS signing/notarization end-to-end
- [ ] Shipping guide: full deployment workflow
- [ ] Testing deep-dive: HeadlessHost API, sanitizer setup, golden-files

### Phase 12: Developer Tooling & CLI
- [ ] Auto-generated parameter UI (generic editor from parameter definitions)
- [ ] `pulp create` — project scaffolding from templates
- [ ] `pulp inspect` — live component inspector launch
- [ ] Plugin-as-CLI interface (batch processing, headless rendering)
- [ ] MCP server per-plugin interface (AI agent parameter control)
- [ ] `pulp audit` — license/clean-room audit tool
- [ ] `pulp add component` — component installer

### Phase 13: AUv3 + Swift Layer
- [ ] AUv3 format adapter (macOS)
- [ ] Swift/C++ interop bridge (Swift 5.9+ direct interop)
- [ ] SwiftUI alternative UI path (Apple platforms)
- [ ] iOS target foundation (AVAudioSession, AUv3)
- [ ] iOS GPU rendering (Dawn/Metal on iOS)

### Phase 14: Core UI Components
Keyboard & Input Foundation (required before widgets — see ~/Code/visage for patterns):
- [ ] MouseEvent with modifiers (Cmd, Ctrl, Shift, Alt/Option, Meta — like visage MouseEvent)
- [ ] KeyEvent with modifiers and key codes (like visage KeyEvent with KeyCode enum + int mods)
- [ ] handleTextInput(string) — separate from key events, for IME and composed characters
- [ ] Multi-pointer/multi-touch support (pointer_id on all mouse events, like visage fork PR #17)
  - iOS: UITouch → stable pointer_id via NSMapTable, next_pointer_id reset on all-up
  - Desktop: pointer_id=0 always (single pointer)
  - Enables simultaneous knob/fader manipulation on iOS
- [ ] Modal window support (Esc to close, focus trapping, overlay dimming)
- [ ] Key command routing (Cmd+C/V/X/Z/A dispatched to focused widget)

Widgets:
- [ ] Text input widget — full TextEditor (see ~/Code/visage visage_widgets/text_editor.h):
  - Focus/defocus with select-on-focus option
  - Text selection (click, shift-click, double-click word, triple-click line)
  - Clipboard: Cmd+C copy, Cmd+V paste, Cmd+X cut, Cmd+A select all
  - Undo/redo with history stack (Cmd+Z, Cmd+Shift+Z), max 1000 entries
  - Cursor movement (arrows, Cmd+Left/Right line start/end, Option+arrows word jump)
  - Up/Down in single-line: move to start/end (visage fix b0b2ee8)
  - Numeric-only mode for parameter value entry
  - Password mode (masked characters)
  - Dead key / accent composition (acute, grave, tilde, umlaut, circumflex)
  - Return/Escape to confirm/cancel editing
  - Multi-line mode with scroll support
- [ ] Animation system (JS-based tweens, easing, property interpolation)
- [ ] ComboBox / dropdown widget (enumerated parameter selection)
- [ ] ListBox + scrolling viewport (scrollable content containers)
- [ ] TabPanel / tabbed component (multi-page plugin UIs)
- [ ] Tooltip support (hover tooltips on all widgets)
- [ ] ProgressBar widget (async operation feedback)
- [ ] CallOutBox / alert dialogs (confirmations, notifications)
- [ ] Parameter attachment helpers (widget-to-parameter binding with normalization)

Preset Management (built-in — see ~/Code/PlunderTube/Source/PresetManager.h for real-world patterns):
- [ ] PresetManager — save/load/delete/rename/import presets
- [ ] Factory vs user preset separation with recursive folder scanning
- [ ] Current preset tracking with unsaved-changes detection
- [ ] Preset file format (JSON-based, extensible, supports custom plugin data)
- [ ] Preset Browser UI component (categories, search, favorites)
- [ ] DAW-integrated preset switching (format adapters expose preset list to host)
- [ ] Smart storage locations (platform-standard: ~/Library/Audio/Presets/ on macOS, %APPDATA% on Windows, ~/.config/ on Linux)

Application Framework (standalone):
- [ ] MenuBar abstraction (native NSMenu/Win32/GTK, keyboard shortcuts)
- [ ] Toolbar abstraction (native NSToolbar/CommandBar, customizable items)
- [ ] Keyboard shortcut manager (KeyMapping with configurable bindings)
- [ ] Audio Device Selector component (device picker, sample rate, buffer size)
- [ ] Musical Typing component (keyboard-as-MIDI, on-screen piano)
- [ ] Application settings persistence (window size/position, device selection, preferences)
- [ ] Multi-instance support (each plugin instance gets independent state, DAW restores correctly)
- [ ] Window state restoration (remember size, position, display — standalone and plugin editor)

Design & Showcase:
- [ ] Widget Showcase app — interactive demo of all Pulp widgets (like ~/Code/visage/examples/Showcase)
  - Every widget type rendered in a single app with live theming
  - Compilable to WASM for web demo at generouscorp.com/pulp/showcase/
  - Design tokens are structured data so AI can read and write them
- [ ] AI Style Designer integration — natural language theming (inspired by ~/Code/ai-style-designer)
  - Chat input in the showcase: describe a style ("80s Macintosh", "neon cyberpunk"), preview updates live
  - Scoped prompts: Cmd+click a widget to restyle just that component type
  - Style history with snapshot restore
  - Uses Claude CLI or MCP — no separate API key needed
- [ ] Design token export (JSON, CSS, C++ headers, shader uniforms, OKLCH)
- [ ] Multi-window support (floating palettes, inspectors)
- [ ] `pulp design` — AI design session CLI (wraps the showcase with chat)

### Phase 15: Windows Platform (use `ssh win2` for runtime testing)
- [ ] Windows audio I/O (WASAPI)
- [ ] Windows MIDI I/O (Win32 MIDI, via libremidi)
- [ ] D3D12 GPU backend (Dawn already supports it)
- [ ] Windows CI matrix (GitHub Actions)
- [ ] Windows Authenticode / Azure Trusted Signing
- [ ] Windows installer (MSI or NSIS)
- [ ] Platform guide: Windows build and deployment

### Phase 16: Linux Platform (use Docker or cloud VM for runtime testing)
- [ ] Linux audio I/O (ALSA, JACK)
- [ ] Linux MIDI I/O (ALSA MIDI, via libremidi)
- [ ] Vulkan GPU backend (Dawn already supports it)
- [ ] LV2 format adapter
- [ ] Linux CI matrix (GitHub Actions)
- [ ] Linux packaging (.deb, AppImage)
- [ ] Platform guide: Linux dependencies and deployment

### Phase 17: Advanced Examples
- [ ] PulpSampler — audio file sampler with waveform editor
- [ ] Waveform Editor component
- [ ] Diagnostic Reporter component
- [ ] WebView embedding (Monaco editor, docs panels)
- [ ] Example screenshots and visual documentation

### Phase 18: Web/WASM Target
- [ ] Emscripten/WASM build pipeline
- [ ] Web Audio API integration
- [ ] Web MIDI API integration
- [ ] WebGPU native browser backend
- [ ] WebCLAP / WAMv2 plugin format (when specs stabilize)
- [ ] WASM CI build and deploy workflow

### Phase 19: Advanced Features & Bindings
DSP expansion:
- [ ] FIR filter, BallisticsFilter, LogRampedValue, high-quality interpolators
- [ ] ProcessorChain, LookupTable, first-order TPT, FilterDesign utilities
MIDI expansion:
- [ ] RPN/NRPN parser, MidiKeyboardState, MIDI 2.0 UMP / MPE
Audio utilities:
- [ ] AudioProcessLoadMeasurer, BufferingAudioReader, AudioWorkgroup
Platform:
- [ ] Plugin hosting (pulp-host: scanner, plugin slot, signal graph)
- [ ] Windows accessibility (UI Automation), Linux accessibility (AT-SPI)
- [ ] TreeView widget, Undo/redo system
- [ ] Python bindings (HeadlessHost via pybind11)
- [ ] Node.js bindings (HeadlessHost via napi)

### Phase 20: AAX + Remaining Formats
- [ ] AAX format adapter (macOS + Windows, requires Avid SDK)
- [ ] ASIO audio I/O (Windows, requires Steinberg SDK)

PLATFORM DEVELOPMENT STRATEGY:
- macOS is primary development platform — write all code here
- Windows runtime testing via `ssh win2` (VM accessible over SSH)
- Linux runtime testing via Docker or GitHub Actions
- Cross-platform code pattern: write on macOS → CI validates on all platforms → fix from CI feedback
- Items that genuinely need a VM: WASAPI/ALSA/JACK audio I/O runtime testing, D3D12/Vulkan GPU validation, platform signing, installer testing
- Everything else: macOS write, CI test

DOCS MAINTENANCE (MANDATORY):
After any code change that affects public behavior:
1. Update relevant YAML manifests in docs/status/
2. Update relevant Markdown docs in docs/
3. Run `tools/check-docs.sh` to validate consistency
4. The CI docs-check workflow will catch drift on PR

PRIORITY ORDER:
1. Phase 11 — documentation parity (highest leverage, blocks adoption)
2. Phase 12 — developer tooling (accelerates everything after)
3. Phase 13 — AUv3 + Swift (highest-value format gap, macOS-only)
4. Phase 14 — core UI components (enables richer examples)
5. Phases 15-20 — platform expansion and advanced features

INTENTIONAL PASSES (do NOT implement):
- Product unlocking/licensing (use third-party services)
- Analytics/telemetry (use dedicated SDKs like Sentry/PostHog)
- Video/camera (not aligned with audio focus)
- Audio CD reading (obsolete)
- OpenGL (replaced by WebGPU/Dawn/Skia — architectural decision)
- Box2D physics (novelty, not framework concern)
- Custom cryptography (use platform crypto APIs)
- VST2/Unity/ARA formats (dead SDK / niche / post-launch consideration)

WORKING REPO:
- ~/Code/pulp — main branch (clean, committed, public-ready)
- Worktrees created as ~/Code/pulp-<name>/ (e.g., ~/Code/pulp-phase-docs/)

WORKTREE MANAGEMENT:
- Create worktrees for each active phase:
  - git worktree add ~/Code/pulp-phase-docs -b phase/docs-parity
  - git worktree add ~/Code/pulp-phase-tooling -b phase/dev-tooling
  - etc.
- For explorations: git worktree add ~/Code/pulp-explore-<topic> -b explore/<topic>
- When a phase is complete: merge to main (via PR or direct merge), remove worktree.
- Use /worktree-manager:create and /worktree-manager:cleanup for worktree lifecycle.

PARALLELIZATION:
- Use /codex <task> for parallel work that would speed things up.
- Good for Codex: Doxygen setup, documentation stubs, CMake modules, test fixtures, CI workflow YAML, stub implementations for Windows/Linux.
- Bad for Codex: core architecture decisions, format adapter integration, sync primitive implementation.
- Run Codex in background and continue your own work.
- Check Codex output and test before marking work complete.

IMPLEMENTATION RULES:
- C++20 standard. Use std:: where possible. Don't reinvent the standard library.
- Every subsystem has its own CMakeLists.txt with explicit dependency declarations.
- Platform-specific code goes in platform/ subdirectories or behind #ifdef guards.
- Public headers in include/pulp/<subsystem>/. Private implementation in src/.
- All naming is original Pulp vocabulary. NO names from the audited framework (JUCE).
- Design tokens are JSON. Themes are data, not code.
- JS UI files are loaded at runtime, not compiled into binaries.

CLEAN-ROOM DISCIPLINE:
- You can use RepoPrompt to understand how things are done in JUCE /Users/danielraffel/Code/JUCE
- You can: Read the JUCE source; Learn patterns, architecture, APIs; Reimplement similar ideas in your own code
- You should not: Copy/paste any JUCE code; Derive code too directly (recognizable structure/implementation)
- NEVER reference JUCE source code during implementation. Use format SDKs and safe-to-study projects.
- If a proposed name matches a JUCE name, reject it and pick an original name.
- When adding any dependency: check license first, add to DEPENDENCIES.md (alphabetical), add to NOTICE.md (alphabetical).
- Only MIT, BSD, Apache 2.0, ISC, zlib, BSL-1.0 licensed dependencies. NO copyleft.

TESTING (MANDATORY):
- Every subsystem has unit tests (Catch2).
- After any build system change: cmake -B build && cmake --build build (must succeed).
- After audio code changes: run golden-file comparison tests.
- After format adapter changes: run format validators (auval for AU, pluginval for VST3, clap-validator for CLAP).
- After docs changes: run tools/check-docs.sh
- Document test results in STATUS.md.
- If tests fail, fix them before moving on. No skipping broken tests.

GIT DISCIPLINE:
- Commit at the end of EVERY iteration where changes were made.
- Commits must be clean, focused, and describe what was built (imperative mood, explain WHY).
- No "WIP", "fix", "stuff" commits. Every commit should be something we're proud of.
- Work on the appropriate branch in its worktree. Create branch if it doesn't exist.
- Do NOT push to remote unless asked or merging a completed phase.
- When merging to main: squash or rebase for clean history.

EACH ITERATION MUST:
1. Read planning/STATUS.md for current progress
2. Identify the NEXT incomplete deliverable from PRIORITY ORDER
3. Ensure correct worktree and branch exist (create if needed)
4. Implement the deliverable
5. Write or update tests
6. Build and verify: cmake -B build && cmake --build build
7. Run relevant tests and tools/check-docs.sh if docs changed
8. Commit changes with a clean message
9. Update planning/STATUS.md with progress
10. Summarize what was done

COMPLETION CONDITION:
- Current phase has all deliverables implemented or blockers documented
- All tests pass
- Docs are consistent (tools/check-docs.sh passes)
- STATUS.md reflects completion
- Clean commit history on the branch
- Ready to merge to main

ONLY WHEN ALL CONDITIONS ARE MET:
Output exactly: PHASE COMPLETE

IF STUCK:
- After 5 iterations without progress on a deliverable, document in STATUS.md
- Consider creating an explore/ branch to prototype a solution
- If truly stuck, ask the user for help" --completion-promise "DONE" --max-iterations 120
