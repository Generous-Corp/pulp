# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What Is This

Pulp is a cross-platform audio plugin and application framework. MIT-licensed. C++20 core, Swift on Apple, JS-scripted GPU UIs via Dawn/Skia/QuickJS. See `VISION.md` for the full picture.

---

## Build & Test Commands

```bash
# Configure (first time or after CMakeLists.txt changes)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug

# Build everything
cmake --build build -j$(sysctl -n hw.ncpu)

# Run all tests
ctest --test-dir build --output-on-failure

# Run tests matching a pattern
ctest --test-dir build -R "Knob"

# Run a single test binary directly
./build/test/pulp-test-state

# Build a specific target
cmake --build build --target pulp-test-state

# Sanitizer build (ASan/TSan/UBSan)
cmake -S . -B build -DPULP_SANITIZER=address
```

**External SDKs** (not committed, cloned at configure time or manually):
- VST3 SDK → `external/vst3sdk` (MIT, `git clone --depth 1 --branch v3.7.12`)
- AudioUnitSDK → `external/AudioUnitSDK` (Apache 2.0, `git clone --depth 1`)
- CLAP → fetched automatically via CMake FetchContent
- Skia → pre-built binaries in `external/skia-build/`

**CLI tool:**
```bash
./build/tools/cli/pulp build       # configure + build
./build/tools/cli/pulp test        # run test suite
./build/tools/cli/pulp validate    # run format validators (auval, clap-validator)
./build/tools/cli/pulp ship sign --identity "Developer ID Application: ..."
./build/tools/cli/pulp ship package --version 1.0.0
./build/tools/cli/pulp ship check  # show signing status
```

**Note:** All plugin formats build and pass tests, including PulpSynth CLAP.

---

## Architecture

### Plugin Flow

A Pulp plugin is a `Processor` subclass. Format adapters (VST3, AU, CLAP) wrap it:

```
Developer writes:
  MyPlugin : Processor
    ├── descriptor()        → plugin metadata, bus config
    ├── define_parameters() → register params in StateStore
    ├── prepare()           → allocate resources at given sample rate
    └── process()           → real-time audio callback

Format adapters translate:
  VST3: PulpVst3Processor (SingleComponentEffect) ↔ Processor
  AU v2: PulpAUEffect (AUEffectBase) ↔ Processor
  AU v3: PulpAudioUnit (AUAudioUnit) ↔ Processor
  CLAP: PulpClapPlugin (clap_plugin_t) ↔ Processor
```

### Thread Model

- **Audio thread**: reads params via `std::atomic<float>` (relaxed), processes buffers, pushes meter data via `TripleBuffer`
- **UI thread**: reads meter data, polls param changes via `Binding::poll()`, updates widgets
- **Main thread**: plugin init, state serialization, file I/O

Sync primitives (in `core/runtime/`):
- `std::atomic` — single independent values (parameters)
- `SeqLock<T>` — coherent multi-field reads (transport state)
- `TripleBuffer<T>` — latest-value publication (meter data, large config swaps)
- `SpscQueue<T>` (CHOC FIFO) — ordered event streams (MIDI, UI commands)

### Parameter System

`StateStore` holds all params as atomic `ParamValue` objects. Format adapters sync bidirectionally:
- Host → plugin: adapters write to StateStore in process()
- Plugin → host: adapters snapshot values before process(), emit output events for any changes after
- UI → host: `Binding` wraps a param with gesture begin/end for undo grouping
- CLAP modulation: `get_modulated()` returns base + mod_offset

### Subsystem Map

| Subsystem | Path | Purpose |
|-----------|------|---------|
| platform | `core/platform/` | OS detection, types |
| runtime | `core/runtime/` | Logging, assert, SeqLock, TripleBuffer, SpscQueue |
| events | `core/events/` | EventLoop, Timer |
| audio | `core/audio/` | BufferView (non-owning channel pointer wrapper) |
| midi | `core/midi/` | MidiEvent, MidiBuffer (via choc::midi) |
| state | `core/state/` | ParamValue, ParamInfo, StateStore, Binding, serialization |
| signal | `core/signal/` | 30+ DSP processors, math, interpolation, filter design |
| format | `core/format/` | Processor interface, VST3/AU/CLAP adapters, standalone |
| canvas | `core/canvas/` | 2D drawing API, RecordingCanvas, CoreGraphics/Skia backends |
| view | `core/view/` | View hierarchy, widgets, themes, JS bridge, AudioBridge |
| render | `core/render/` | WebGPU/Dawn surface, Skia Graphite context |
| osc | `core/osc/` | OSC 1.0 sender/receiver over UDP |
| ship | `ship/` | Codesign, notarization, DMG/PKG creation, appcast |

---

## Repo Standards

This repo will be open-sourced. Every commit, every file, every directory name should reflect that. No throwaway code on main. No "WIP" commits. No embarrassing history.

### Structure

```
pulp/
├── CLAUDE.md              # This file
├── VISION.md              # Public manifesto
├── LICENSE.md             # MIT
├── README.md              # Public-facing, concise
├── NOTICE.md              # Third-party attribution
├── DEPENDENCIES.md        # Tracked dependency inventory with licenses
├── CMakeLists.txt         # Root build
├── Package.swift          # SPM manifest (Apple)
│
├── core/                  # Cross-platform C++ subsystems
│   ├── runtime/
│   ├── events/
│   ├── audio/
│   ├── midi/
│   ├── signal/
│   ├── state/
│   ├── format/
│   ├── platform/
│   ├── canvas/
│   ├── render/
│   └── view/
│
├── apple/                 # Swift subsystems (Apple only)
├── inspect/               # Component inspector
├── tools/                 # Build tooling, CMake modules, scripts, templates
├── test/                  # Test framework and harnesses
├── ship/                  # Packaging, signing, distribution
├── ci/                    # GitHub Actions workflows
├── claude/                # Claude Code plugin (commands, skills)
├── examples/              # Example projects
├── external/              # Third-party dependencies (vendored or fetched)
└── docs/                  # PUBLIC documentation (getting-started, API, guides)
```

### What Does NOT Go in the Repo

Planning documents, spec drafts, audit notes, phase tracking, and internal design exploration stay **outside the repo** or in gitignored paths. The repo is the product. The planning is the process.

```
# In .gitignore:
/planning/           # Phase specs, status tracking, archived plans
/research/           # Audit notes, competitive analysis
*.DS_Store
```

Planning docs live in `planning/` locally. When a phase completes, its spec moves to `planning/archive/`. Status is tracked in `planning/STATUS.md`. None of this is committed.

---

## Development Methodology

### Branch Model

- **`main`** — always clean, always builds, always passes tests. Every commit on main should be something we're proud of.
- **`explore/*`** — exploration branches for prototyping uncertain ideas. Created in worktrees. Never merged directly to main.
- **`phase/*`** — implementation branches for planned work. Created from specs. Merged to main via PR when validated.
- **`fix/*`** — bug fixes, small improvements.

### Workflow: Exploration → Validation → Landing

1. **Explore** — create a worktree on `explore/topic`. Prototype freely. Use ralph-loop for iterative development. Break things. Learn.
2. **Validate** — when the exploration proves out, write a spec (in `planning/`). Define acceptance criteria. Write tests.
3. **Plan** — create `phase/name` branch from main. Implement against the spec. Follow the spec, don't freestyle.
4. **Test** — all tests pass, all validation criteria met, code reviewed.
5. **Land** — PR to main. Squash or rebase for clean history. Delete the worktree.

If an exploration doesn't work out, delete the worktree. No trace in main. No embarrassment.

### Parallel Work via Worktrees

Use git worktrees aggressively for parallel development:

```bash
# Create exploration worktree
git worktree add ../pulp-explore-render explore/render-engine

# Create phase implementation worktree
git worktree add ../pulp-phase-audio phase/audio-io

# List active worktrees
git worktree list

# Clean up after landing
git worktree remove ../pulp-phase-audio
```

Multiple explorations can run simultaneously. Multiple phases can be implemented in parallel if they don't share subsystems. The worktree-manager plugin handles this.

### Status Tracking

`planning/STATUS.md` tracks progress against the full framework spec:

```markdown
# Pulp Status

## Active Work
| Worktree | Branch | Phase | What | Owner | Started |
|----------|--------|-------|------|-------|---------|
| pulp-phase-runtime | phase/runtime | 2 | Core runtime | — | 2026-03-25 |
| pulp-explore-dawn | explore/dawn-skia | 6 | Dawn/Skia integration | — | 2026-03-25 |

## Completed Phases
| Phase | Landed | Notes |
|-------|--------|-------|
| 0 - Audit | 2026-03-24 | Spec complete, not committed |

## Blocked / Parked
(empty)
```

### Phase Specs

Each phase gets a spec document in `planning/phases/`:

```
planning/
├── STATUS.md
├── phases/
│   ├── phase-1-foundation.md
│   ├── phase-2-runtime.md
│   └── ...
└── archive/
    └── phase-0-audit.md    # Completed, moved here
```

Phase specs define: goals, deliverables, acceptance criteria, test plan, clean-room notes. Implementation follows the spec. Deviations get noted and the spec is updated.

---

## Clean-Room Discipline

### Rules

1. **Never reference JUCE source code while implementing Pulp.** Use RepoPrompt to reference our own code, external SDKs (VST3, AU, CLAP), and permissively-licensed projects. Never load JUCE headers or implementation files as context during implementation.

2. **JUCE is audit material only.** The audits in `planning/` describe observed capabilities. Implementation is from specs, SDK documentation, and original design. If you need to understand how a plugin format works, read the format SDK — not how another framework wraps it.

3. **No JUCE names in Pulp code.** No class names, module names, API names, file names, or naming patterns from JUCE. If a proposed name matches a JUCE name, rename it.

4. **Separate observation from design.** When writing specs: "The audited framework does X" → "Pulp requires Y" → "Pulp implements Z." Three distinct steps.

5. **Track all references.** When studying external code for inspiration (iPlug2, AudioKit, SignalKit, format SDKs), note it in the spec. This creates an audit trail.

### Reference Projects (Safe to Study)

| Project | License | Use For |
|---------|---------|---------|
| VST3 SDK | MIT | VST3 format implementation |
| AudioUnit SDK | Apache 2.0 | AU/AUv3 format implementation |
| CLAP | MIT | CLAP format implementation |
| LV2 SDK | ISC | LV2 format implementation |
| Dawn | BSD-3-Clause | WebGPU rendering |
| Skia | BSD-3-Clause | 2D GPU rendering |
| QuickJS | MIT | JS engine |
| Oboe | Apache 2.0 | Android audio |
| iPlug2 | zlib-like | Architecture inspiration (NOT code copying) |
| AudioKit | MIT | Swift audio patterns |
| SignalKit | MIT | Swift real-time DSP patterns |

### Projects NOT to Reference During Implementation

| Project | Why |
|---------|-----|
| JUCE | Restrictive license, clean-room target |
| AAX SDK | Proprietary (obtain independently if needed) |
| ASIO SDK | Proprietary (obtain independently if needed) |

---

## Dependency Management

### DEPENDENCIES.md

Every third-party dependency is tracked in `DEPENDENCIES.md` at the repo root:

```markdown
# Dependencies

| Name | Version | License | Bundled | Purpose | Added |
|------|---------|---------|---------|---------|-------|
| Dawn | main | BSD-3-Clause | FetchContent | WebGPU rendering | 2026-03-xx |
| Skia | m130 | BSD-3-Clause | FetchContent | 2D GPU rendering | 2026-03-xx |
| QuickJS | 2024-02-14 | MIT | Vendored | JS engine | 2026-03-xx |
| Catch2 | 3.x | BSL-1.0 | FetchContent | Testing | 2026-03-xx |
```

### License Policy

- **Allowed:** MIT, BSD (2/3-clause), Apache 2.0, ISC, zlib, BSL-1.0, public domain
- **Not allowed:** GPL, LGPL, AGPL, SSPL, proprietary, any copyleft
- **Review required:** MPL-2.0 (weak copyleft, case-by-case)

Before adding ANY dependency: check its license, add it to DEPENDENCIES.md, verify compatibility with MIT release. No exceptions.

### Attribution Ordering

DEPENDENCIES.md and NOTICE.md entries are always sorted **alphabetically by name**. When adding a new dependency, insert it in the correct alphabetical position — don't append to the end. This applies to both manual additions and `pulp add` / `pulp audit` operations.

---

## Testing and Validation

### Philosophy

If it's not tested, it doesn't work. Every subsystem has tests. Every feature has acceptance criteria. Every phase has a validation plan.

### Test Layers

| Layer | What | Tool | When |
|-------|------|------|------|
| Unit tests | Individual functions, classes | Catch2 | Every commit |
| Integration tests | Subsystem interactions | Catch2 + custom harnesses | Every PR |
| Format validation | Plugin loads correctly | CLAP dlopen, VST3 load tests; auval/pluginval/clap-validator for local use | Every PR touching format code |
| Audio golden-files | DSP output matches reference | Custom harness (bit-exact or tolerance) | Every PR touching signal code |
| Visual regression | UI renders correctly | Screenshot comparison | Every PR touching view/render code |
| Build matrix | Builds on all platforms | GitHub Actions CI | Every PR |
| DAW compatibility | Plugin works in real DAWs | Manual + automated (future) | Before releases |

### Platform-Specific Testing Tools

**macOS:**
- XcodeBuildMCP for building and running Xcode targets
- auval for Audio Unit validation
- `codesign --verify` and `spctl --assess` for signing validation
- Instruments for performance profiling

**Web/WASM:**
- chrome-devtools MCP for inspecting web builds
- Lighthouse for performance audits
- WebDriver for automated UI testing

**Cross-platform UI:**
- Screenshot capture and comparison (automated in CI)
- The component inspector (pulp-inspect) doubles as a validation tool
- WebDriver-based automation via tauri-plugin-webdriver patterns

**Audio:**
- Golden-file comparison: render known input → compare output against reference
- Round-trip state serialization tests
- Latency measurement: high-resolution timer in audio callback
- Buffer size stress tests (32 to 4096 samples)
- Sample rate tests (44.1k to 192k)

### Plugin Install Policy

**NEVER install a plugin to system folders without passing validation first.**

The build process for plugin formats follows this pipeline:

```
Build → Validate → Install
```

- `pulp build` — builds the plugin bundle(s) in the build directory
- `pulp build --test` — builds + runs validation (auval for AU if installed, pluginval for VST3 if installed, CLAP dlopen tests)
- `pulp build --install` — builds + validates + installs to system folders (only if validation passes)
- `pulp build --install --skip-validation` — builds + installs WITHOUT validation (for debugging adapter code only, never for normal use)

**System plugin folders** (where DAWs scan):
- AU: `~/Library/Audio/Plug-Ins/Components/`
- VST3: `~/Library/Audio/Plug-Ins/VST3/`
- CLAP: `~/Library/Audio/Plug-Ins/CLAP/`

A plugin that crashes a DAW during scan is worse than no plugin at all. Validation is the gate. The `--skip-validation` flag exists for debugging but defaults OFF.

### Test in Every Worktree

Tests must pass in the worktree before creating a PR. CI runs the full matrix on PR. No merging with red tests.

---

## RepoPrompt Usage

Use RepoPrompt to build context efficiently across Pulp's codebase. Key workflows:

- **Context building:** Use `context_builder` to understand subsystem interactions before making changes
- **Code review:** Use `context_builder` with `response_type="review"` after changes for thorough review
- **Cross-reference:** Use RepoPrompt to reference our own code, external SDK code, and safe-to-study projects
- **Never use RepoPrompt to load JUCE source as context during implementation**

RepoPrompt is available globally as an MCP server.

---

## Commit Standards

- Commits on main are clean and purposeful
- Commit messages: imperative mood, explain why not just what
- No "WIP", "fix", "stuff", "misc" commits on main
- Squash exploration work before landing
- Every commit should build and pass tests
- Sign commits if GPG is configured

---

## Working with AI Tools

### Claude Code
- Use the pulp-claude plugin commands for project workflows
- Use ralph-loop for sustained iterative work in worktrees
- Use explore branches for uncertain work so AI mistakes don't pollute main
- Provide specs as context when implementing (paste or reference via RepoPrompt)

### RepoPrompt
- Primary tool for building deep context about the codebase
- Use for code review, architecture questions, and cross-subsystem understanding
- Context persists for follow-up questions

### General
- AI tools are collaborators, not autopilots. Review all generated code.
- Tests validate AI output. Don't trust, verify.
- When AI suggests a name that matches JUCE naming, reject it and pick an original name.
