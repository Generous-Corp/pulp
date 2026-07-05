# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What Is This

Pulp is a cross-platform audio plugin and application framework. MIT-licensed. C++20 core, Swift on Apple, JS-scripted GPU UIs via Dawn/Skia/QuickJS. See `VISION.md` for the full picture.

---

## Build & Test Commands

### Build type — Release is the default

**Always build Release unless you're actively investigating a bug.** A
Debug build of a JS-scripted GPU UI is dramatically slower than its
Release equivalent (no -O3, no NDEBUG, asserts live, no inlining of
canvas / Skia / Yoga / QuickJS) — slow enough that a UX-perceived
regression in a Debug build is almost always the build type, not the
code. The Codify-Release work made `pulp build` default to Release;
match that convention when reaching for raw `cmake` too.

Rules of thumb:
- **Default**: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`. Use
  `pulp build` (the CLI) when possible — it pins Release for you and
  refuses to silently flip.
- **Flip to Debug only when**: stepping in a debugger, capturing fresh
  `runtime::log_info` traces, repro'ing a sanitizer hit, or running
  `validate-build.sh` for a clean detached reconfigure pass. Restore
  Release immediately when the investigation ends.
- **Reconfigure gotcha**: a bare `cmake -S . -B build` (no build-type
  flag) usually preserves the cache value, BUT something in the
  shipyard / pre-push gate / rebase paths can silently reset the cache
  to Debug. Pass `-DCMAKE_BUILD_TYPE=Release` every reconfigure, or
  use `pulp build`.
- **Verify before reporting a build is Release**: BOTH
  `grep '^CMAKE_BUILD_TYPE' build/CMakeCache.txt` (should print
  `Release`) AND `grep '^CXX_FLAGS '
  build/<dir>/CMakeFiles/<target>.dir/flags.make` (should contain
  `-O3 -DNDEBUG`) must check out. Checking only the cache field is
  necessary-but-not-sufficient — it has been wrong by itself.
- **A struct-layout change must compile EVERYWHERE before shipping**: when a
  change alters the field layout of a struct that is brace-initialized
  positionally (adding/removing a field, swapping a `bool` for an `enum`), run a
  FULL `cmake --build build -j$(sysctl -n hw.ncpu)` over ALL targets — not just
  the named feature/parity targets — and run it in the background (a full build
  exceeds a short foreground budget). Stray positional inits in unrelated test
  files fail *closed* at compile time, so they are safe, but only a full build
  surfaces them; building a subset and shipping pushes the discovery to CI. A
  uniform build failure across macOS/Linux/Windows is the tell for a real
  compile miss, versus the macOS-only stale-build-dir ODR the `pulp-runner-ops`
  skill handles.

```bash
# Configure (first time or after CMakeLists.txt changes)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# Or preferred for repo + example builds:
./build/pulp build

# Build everything
cmake --build build -j$(sysctl -n hw.ncpu)

# Run all tests
ctest --test-dir build --output-on-failure

# Run an outer-loop clean validation in a detached worktree
./validate-build.sh

# Audit dependency pins / notice coverage
python3 tools/deps/audit.py --strict

# Audit current pins against upstream refs/tags
python3 tools/deps/audit.py --check-upstream --format markdown

# Run outer-loop validation locally and on any configured SSH hosts
python3 tools/deps/validate_hosts.py

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
- VST3 SDK → `external/vst3sdk` (MIT, `git clone --depth 1 --branch v3.7.12_build_20`)
- AudioUnitSDK → `external/AudioUnitSDK` (Apache 2.0, `git clone --depth 1`)
- CLAP → fetched automatically via CMake FetchContent
- Skia → pre-built binaries in `external/skia-build/`

**CLI tool (source tree):**
```bash
./build/pulp build       # configure + build
./build/pulp test        # run test suite
./build/pulp validate    # run format validators (auval, clap-validator)
./build/pulp ship sign --identity "Developer ID Application: ..."
./build/pulp ship package --version 1.0.0
./build/pulp ship check  # show signing status
./build/pulp version             # show SDK and project version
./build/pulp version bump patch  # bump version
./build/pulp version check       # verify version consistency
./build/pulp dev --test          # watch + rebuild + test loop
./build/pulp build --watch       # watch + rebuild loop
```

### Non-interactive signing + notarization (no keychain / 1Password prompt)

`codesign` / `notarytool` pop a macOS keychain "allow access" dialog or a
1Password prompt when the signing key is in the *login* keychain or notarytool
runs off a keychain *profile* — which wedges any headless / SSH / CI sign. The
hardened, codified solution:

```bash
pulp ship doctor                 # self-heal: dedicated signing keychain + validate .p8 (offline)
pulp ship doctor --check-online  # also prove the .p8 vs Apple (read-only) + refresh pulp-notary profile
```

`pulp ship doctor` (script: `tools/scripts/ensure_signing_ready.sh`, tests in
`tools/scripts/test_ensure_signing_ready.sh`) materializes a **dedicated signing
keychain** authorized for `codesign` via `set-key-partition-list` (never the
login keychain / 1Password), and verifies a **file-based App Store Connect `.p8`
notary key** (no keychain to lock or lose). `pulp ship sign` runs it as a
best-effort quiet preflight. The working recipe: sign from the dedicated keychain
with the identity hash + `--options runtime --timestamp` (inner dylibs/frameworks
first), then `notarytool submit --key <.p8> --wait`, `stapler staple`, `spctl
--assess`. **Secrets live in `~/.config/pulp/secrets/` (`keychain.env` +
`notary.env`), never in the repo**; env vars of the same name override the files.

After the Rust CLI cutover, source builds produce `./build/pulp` as
the user-facing CLI and `./build/tools/cli/pulp-cpp` as the C++
delegate for commands that still live in C++.

**Note:** All plugin formats build and pass tests, including PulpSynth CLAP.

### Outer-Loop Validation Cadence

Use the normal incremental `build/` loop for most work, but periodically run a clean detached validation pass with `./validate-build.sh`.

- Run it after changes to `CMakeLists.txt`, `setup.sh`, toolchain/bootstrap logic, packaging/install/export code, dependency wiring, or cross-platform process/spawn code.
- Run it before landing broad or risky slices, even if incremental builds are green.
- Prefer the default quiet mode in agent loops so success stays silent and only failure logs appear.
- Use `./validate-build.sh --verbose` when a human is actively watching or when debugging the validator itself.

### Dependency Update Workflow

- Treat dependency changes as non-trivial work. Do them on a branch, never directly on `main`.
- `tools/deps/manifest.json` is the machine-readable dependency inventory and should be updated alongside any pin/version change.
- `python3 tools/deps/audit.py --strict --check-upstream` is the first check before and after a dependency bump.
- `python3 tools/deps/validate_hosts.py` is the local multi-host validation lane. It always validates locally and can also validate over SSH via `tools/deps/hosts.local.json`.
- Keep `DEPENDENCIES.md` and `NOTICE.md` in sync with the manifest whenever dependency inventory changes.
- For FetchContent dependencies, prefer exact tags or commits over floating branches on stable lanes. If a dependency is intentionally floating, call that out explicitly in the manifest and docs.

When asked to "check for dependency updates" or "update pinned binaries", use this sequence:
1. Run `python3 tools/deps/audit.py --strict --check-upstream --format markdown` to identify drift.
2. Bump only the chosen dependency pins on a branch, and update `tools/deps/manifest.json` plus any affected docs/notices in the same change.
3. Run `python3 tools/deps/validate_hosts.py` and any risk-appropriate local tests before proposing a merge to `main`.
4. Merge only after the updated pins and validation results are explicitly summarized.

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

### Choosing a Processing Model (Processor vs SignalGraph)

Pulp has one DSP **authoring** model and one **composition** engine. Default to
the authoring model; reach for the graph only when routing is dynamic at run time.

- **Default to `Processor`** for any plugin, effect, instrument, MIDI effect, or
  agent-generated DSP. One plugin = one `Processor`; compose internal multi-stage
  DSP from `pulp::signal::*` helpers inside `process()`.
- **Reach for `SignalGraph` only when the routing itself is runtime-dynamic:**
  hosting external plugins, a user-editable rack/mixer/node-editor, or a topology
  loaded/saved as `.pulpgraph`. A graph node *wraps* a built unit — it is not a way
  to author DSP.
- **"Fixed topology" can still be complex.** Polyphony (synths/samplers),
  multi-bus inputs, sidechain, internal parallel/serial chains, mid/side,
  oversampling, and convolution are **all `Processor` concerns** — not reasons to
  reach for `SignalGraph`. Never express a one-in/one-out (or any fixed) chain as a
  graph without a stated runtime-routing reason.
- **Heuristic:** fixed at build time → `Processor`; edited/loaded at run time →
  `SignalGraph`.
- A `CustomNodeType` is a graph utility node, **not** a plugin authoring surface.

Full guidance and reserved terminology: `docs/reference/processing-models.md`.
Run `python3 tools/scripts/processing_model_terms_lint.py` to check terminology.

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
| platform | `core/platform/` | OS detection, types, Windows registry |
| runtime | `core/runtime/` | Logging, SIMD (Highway), XML, ZIP, HTTP, sockets, pipes, base64, crypto (mbedTLS), i18n, analytics, licensing, BigInteger |
| events | `core/events/` | EventLoop, Timer, AsyncUpdater, InterprocessConnection, ChildProcessManager, NetworkServiceDiscovery |
| audio | `core/audio/` | BufferView, FormatRegistry (WAV/FLAC/MP3/OGG/AIFF/AAC), ChannelSet, OfflineProcessor, SystemVolume |
| midi | `core/midi/` | MidiEvent, MidiBuffer (via choc::midi), MIDI CI (discovery, profiles, properties) |
| state | `core/state/` | ParamValue, ParamInfo, StateStore, Binding, StateTree, PropertiesFile, CachedProperty |
| signal | `core/signal/` | 30+ DSP processors, math, SIMD buffer, DryWetMixer, matrix, special functions |
| format | `core/format/` | Processor interface, VST3/AU/CLAP adapters, standalone |
| canvas | `core/canvas/` | 2D drawing API, TextShaper (PreText-style), AttributedString, RectangleList, ImageConvolution |
| view | `core/view/` | View hierarchy, widgets, themes, JS bridge, AudioBridge, accessibility interfaces |
| render | `core/render/` | WebGPU/Dawn surface, Skia Graphite context |
| osc | `core/osc/` | OSC 1.0 sender/receiver over UDP, bundles, address pattern matching |
| ship | `ship/` | Codesign, notarization, DMG/PKG creation, appcast |

### Text Shaping

Pulp uses a PreText-inspired text layout architecture (measure once, reflow forever):

- **`PULP_TEXT_SHAPING`** CMake option — default ON with GPU, OFF without
- `TextShaper::prepare()` — expensive: shapes text via SkFont/HarfBuzz, caches segment widths
- `TextShaper::layout()` — cheap: pure arithmetic over cached widths, no font calls
- HarfBuzz, ICU, and SkParagraph are bundled in the Skia pre-built binaries — no separate vendoring needed
- Fallback to character-width estimation when Skia unavailable

### Layout Model — Flex + Grid only (deliberate)

Pulp's layout engine is **Yoga** (`external/yoga/`), so the layout primitives are exactly what Yoga supports: **CSS Flexbox + CSS Grid** (Grid landed in Yoga 3.0, wired in PR #1509). CSS block flow, table layout, multi-column, floats, and print pagination are **out of scope by design**.

When implementing a feature or assessing a compat gap, **don't reach for non-Yoga layout primitives**. If a property in `compat.json` is marked `wontfix` because it requires block/inline/table/multi-column/float/print primitives, that's the architectural ceiling — not a bug to fix. Reasons:

1. **Yoga is the engine.** Adding non-Yoga layout means either a parallel layout engine (double maintenance) or block/table/columns reimplemented over Yoga (massive refactor).
2. **React Native parity.** RN is also flex+grid-only. `@pulp/react` and the JS bridge depend on this contract.
3. **GPU-first render pipeline.** Skia/Dawn favor a single tree-walk layout pass; multi-pass sequential algorithms (block flow, table cell auto-sizing, multi-column balancing, float wrap, paginated layout) don't compose with the render pipeline cleanly.
4. **Use case is structured panels.** Pulp targets audio plugin UIs + cross-platform apps — knobs, faders, side panels, popovers. Newspaper columns and complex tables are not real Pulp use cases.
5. **Modern design tools output flex/grid.** Figma, Stitch, v0, Pencil all produce flex containers + grid as their default layout primitives. Tooling alignment matters for the design-import path.

The honest tradeoff: Pulp will never be a general-purpose web browser. It IS the right shape for cross-platform native UI. CSS coverage at ~95% raw / ~100% on non-OOS denominator IS the architectural ceiling, not a goal to push past by adding layout primitives.

For the user-facing version of this rationale, see `docs/reference/layout-model.md`.

---

## Repo Standards

This repo will be open-sourced. Every commit, every file, every directory name should reflect that. No throwaway code on main. No "WIP" commits. No embarrassing history.

`tools/scripts/docs_noise_lint.py` guards the repo against stale workflow breadcrumbs in long-lived docs and comments.
Long-lived docs and source comments should explain current behavior, invariants, and upstream/vendor quirks — not workflow history.
Transient issue/PR/wave/handoff references belong in `planning/`, `docs/migrations/`, `docs/reports/`, or the changelog.

**This applies to source code too — comments AND test names/tags, not just docs.** Do NOT write phase/PR/issue/wave/handoff breadcrumbs in `core/`, `test/`, or any shipped source. Specifically forbidden in code: `(Phase N)` / `Phase N will…` / `4f`-style sub-phase labels, `[phaseN]` Catch2 tags, "sub-PR"/"slice N of", and bare `#1234` issue/PR references. Write the comment as a present-tense statement of what the code does or a neutral capability note (e.g. "feedback needs a previous-block slot" — not "Phase 4d adds feedback"). A test tag should say what it covers (`[parity]`, `[rt-safety]`), never which session shipped it. The phase/PR narrative goes in the **commit message** and the **planning submodule**, where reviewers expect it. (`docs_noise_lint.py`'s diff-scoped scan enforces this for docs/skills today; source-comment enforcement is the author's responsibility until the lint's source scope lands.)

### Verify Against Code, Not Planning Docs

When assessing what features exist or what claims are accurate, **always check the actual code on the current branch** — never trust planning documents, phase trackers, or status files as the source of truth. Planning docs describe intent; the code describes reality. If there is a conflict, the code wins. This applies to:

- Feature status assessments (check `git ls-tree`, read source files)
- Capability claims in docs (verify against actual implementations)
- Phase completion judgments (check what's merged to main, not what a planning doc says)

Run `git ls-tree -r --name-only origin/main <path>` or `git log --oneline origin/main -- <path>` to verify claims before reporting them.

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
├── docs/                  # PUBLIC documentation (getting-started, API, guides)
└── planning/              # Private submodule (pulp-planning) — specs, roadmaps, assessments
```

### What Does NOT Go in the Public Repo

Internal planning — capability assessments, feature specs, roadmaps, phase tracking, and design exploration — lives in the `planning/` submodule (private repo `pulp-planning`). This separation keeps the public repo focused on code, docs, and examples.

```
planning/            # Private submodule — specs, assessments, roadmaps, research, phase tracking
```

When a phase completes, its spec moves to `planning/archive/`. Status is tracked in `planning/STATUS.md`. The `planning/` submodule is optional — external cloners can build and use Pulp without it.

---

## Development Methodology

### Branch Model

- **`main`** — always clean, always builds, always passes tests. Every commit on main should be something we're proud of.
- **`explore/*`** — exploration branches for prototyping uncertain ideas. Created in worktrees. Never merged directly to main.
- **`feature/*`** — the default branch for non-trivial implementation work. Use this for focused features, fixes, and polish slices.
- **`develop/*`** — integration branches for complex, multi-piece features that need extended testing before merging to main. Feature branches merge to the develop branch first via PR; the develop branch merges to main at phase boundaries after full CI validation. Same quality bar as main.
- **`phase/*`** — optional name for a larger, spec-driven feature branch tied to a concrete planning document. This is a special case of feature work, not a separate workflow.

### Default Branch Discipline

- Default to a branch for any non-trivial change. Use `feature/*` unless the work is large enough to deserve a spec-driven `phase/*` branch or a `develop/*` integration branch.
- Treat `main` as a landing branch, not a scratch branch.
- **Never push directly to main. No exceptions.** Every change must go through: branch → PR → CI green → merge.
- Before merging to `main`, have high confidence the slice is stable: targeted tests pass, higher-risk changes get a clean detached validation pass with `./validate-build.sh`, and docs/status stay in sync.
- When multiple people or agents are active in the repo, prefer branches and worktrees even for medium-sized changes to reduce accidental breakage and merge confusion.

### Workflow: Exploration → Validation → Landing

1. **Explore** — create a worktree on `explore/topic`. Prototype freely. Use ralph-loop for iterative development. Break things. Learn.
2. **Validate** — when the exploration proves out, write a spec (in `planning/`). Define acceptance criteria. Write tests.
3. **Plan** — create `feature/name` from main for ordinary implementation, or `phase/name` if the work is large and tied to a planning doc. Implement against the spec when one exists. Follow the spec, don't freestyle.
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

When creating a fresh worktree for a task that references `planning/`, initialize the planning submodule before reading specs or handoffs:

```bash
git submodule update --init planning
```

If a named planning file is still missing after initialization, verify the current planning checkout or source planning repo before treating the file as nonexistent.

Fresh worktrees may also have only `external/skia-build/` headers and
`VERSION.md`, without the platform static libraries. For work that needs
Skia raster determinism, first check for a populated `SKIA_DIR` or reuse the
primary checkout's cached `external/skia-build` when it has the required
`*-gpu/lib/Release` libraries. Skia-dependent smoke tests should either run
against that locked cache or skip with a clear "locked raster dependency not
installed" reason; they should not replace the Skia render proof with a fake
or unrelated renderer.

**GPU-host gotcha** (added 2026-05-19 after burning ~3h debugging it):
when `PULP_HAS_SKIA` is FALSE, `WindowHost::create()` on macOS returns the
CPU-only `MacWindowHost` instead of `MacGpuWindowHost`. The CPU host does
NOT override `set_design_viewport()` / `set_fixed_aspect_ratio()` — both
become base-class no-ops. The design-tool example and any runtime-import
host that depends on viewport-pin + aspect-locked uniform paint scale
becomes silently broken: content renders at native size, dark fill is
visible past the design surface, drag-resize feels weird. Before tuning
*anything* in `set_design_viewport`, `windowWillResize:toSize:`, or
`compute_design_viewport_transform`, verify the binary actually contains
`MacGpuWindowHost`:

```bash
nm build/examples/design-tool/pulp-design-tool 2>/dev/null \
  | grep -q MacGpuWindowHost && echo OK || echo "FAIL: CPU-only build"
strings /tmp/MyApp.app/Contents/MacOS/MyApp-Bin \
  | grep -F "[gpu-host]" && echo "OK" || echo "FAIL: CPU-only build"
```

The `pulp-design-tool` example hard-fails CMake configure without Skia
and refuses to run at startup (`EX_CONFIG`, code 78) as belt-and-
suspenders. See `.agents/skills/import-design/SKILL.md` for the recovery
recipe.

For the visual-harness Docker smoke, prefer
`tools/harness/visual/docker-build.sh` over a raw `docker build`. The wrapper
uses the pinned `linux/amd64` Skia archive and a reusable local buildx cache
under `~/.cache/pulp/visual-harness/buildx`; the Dockerfile also keeps
BuildKit cache mounts for apt packages, the Skia release zip, and pip wheels.
That cache is intentionally machine-local, so repeated runs from new worktrees
or SSH hosts on the same machine should reuse downloaded inputs.

For future visual fixtures that need interaction or screenshots, reuse Pulp's
existing hooks before adding platform-specific automation: drive views with
`View::simulate_click`, `simulate_drag`, and `simulate_hover`; capture headless
view trees with `pulp::view::render_to_png` / `render_to_file`, or live host
surfaces with `WindowHost::capture_png()`. Non-Apple platforms require a
registered screenshot provider via `set_screenshot_provider()`, so cross-
platform tests should install that provider or report a clear unsupported skip.
When macOS rendering is the product risk, run the local arm64-darwin smoke in
addition to the Docker smoke; Docker proves the dependency recipe, not Apple's
screenshot or live-window capture paths.

### Status Tracking

Status is tracked in `planning/STATUS.md` (private submodule). Phase specs live in `planning/` with goals, deliverables, acceptance criteria, test plans, and notes. After writing or updating files in `planning/`, always commit and push to the planning repo.

---

## Planning & Internal Docs

The `planning/` directory is a private git submodule (`pulp-planning`). All internal documents go here — never in the public repo.

### What goes in `planning/`

- **Feature specs** — detailed plans for upcoming work
- **Roadmaps and phase tracking** — STATUS.md, phase plans
- **Design exploration** — architecture proposals, workflow designs

### What goes in `planning/research/`

- **Capability assessments** — evaluating what Pulp needs to improve
- **Technology evaluations** — library reviews, framework studies
- **Proposals** — package manager concepts, integration strategies
- **Reference material** — notes from studying other implementations

### Workflow

When creating or updating planning documents:

1. Write files to `planning/`
2. Commit in the planning submodule: `cd planning && git add . && git commit`
3. Push to the planning repo: `git push origin main`
4. Optionally update the submodule pointer in the parent repo

Never create branches or worktrees on the public repo for planning work. Write directly to the submodule.

### Language guidelines

Planning docs should use neutral, professional language:
- "Capability assessment" not "competitive analysis"
- "Feature evaluation" not "gap analysis against [project]"
- "Reference implementation study" not "reverse engineering"
- Focus on what Pulp needs, not on critiquing other projects

---

## Dependency Management

### CHOC-First Policy

Before implementing common C++ utilities, check if [CHOC](https://github.com/Tracktion/choc) already provides it. CHOC is vendored in `external/choc/`, MIT-licensed, and is Pulp's preferred utility layer.

CHOC already provides:
- **MIDI**: `choc::midi::ShortMessage`, note/CC helpers
- **JSON**: `choc::json`, `choc::value`
- **Audio file I/O**: `choc::audio::AudioFileFormat` (WAV, FLAC, OGG)
- **Lock-free containers**: `choc::fifo::SingleReaderSingleWriterFIFO`
- **String utilities**: `choc::text` (trim, split, contains, replace)
- **JavaScript**: `choc::javascript::QuickJSContext`

When writing new code, prefer a CHOC utility over a hand-rolled equivalent. Only implement custom when CHOC doesn't cover the need or when performance requirements demand a specialized implementation (e.g., SIMD FFT).

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

- **Allowed for repo-distributed code:** MIT, BSD (2/3-clause), Apache 2.0, ISC, zlib, BSL-1.0, public domain
- **Not allowed in repo or shipped artifacts:** GPL, LGPL, AGPL, SSPL, proprietary, any copyleft
- **Review required:** MPL-2.0 (weak copyleft, case-by-case)
- **Optional vendor SDK carve-out:** AAX, ASIO, and similar SDKs may be supported only when they stay off by default, outside the source tree, developer-supplied under the user's own agreement, never committed, never exported by `cmake --install`, and never required by public CI.

Before adding ANY bundled or redistributed dependency: check its license, add it to DEPENDENCIES.md, verify compatibility with MIT release. No exceptions.

### Attribution Ordering

DEPENDENCIES.md and NOTICE.md entries are always sorted **alphabetically by name**. When adding a new dependency, insert it in the correct alphabetical position — don't append to the end. This applies to both manual additions and `pulp add` / `pulp audit` operations.

---

## Testing and Validation

### Philosophy

If it's not tested, it doesn't work. Every subsystem has tests. Every feature has acceptance criteria. Every phase has a validation plan.

### Tests ship with fixes — NON-NEGOTIABLE

**Every correctness fix, stub fill-in, or feature slice lands in the same PR as the test that covers it. This is not optional. "CI built green" is not a test — it means the compiler agreed, not that the behavior works.**

Applies to:

- **Parsers / state machines** — UMP sysex reassembly, Atom sequence walking, VST3 event queues, ARA callback sequences, any binary-format decoder. Deterministic input vector → expected output, asserted with Catch2.
- **Threading / lock-free code** — atomic refcounts, SPSC/triple-buffer flush, audio-thread ↔ UI-thread handoff. N-thread hammer test + assert final state. A race-condition bug the CI matrix will not catch.
- **Stub fill-ins** — the acceptance criterion for "implement stub X" is "round-trip test through X using the real path", not "X now compiles." E.g. wiring CLAP `set_parameter` means a test that calls it and reads `get_parameter` back.
- **CLI behavior changes** — shell out to the built binary, assert exit code + stderr content. Catches silent-failure bugs like #295 where `--sign-key` wrote empty signatures without error.
- **Platform-specific backends** — clipboard, file dialogs, screenshot hosts, mDNS. Even if the platform path only runs on one OS, write the test to run on that OS in CI. Cross-platform parity means the test runs on every supported OS too.

What "CI green is enough" cost us on 2026-04-16: a UMP-cursor-advance P1 bug (`#292`), an atomic-refcount P1 bug (`#281`), and a silent-empty-Ed25519-signature P0 bug (`#295`) — all caught by Codex review comments *after* merge because the PRs had no dedicated unit test. A 20-line Catch2 fixture would have caught each of them during PR development.

**The only acceptable "no test in this PR" justification** is: pre-existing historical coverage gap in a subsystem you are not modifying. In that case, file a follow-up issue linked to `#290` and reference it in the commit trailer. Anything else — ship the test.

**Test hygiene:**

- Catch2 tag names cannot contain `#` (reserved). Use `[issue-NNN]` not `[#NNN]`.
- Place tests next to existing `test/test_<subsystem>.cpp` files where one exists; only create a new file for a genuinely new surface.
- Tests that need a runtime binary (CLI, validator) can shell out — don't skip them because the infrastructure is "too heavy."
- Parser tests should exercise the edge cases the fix actually addresses (e.g., the UMP regression test must include "second word's top nibble is 0x3" specifically, not just happy-path single-packet sysex).

**Subsystems under active coverage hardening** (tracked by `#290`): `platform`, `host`, `format`, `runtime`, `view`, `osc`, `dsl`. When touching any of these, grow the test surface — do not leave the coverage number where you found it.

### Test Layers

| Layer | What | Tool | When |
|-------|------|------|------|
| Unit tests | Individual functions, classes | Catch2 | Every commit |
| Integration tests | Subsystem interactions | Catch2 + custom harnesses | Every PR |
| Format validation | Plugin loads correctly | CLAP dlopen, VST3 load tests; auval/pluginval/clap-validator for local use | Every PR touching format code |
| Audio golden-files | DSP output matches reference | Custom harness (bit-exact or tolerance) | Every PR touching signal code |
| Visual regression | UI renders correctly | Screenshot comparison | Every PR touching view/render code |
| UI interaction | Widgets respond correctly | Headless simulate_click/type + assertions | Every PR touching view/widget code |
| Build matrix | Builds on all platforms | GitHub Actions CI | Every PR |
| DAW compatibility | Plugin works in real DAWs | Manual + automated (future) | Before releases |

### Automated Testing Process

**When modifying view/widget/input code, you MUST:**

1. Write or update automated headless tests that verify the behavior
2. Run `ctest --test-dir build --output-on-failure` and verify all tests pass
3. Only show the user visual results AFTER automated tests confirm correctness

**Headless UI testing pattern:**
```cpp
// Create widget, simulate input, verify state — no window needed
TextEditor editor;
editor.on_focus_changed(true);
TextInputEvent te; te.text = "hello";
editor.on_text_input(te);
REQUIRE(editor.text() == "hello");
```

Use `simulate_click()`, `simulate_drag()`, and direct `on_text_input()`/`on_key_event()` calls to test widget interaction without a window. Use screenshot rendering (`render_to_file()`) to verify visual output in CI.

### Docs Maintenance Rule

When you modify files in `core/`, `examples/`, or `tools/cli/`:

1. Check if the change affects public behavior, supported formats, module dependencies, or CLI commands.
2. If yes, update the relevant YAML manifests in `docs/status/`:
   - `support-matrix.yaml` — format/platform support levels
   - `modules.yaml` — module status, dependencies
   - `cli-commands.yaml` — CLI command descriptions
   - `cmake-functions.yaml` — CMake function signatures
3. If yes, update the relevant Markdown docs in `docs/`:
   - `reference/modules.md` — module descriptions
   - `reference/cli.md` — CLI reference
   - `reference/capabilities.md` — capability listings
   - Example pages in `docs/examples/` if examples changed
4. Run `tools/check-docs.sh` (or `pulp docs check`) to validate consistency.

### Skill Maintenance Rule

Skills in `.agents/skills/` are living documents. When you discover a gotcha, fix a non-obvious bug, or establish a new pattern while working in a skill's domain, update the relevant skill:

1. **Trigger**: You made a change that taught you something non-obvious about a subsystem that has a matching skill (e.g., fixed a platform bug → update the `android` skill's gotchas section).
2. **What to add**: Gotchas, conventions, corrected assumptions, new build/deploy recipes. Not ephemeral task state.
3. **What NOT to add**: Things derivable from reading the code or git log. Skills capture *why* and *watch out for*, not *what exists*.
4. **Mapping**: Match the change path to the skill:
   - `core/render/platform/android/`, `android/`, `tools/cmake/PulpAndroid.cmake` → `android`
   - `tools/cli/`, `core/format/src/standalone.cpp` → `cli-maintenance`
   - `core/format/src/aax*`, AAX SDK → `aax`
   - `ci/`, `tools/local-ci/`, `.github/workflows/` → `ci`
   - `core/view/src/webview*`, `core/view/include/*/webview*` → `webview-ui`
   - Design import paths → `import-design`
5. **No skill exists**: If you've accumulated 3+ gotchas for a domain with no skill, create one.

This rule applies to all agents (Claude Code, Codex) and humans. Skills are checked into the repo alongside the code they document.

The rule is **enforced** by `tools/scripts/skill_sync_check.py` via the path map in `tools/scripts/skill_path_map.json`. A PR that touches a skill's mapped paths without also updating the corresponding `SKILL.md` is rejected by CI (`.github/workflows/version-skill-check.yml`) and by the pre-push hook (`.githooks/pre-push`). The explicit per-commit bypass is:

```
Skill-Update: skip skill=<name> reason="..."
```

See `docs/guides/versioning.md` for the full three-layer enforcement design.

### Versioning & Skill-Sync Policy

Pulp versions three surfaces independently: SDK/CLI (`CMakeLists.txt`), Claude plugin (`.claude-plugin/plugin.json` + `marketplace.json`), and Shipyard-binary pin (`tools/shipyard.toml`, consumed by `tools/install-shipyard.sh` — covered by the Dependency Update Workflow, not this policy).

**Enforcement (three layers, one source of truth):**

1. **Agent hooks (Layer 1)** — `hooks/scripts/cli-plugin-sync.sh` runs `version_bump_check.py` and `skill_sync_check.py` in `--mode=hint` on every PostToolUse so you see drift while iterating. Advisory only.
2. **Pre-push hook (Layer 2)** — `.githooks/pre-push` runs both scripts in `--mode=report`. **Enforcing by default** after the advisory-only mode burned 80+ minutes per multi-touch PR on CI roundtrips. `PULP_DISABLE_PREPUSH_GATES=1` demotes back to advisory; `PULP_SKIP_PREPUSH=1` skips entirely (emergencies only).
3. **CI + Shipyard (Layer 3, authoritative)** — `.github/workflows/version-skill-check.yml` and the `validation.gates` stage in `.shipyard/config.toml` both invoke the same scripts in `--mode=report`. CI is hard-failing (no env-var demotion). No bypass other than the commit trailers below.

**Shipping a PR** — when the user says any of "push a PR", "ship this", "ship it", "we're done", "merge this", or "push it", invoke `shipyard pr` (the existing `ci` skill routes through this). Never run `gh pr create` + `shipyard ship` separately; never run the version-bump or skill-sync scripts by hand. `shipyard pr` orchestrates:

1. `skill_sync_check.py --mode=report` — hard-fails on missing SKILL.md updates.
2. `version_bump_check.py --mode=apply` — rewrites version files and CHANGELOG stub.
3. Branch push, PR creation, Shipyard state recording, and cross-platform validation — one command, merges on green.
4. `.github/workflows/auto-release.yml` — on merge to main, tags the new version(s) and the existing tag-triggered release workflows build + publish binaries.

**Exact fix/feat release marker:** `.github/workflows/version-skill-check.yml`
adds `--require-bump-for-fix-feat` on PRs. If the PR title starts with
`fix:` / `fix(scope):` / `feat:` / `feat(scope):`, the diff range must
contain a bump-marker commit whose subject starts with exactly
`chore: bump versions` (canonical) or `chore(versions): bump` (legacy),
unless the tip commit has a top-level `Version-Bump: skip reason="..."`
trailer. Near-misses such as `chore: bump SDK to vX.Y.Z` do not count.
When manually repairing a release bump, use the exact canonical subject.

**Bumps are diff-driven, not title-driven.** `--require-bump-for-fix-feat`
is an *additional* gate layered on `fix:`/`feat:` titles — it is NOT what
triggers the base requirement. `version_bump_check.py` derives a bump
heuristic from the touched paths, so ANY change to versioned source (e.g.
`core/**`) needs a `chore: bump versions` commit regardless of the PR/commit
title — a `refactor:` or `chore:` subject does not exempt it. Don't predict
from the title; run `tools/scripts/gates.sh origin/main` (the same check)
before pushing. Genuinely exempt changes (docs-only, workflow-only,
test-only) use the `Version-Bump: <surface>=skip reason="..."` trailer.

Direct `gh pr create` is an emergency/manual bypass only. If it is used because the user explicitly asked for it or Shipyard is broken, call out that the PR may not be visible in Shipyard-managed state until it is reconciled or re-shipped through Shipyard.

`pulp pr` defaults to the same Shipyard path. A human can opt out in their
local checkout with `pulp config set pr.workflow github` or `manual`, or
temporarily with `PULP_PR_WORKFLOW` / `--workflow`, but agents should not
select those workflows unless the user explicitly requests that bypass.
`pulp status` reports the effective PR workflow and local tool health.

Shipyard is pinned in `tools/shipyard.toml` and auto-discovers Pulp's `tools/scripts/` layout; `.shipyard/config.toml [validation]` additionally pins the paths explicitly for reproducibility.

**Bypass trailers** (tip commit, never PR body — audit trail lives in git):

| Gate           | Trailer                                                          |
|----------------|------------------------------------------------------------------|
| Version bump   | `Version-Bump: <surface>=<patch\|minor\|major\|skip> reason="..."` |
| Skill update   | `Skill-Update: skip skill=<name> reason="..."`                  |
| Auto-release   | `Release: skip reason="..."`                                     |

**Reference-Lineage trailer** (required on any commit touching
`core/format/host_quirks.cpp` or `core/format/include/pulp/format/host_quirks/`):

| Trailer                                                                                  |
|------------------------------------------------------------------------------------------|
| `Reference-Lineage: cleanroom reproducer=#<issue> docs=<url>`                           |

DAW-quirk fixes must be reached independently from host vendor docs + a
reproducer Pulp issue — never by transcribing the reference framework's
workaround. The trailer is the audit trail that proves the
implementation is clean-room. See
`planning/2026-05-24-daw-host-quirks-inheritance.md` for the
license-hygiene contract and the catalog of accommodations the
HostQuirks struct dispatches.

Codex picks this policy up via the existing `AGENTS.md → CLAUDE.md` pointer; `AGENTS.md` intentionally stays a thin redirect so the two never drift. Full design: [docs/guides/versioning.md](docs/guides/versioning.md).

### Release Watchdog

Three layers of protection against silent release failures, independent
of the versioning gates above. Motivated by the 2026-04-20 incident
where a YAML-indent bug in `auto-release.yml` caused every release
attempt for 24h to fail silently with zero jobs dispatched.

1. **`.github/workflows/workflow-lint.yml`** — PR-gate: `yamllint` +
   `actionlint` + structural `yaml.safe_load` on every PR that touches
   `.github/workflows/**`. Catches YAML syntax, action refs, shell
   escaping. Same tools any contributor/agent runs locally.
2. **`.github/workflows/auto-release-watchdog.yml`** — runtime: fires on
   `auto-release.yml` completion; opens a tracking issue on
   `conclusion=failure` (distinguishes workflow-file-rejected from
   job-level failures). Auto-closes on recovery.
3. **`.github/workflows/release-cadence-check.yml`** — invariant
   (every 30 min): scans `main` for commits that bumped `CMakeLists.txt`
   VERSION in the last 24h; flags any bump without a matching tag
   after the grace window. Cause-agnostic — catches unknown-unknowns.

Full design: [docs/guides/release-watchdog.md](docs/guides/release-watchdog.md).

### Local macOS signing & notarization credentials

On a configured dev machine the Developer ID signing and Apple notarization
credentials live in **`~/.config/pulp/secrets/`** — `notary.env` (App Store
Connect API-key trio `PULP_NOTARY_KEY_PATH` / `_KEY_ID` / `_ISSUER_ID`), the
`AuthKey_*.p8`, `keychain.env` (signing-keychain password + cert hashes +
`PULP_SIGN_P12_PW`), and `pulp-signing.p12` (Developer ID Application +
Installer). `pulp ship notarize` reads `notary.env` automatically; by hand:
`set -a; . ~/.config/pulp/secrets/notary.env; set +a` then
`xcrun notarytool submit <pkg> --key "$PULP_NOTARY_KEY_PATH" --key-id
"$PULP_NOTARY_KEY_ID" --issuer "$PULP_NOTARY_ISSUER_ID" --wait` and
`xcrun stapler staple <pkg>`.

**Check that directory before ever concluding "no signing/notarization
credentials."** These are machine-local (never committed); a fresh checkout
won't have them, but a set-up machine does. The full recipe — keychain
partition-list gotchas, inner-out bundle signing, multi-format PKG install
locations — lives in the [`ship`](.agents/skills/ship/SKILL.md) skill.

### Status Vocabulary

Use only these values for `status:` fields in manifests:
`stable`, `usable`, `experimental`, `partial`, `planned`, `unsupported`

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
- **Headless screenshot verification**: Before launching a UI window for visual inspection, use `render_to_file()` or the `--screenshot` flag on preview apps to capture a headless PNG and verify the rendering is correct. Never show the user an empty or broken window. Example: `./build/examples/ui-preview/pulp-ui-preview --screenshot` renders to `/tmp/pulp-animation-preview.png` without opening a window.
  - **Use the Skia backend for any UI with images** (assets, icons, imported designs). `render_to_png`'s default macOS backend is CoreGraphics, whose canvas does NOT implement `draw_image_from_file` — so `ImageView` renders each image's *filename as placeholder text* (empty boxes + scattered `*.png`), which looks like a broken import but is just the backend. The Skia backend (`ScreenshotBackend::skia`) composites file images correctly. `pulp import-design --validate` defaults to `--screenshot-backend skia` for this reason; only pass `coregraphics` deliberately. Showing a non-faithful CoreGraphics render of an asset-rich design wastes a review cycle — re-render with Skia first. See the `screenshot` skill.

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

### Real-DAW Functional Smoke (REAPER) — opt-in, scoped

`auval` / `pluginval` / `clap-validator` prove a plugin **scans + loads**. They do
NOT prove a **functional** behavior happens in a host. Some bugs only appear in a
real DAW because the format adapter injects state no headless / standalone /
render-to-png path has — e.g. a synthesized Bypass param that made the reload
param-contract gate reject every in-DAW reload (2026-07-04; fixed `4a6e048f4`, caught
only by driving the plugin in REAPER).

So for **reload / editor / format-adapter** changes, run the real-DAW smoke
(`tools/testing/daw-smoke/reaper_smoke.py`) when you want functional proof beyond the
validators — **especially before asking a human to test**. It is **opt-in (default
OFF)**, enabled per-machine in `~/.config/pulp/daw-smoke.toml`, and runs only when the
diff touches the reload/editor/adapter allowlist AND the moment warrants it (initial
development of a behavior, or a risky change) — **NOT every build**. It is a ship gate
**only at certain times** (`enabled` + `gate` + path-hit); it degrades to a loud SKIP
when REAPER is absent (unless `strict`), and a SKIP is never a PASS. Bypass a single
commit with a `DAW-Smoke: skip reason="..."` trailer. It is zero-pollution (temp scan
path, always cleans up) and headless-safe (log-scrape, never `screencapture`). Full
rules + how-to: [docs/guides/daw-smoke.md](docs/guides/daw-smoke.md).

### Test in Every Worktree

Tests must pass in the worktree before creating a PR. CI runs the full matrix on PR. No merging with red tests.

For risky build-system or packaging work, "tests pass" also includes a clean detached validation pass with `./validate-build.sh`, not just an incremental rebuild in the existing `build/` directory.

When in doubt, validate in the branch/worktree first and merge to `main` second. The burden of proof should be on landing safely, not on fixing `main` after the fact.

### Local Diff-Coverage Check

Before pushing a PR, run a local diff-coverage check to catch the same
`Diff coverage required (75%)` gate that CI runs. Saves a 20-min CI
roundtrip on coverage-only failures.

```bash
# Whole-tree (slow, matches CI)
tools/scripts/local_diff_cover.sh

# Targeted build (fast — builds only named test targets)
tools/scripts/local_diff_cover.sh pulp-test-widget-bridge

# Targeted build + focused CTest pass
PULP_DIFF_COVER_CTEST_REGEX='WidgetBridge' \
  tools/scripts/local_diff_cover.sh pulp-test-widget-bridge

# Skip (e.g. workflow-only or doc-only PRs)
PULP_SKIP_DIFF_COVER=1 tools/scripts/local_diff_cover.sh

# Same flow via the CLI
pulp coverage diff
pulp coverage diff pulp-test-widget-bridge
```

The threshold + surface filters live in
`tools/scripts/coverage_config.json` — edit there once and CI
(`.github/workflows/coverage.yml`) plus this local script stay in
sync. The pre-push hook runs this check enforcing-by-default;
`PULP_DISABLE_PREPUSH_DIFF_COVER=1` demotes it to
advisory if you genuinely need to push a known coverage gap.

**Two different knobs — pick the right one.** `PULP_DISABLE_PREPUSH_DIFF_COVER=1`
still runs the full configure + build, then only *demotes the failure* to
advisory — so it does NOT make the push faster (a push that looks "hung" right
after this is the diff-cover build compiling, not the network). To skip the
build entirely, use `PULP_SKIP_DIFF_COVER=1`, which exits before any
configure/build while skill-sync / version-bump / compat gates still run. Reach
for `PULP_SKIP_PREPUSH=1` only when those other gates must be skipped too.

The Claude Code slash command `/coverage-diff` invokes the same
script with the same args, so all four invocation surfaces share
one implementation.

**Reclaiming coverage build dirs.** `local_diff_cover.sh` (and shipyard's
local validation, which runs it) creates a per-worktree `build-cov/` and never
cleans it. Across many worktrees these accumulate into hundreds of GB and fill
the disk — which then fails the next coverage build with "No space left on
device" (looks like a code failure; it is not). Reclaim them with:

```bash
tools/scripts/clean_build_cov.sh          # dry-run: list + total reclaimable
tools/scripts/clean_build_cov.sh --yes    # delete (idle-gated; skips an in-flight build)
```

It only ever removes dirs literally named `build-cov` / `build-coverage` (never
a source tree or the primary `build/`), scans sibling worktrees by default
(override with `PULP_WORKTREES_ROOT`), and skips any coverage dir a live build
process is using. Tested by `tools/scripts/test_clean_build_cov.py`.

### Pre-Push Gates Check

`tools/scripts/gates.sh` is the on-demand runner for the cheap
sub-second gates the pre-push hook also runs (skill-sync,
version-bump, compat-sync, deps-audit). It does NOT run the slow
diff-cover lane. Use it before `git push` when you want a fast safety
net independent of the git hook. Named to align with Shipyard's
planned `shipyard gates` subcommand
(planning/2026-05-19-shipyard-preflight-upstream-proposal.md).

```bash
tools/scripts/gates.sh                 # uses origin/main as base
tools/scripts/gates.sh main            # custom base
```

**Bypass priority — reach for the surgical knob first.** The 2026-05-18
pre-push lesson: `PULP_SKIP_PREPUSH=1` on a brand-new commit (not a
rebase) skipped skill-sync, the missed SKILL.md update caught the PR
in CI ~20 minutes later, and burned a roundtrip. Pick the bypass that
maps to the one gate you genuinely need to skip, not the nuclear
"skip everything" knob:

| When you need to                          | Use (surgical)                                  | Avoid (nuclear)                  |
|-------------------------------------------|-------------------------------------------------|----------------------------------|
| Skip the diff-cover BUILD (push fast)     | `PULP_SKIP_DIFF_COVER=1 git push`               | `PULP_SKIP_PREPUSH=1 git push`   |
| Run diff-cover but ignore its result      | `PULP_DISABLE_PREPUSH_DIFF_COVER=1 git push` (still builds) | `PULP_SKIP_PREPUSH=1 git push`   |
| Demote all gates to advisory              | `PULP_DISABLE_PREPUSH_GATES=1 git push`         | `PULP_SKIP_PREPUSH=1 git push`   |
| Skip skill-sync for a single commit       | `Skill-Update: skip skill=<name> reason="…"` trailer | `PULP_SKIP_PREPUSH=1 git push`   |
| Skip version-bump for a single commit     | `Version-Bump: skip reason="…"` trailer         | `PULP_SKIP_PREPUSH=1 git push`   |
| Force-push after rebase (gates ran cleanly on pre-rebase content) | `PULP_SKIP_PREPUSH=1 git push --force-with-lease` (legitimate nuclear use) | — |

If `gates.sh` fails, fix the listed gate and re-run — don't reach
for `PULP_SKIP_PREPUSH=1` unless you're in the rebase case above.

---

## RepoPrompt Usage

Use RepoPrompt to build context efficiently across Pulp's codebase. Key workflows:

- **Context building:** Use `context_builder` to understand subsystem interactions before making changes
- **Code review:** Use `context_builder` with `response_type="review"` after changes for thorough review
- **Cross-reference:** Use RepoPrompt to reference our own code, external SDK code, and safe-to-study projects

RepoPrompt is available globally as an MCP server.

### CRITICAL: RepoPrompt explores the local worktree, not origin/main

RepoPrompt's `context_builder`, `file_search`, and `get_code_structure` read files from whatever branch/commit the local worktree has checked out. **If the worktree is on a feature branch or behind main, RepoPrompt will not see code that has been merged to main via PR.**

**Before any audit, assessment, or "what exists in the codebase" query:**

```bash
# Always verify what's actually on main first
git fetch origin main
git log --oneline origin/main | head -10

# For thorough exploration, create a fresh worktree from origin/main
git worktree add /tmp/pulp-audit origin/main
# Then point all searches at /tmp/pulp-audit, NOT the main repo dir
```

**Never trust "file not found" from RepoPrompt without checking which branch you're on.** Multiple worktrees (feature branches, depth-pass branches, etc.) can cause RepoPrompt to explore the wrong code.

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

### Local-dev audio etiquette

Several Pulp dev workflows can produce host audio out of the user's
speakers without warning: `xcrun simctl launch` of an iOS Sim app that
opens a virtual coreaudio device, `auval` validating an AU plug-in,
loading any AUv3/VST3/CLAP into a host (Logic, GarageBand, REAPER,
AUM, Cubasis) for scan or validation, `pulp run` of a standalone target
(including visually headless live-probe commands such as
`--audio-probe-json`), or any test that exercises the audio render path
with non-zero gain on an unmuted bus. If the user
is listening to music, on a call, near sleeping people, or just away
from the desk, mystery audio is at minimum confusing and at worst
embarrassing or harmful.

Until issue [#3173](https://github.com/danielraffel/pulp/issues/3173)
ships a real solution (announce-before / bounded-duration / muting /
configurable signature / `audio_consent` knob), the interim contract
for every agent (Claude Code, Codex, human-driven scripts) is:

1. **Announce before launching.** In the user-facing message that
   dispatches the work, include a one-liner naming the source and an
   expected duration: "heads up — about to launch the iOS Sim with
   the demo plugin; audio may be active for ~30s while the cube
   renders." Same for `pulp run`, `--audio-inspector`,
   `--audio-probe-json`, `auval`, host launches, etc. Prefer
   `HeadlessHost`, the audio observability harness, or Audio Doctor for
   no-speaker checks whenever the live device is not the thing being tested.
2. **Cap the duration.** Pre-PR / pre-merge verification steps that
   open an audio device should have a hard wall-clock cap. Don't let
   a verify wait hang an open coreaudio session for hours; terminate
   the launch after the relevant marker fires (or after ~90s if it
   doesn't).
3. **Tear down promptly.** When verification is done, terminate the
   launched app and shut down the Sim (`xcrun simctl terminate ...`
   + `xcrun simctl shutdown ...`) rather than leaving it open. Same
   for `auval` host processes, REAPER / Logic test instances.

When [#3173](https://github.com/danielraffel/pulp/issues/3173) lands a
real solution, **remove this section** and replace it with a pointer
to the new mechanism. This contract exists only because the proper
fix is still on the issue tracker.

---

## Shared Agent Skills

Skills live in `.agents/skills/` and are read by both Claude Code and Codex CLI. This is the single source of truth — do not duplicate skills into `.claude/skills/` or `.codex/skills/`.

### Skill Versioning

Skills are committed with the code. A skill must never reference CLI commands, scripts, or features that don't exist at the same commit. If a skill depends on a script, list it in the `requires` frontmatter and check for its existence before running.

### Self-Enhancement

When you create a new repeatable workflow, CLI command, or multi-step pattern that would benefit other sessions or agents, check if it should become a skill:

1. **Is it repeatable?** If you'd do the same steps again next time, it's a skill.
2. **Is it project-specific?** Put it in `.agents/skills/`. Global personal skills go in `~/.codex/skills/` or `~/.claude/skills/` instead.
3. **Create or update** the SKILL.md with name, description, and clear step-by-step instructions.
4. **Don't over-skill** — simple one-off tasks don't need a skill. The bar is "would this save time on the third occurrence?"

When updating existing skills, preserve backward compatibility — don't remove commands that older checkouts might still need. Add new capabilities alongside existing ones.

### Current Skills

Alphabetical. One line of purpose per skill. Each directory at `.agents/skills/<name>/SKILL.md` carries the authoritative, full description. `tools/scripts/skill_path_map.json` owns the source-path → skill mapping used by `skill_sync_check.py` to enforce SKILL.md updates on mapped edits.

| Skill | Purpose |
|-------|---------|
| `aax` | Optional AAX format: developer-supplied Avid SDK, CMake enablement, DigiShell/AAX Validator workflows |
| `android` | Android NDK builds, Oboe audio, Dawn/Skia GPU, JNI bridge, emulator smoke, platform gotchas |
| `ara` | Optional ARA support: developer-supplied SDK, companion APIs, adapter wiring, validation |
| `audio-harness` | Prove/debug what a Processor emits: signal generators, metrics, assertions, RenderScenario, contracts + offline Audio Doctor (response, THD) |
| `audio-headless-debug` | Headless Processor scenes and standalone AU probes for DAW-only audio bugs |
| `auv2` | AU v2 adapter: aufx/aumf/aumi/aumu component types, MIDI input wiring, DAW cache gotchas |
| `auv3` | AU v3 adapter: AUAudioUnit render block, parameter tree, UMP / sysex, sidechain, iOS extension |
| `ci` | Local + cloud CI: validate branches, `shipyard pr` ship flow, merge on green, PR triage |
| `clap` | CLAP adapter: param / mod / sidechain routing, MIDI 1.0 + UMP + sysex + note-expression, ARA hook |
| `cli-maintenance` | CLI command add/modify/remove checklist — keeps source, slash commands, docs, skills in sync |
| `cmajor-external` | MIT-safe Cmajor lane: source-owned patches, external `cmaj` toolchain, generated-artifact flow |
| `content` | Validate, preview, install, update, list, rescan, remove, and reveal data-only content packs |
| `daw-smoke` | Real-DAW (REAPER) functional smoke for reload/editor/format-adapter changes — opt-in, scoped, headless-safe, zero-pollution |
| `engine` | JS engine backend selection (QuickJS / JavaScriptCore / V8) with recommendations per workload |
| `faust` | FAUST DSP plugins: offline codegen, pre-generated C++ headers, FaustProcessor wrapper |
| `hosting` | Load + run + test VST3 / AU / CLAP / LV2 plugins from Pulp (scanner, plugin_slot, signal_graph) |
| `import-design` | Import designs from Figma / Stitch / v0 / Pencil into Pulp web-compat JS with visual validation |
| `ios` | iOS platform: AUv3 app extensions, Simulator builds, UIKit host, CoreAudio, touch + Pencil input |
| `jsfx-subset` | Bounded JSFX subset — source-only examples, explicit exclusions (no `@gfx`), subset validation |
| `kits` | Search, inspect, plan, apply, remove, pack, and scaffold local Pulp kit manifests |
| `motion` | Trace and validate animations, transitions, scroll geometry, reduced motion, and motion fixtures |
| `mpe` | Build MPE-aware synths: descriptor opt-in, `MpeBuffer` consumption, `MpeVoiceAllocator` routing |
| `packages` | Third-party audio package search, suggest, add, browse |
| `pr-review-sweep` | Sweep a PR's automated + human review comments and act on them — especially material/large PRs; pre-/post-merge, cross-repo (Pulp + Shipyard) |
| `prototype-loop` | Leveraged-prototype dev loop (`pulp loop`): focus marker + normal watch/rebuild, AOT analyzer guidance, deferred ar-swap / PR monitor |
| `screenshot` | Faithful headless PNG capture: render_to_png Skia-vs-CoreGraphics backends, image-compositing trap, `--screenshot-backend`, capture_png |
| `sdf-text` | SDF / MSDF / PSDF glyph atlases: building, sampling via SkSL, shared text-layout helpers |
| `ship` | Sign / notarize / package / distribute Pulp plugins and apps across macOS / Windows / Android |
| `skia-gpu-build` | Enable Skia+Dawn GPU builds: prebuilt skia-builder libs, headers-only worktree trap, `SKIA_DIR` reuse, `MacGpuWindowHost` verify, raster-fallback + GPU-wedge gotchas |
| `streams` | `pulp::runtime::AsyncStream` selection, async-callback wiring without deadlock, backpressure |
| `stretch` | Offline time-stretch / pitch / varispeed: character modes, fine-tune presets, A/B eval toolkit, honest quality state |
| `tart-ci` | Tart golden-VM macOS CI: layered goldens, ephemeral per-job runners, vm-image manifest, caching/rebake, host-keychain safety |
| `threejs-bridge` | Native Dawn-backed Three.js: three.webgpu.js renderer, bridge tests, native demo capture |
| `upgrade` | `pulp upgrade` guidance: release discovery, migration notes, breaking-change fixes |
| `video-proof` | Desktop validation videos: record raw proof, render Remotion context, publish/serve report, prepare review issue body |
| `view-bridge` | Editor lifecycle and multi-view attach — `Processor::create_view()`, open/notify/resize/close protocol |
| `vst3` | VST3 adapter: SingleComponentEffect, bus arrangement, param/MIDI routing, state, Steinberg SDK traps |
| `webview-ui` | WebView UI: native bridge, embedded assets, directory-backed dev resources, WebView validation |

When adding a new skill, append its row here and register the subsystem in `tools/scripts/skill_path_map.json`.

### Claude Code Plugin

Pulp ships as a Claude Code plugin with slash commands (`/build`, `/test`, `/create`, `/status`, `/validate`, `/design`, `/ship`, `/import-design`, `/audio-harness`), skills, and hooks. The plugin manifest is at `.claude-plugin/plugin.json`. See `docs/guides/claude-code-plugin.md` for installation and full details.

### CI Workflow

Use Shipyard for all merges. Current Pulp branch protection requires the
macOS lane; Linux and Windows still run in GitHub Actions but are advisory
unless branch protection changes. When the macOS lane wedges or
Shipyard itself drifts, see the `ci` skill's *Self-hosted runner ops*
section for the v0.56.2+ toolkit (`shipyard rescue` / `runner watch
--kill-hung-workers` / `update`).

#### The `ship` workflow (primary path for all agents)

```bash
# Primary path for "ship this" / "push a PR" / any natural-language ship trigger:
shipyard pr

# Useful options (v0.20.0):
shipyard pr --base main                         # base ref must NOT include `origin/` — Shipyard prepends it internally (Shipyard #301)
shipyard pr --no-apply-bumps                    # hard-fail on missing version bumps
shipyard pr --skip-bump plugin --bump-reason="test-only change"
shipyard pr --skip-skill-update ci --skill-reason="docs-only"
shipyard pr --skip-target ubuntu                # deliberate lane skip

# Docs-only PR (no source under core/**, examples/**, ship/**, tools/cli/**, no
# CMakeLists.txt change). Skip both SSH targets and the pre-push diff-cover
# gate — both trigger a full CMake configure + build that fails in fresh
# worktrees lacking Skia and FetchContent dependencies. Upstream fixes are
# tracked at pulp #2021 (auto-detect docs-only and skip diff-cover) and
# Shipyard #301 (--base double-prefix + auto-skip unreachable SSH targets).
PULP_SKIP_DIFF_COVER=1 shipyard pr --base main \
  --skip-target ubuntu --skip-target windows

# `pulp pr` is a Pulp-side wrapper; it defaults to shipyard pr and reports
# opt-out workflows via `pulp status`. Agents should prefer `shipyard pr`.
pulp pr

# Lower-level Shipyard commands — use only for diagnostics or recovery, NOT as the
# default ship path:
shipyard run                              # validate current branch
shipyard ship                             # resume/operate on an existing Shipyard-managed PR

# SSH preflight (v0.20.0+ / Shipyard #106): exit codes are distinct.
#   0 — success
#   1 — validation failed
#   2 — configuration error
#   3 — backend unreachable (new; surfaces within 10s with classified reason)
# Use --skip-target NAME for DELIBERATE lane skips (no probe).
# Use --allow-unreachable-targets only when you genuinely want to proceed despite
# an unreachable backend — it now prints a loud "⚠︎ VALIDATION GAP" banner naming
# the skipped lane.
```

The CI skill (`.agents/skills/ci/SKILL.md`) is the single source of truth for landing code. Normal ship cycle:

1. Run `shipyard pr` — never `gh pr create` + `shipyard ship` separately (that bypasses the skill-sync and version-bump gates)
2. The orchestrator runs skill-sync + version-bump gates, commits any bumps, pushes, opens/tracks the PR, and invokes Shipyard validation
3. Shipyard validates the macOS lane through the local self-hosted runner path
4. GitHub Actions runs Linux and Windows on GitHub-hosted runners as advisory checks
5. Posts a closeout comment

`local_ci.py` remains in the repo as a legacy fallback but is scheduled for removal.

#### GitHub Actions

PRs trigger `.github/workflows/build.yml` on all three platforms. macOS routes
to the local self-hosted runner through `PULP_LOCAL_MACOS_RUNS_ON_JSON` when
that repo variable is set. Linux and Windows use GitHub-hosted runners and are
advisory unless explicitly required by branch protection.

**A slow / stuck PR is worth investigating before assuming runner saturation.**
Saturation is possible (a real burst, or a wedged runner), but the required
`macos` gate runs on the local Mac Studios (usually idle), so confirm it rather
than assume it. Before concluding capacity:
(1) check the required checks even registered — a Shipyard-App-opened PR does NOT
auto-trigger `pull_request` workflows, so `ghapp workflow run build.yml --ref
<branch>` + `… version-skill-check.yml --ref <branch>` are often needed;
(2) check for a version-bump race (PR goes `DIRTY` on the `CMakeLists.txt`
VERSION line — re-merge `main`); (3) only then verify capacity with
`ghapp api repos/danielraffel/pulp/actions/runners` (`busy` per runner) —
GitHub-*hosted* advisory lanes queue independently and don't block merge. Full
diagnosis + the non-Shipyard fallback: the `ci` skill, "Diagnosing a slow /
stuck PR."

#### Runner priority (hard rule)

**No Namespace.** Namespace cloud macOS runners are a *paid* overflow we do NOT
use (cost). The capability stays wired in Shipyard / `build.yml` as an explicit,
operator-dispatched break-glass option, but it is OFF by default and must never
be auto-routed. Keep `PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON` (and the
Linux/Windows equivalents) **UNSET**, and **never hijack that variable to point
at self-hosted runners** — doing so dumps the high-volume sanitizer/coverage
lanes onto the Mac Studio runners that host the required `macos` gate and breaks
it across PRs (the 2026-06-07 lesson). This matches `.shipyard/config.toml`
("Namespace macOS routing is disabled for cost control").

macOS runs on **local Macs + GitHub-hosted**, in this order:

1. **Mac Studio** (`pulp-studio-01/02/03`, `PULP_LOCAL_MACOS_RUNS_ON_JSON`) — the
   primary required `macos` gate.
2. **M5 Mac** (`pulp-build-m5`, `PULP_OVERFLOW_BUILD_MACOS_RUNS_ON_JSON`) — local
   overflow when the Studio runners are saturated
   (>= `PULP_LOCAL_MAC_OVERFLOW_THRESHOLD` busy).
3. **GitHub-hosted `macos-15`** — sanitizers, coverage, release-cli, and the
   build-overflow fallback (each via its own `PULP_SANITIZER_*` /
   `PULP_COVERAGE_MACOS` var, all `macos-15`). Clean per run, so the ODR-prone
   lanes stay OFF the warm self-hosted build dirs.
4. **Namespace macOS cloud** — break-glass ONLY, operator-dispatched, never
   automatic.

Linux and Windows use GitHub-hosted runners (advisory). SSH Ubuntu/Windows only
when a human explicitly asks. If Shipyard probes SSH Ubuntu/Windows for a
macOS-focused PR where they're out of scope, use
`--skip-target ubuntu --skip-target windows`.

**Future (deliberate, not automatic):** using the local Macs (Studio/M5) for the
heavier lanes (release-cli, sanitizers) means wiring each lane its OWN dedicated
`runs-on` var pointing at the local labels — **not** the Namespace var — paired
with auto-cleaning the warm `build-<key>` dirs on churn (see the
`pulp-runner-ops` skill, since warm-dir ODR is why those lanes stay on
GitHub-hosted today). Until that lands, they stay on `macos-15`.

See `docs/guides/local-ci.md` for setup.
