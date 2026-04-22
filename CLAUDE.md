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
./build/tools/cli/pulp version             # show SDK and project version
./build/tools/cli/pulp version bump patch  # bump version
./build/tools/cli/pulp version check       # verify version consistency
./build/tools/cli/pulp dev --test          # watch + rebuild + test loop
./build/tools/cli/pulp build --watch       # watch + rebuild loop
```

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

---

## Repo Standards

This repo will be open-sourced. Every commit, every file, every directory name should reflect that. No throwaway code on main. No "WIP" commits. No embarrassing history.

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
- Parser tests should exercise the edge cases the fix actually addresses (e.g., the #292 UMP test must include "second word's top nibble is 0x3" specifically, not just happy-path single-packet sysex).

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

Pulp versions three surfaces independently: SDK/CLI (`CMakeLists.txt`), Claude plugin (`.claude-plugin/plugin.json` + `marketplace.json`), and Shipyard-binary pin (`tools/install-shipyard.sh` — covered by the Dependency Update Workflow, not this policy).

**Enforcement (three layers, one source of truth):**

1. **Agent hooks (Layer 1)** — `hooks/scripts/cli-plugin-sync.sh` runs `version_bump_check.py` and `skill_sync_check.py` in `--mode=hint` on every PostToolUse so you see drift while iterating. Advisory only.
2. **Pre-push hook (Layer 2)** — `.githooks/pre-push` runs both scripts in `--mode=report`. Advisory by default; `PULP_ENFORCE_PREPUSH=1` upgrades to hard failure.
3. **CI + Shipyard (Layer 3, authoritative)** — `.github/workflows/version-skill-check.yml` and the `validation.gates` stage in `.shipyard/config.toml` both invoke the same scripts in `--mode=report` with `PULP_ENFORCE_PREPUSH=1`. No bypass other than the commit trailers below.

**Shipping a PR** — when the user says any of "push a PR", "ship this", "ship it", "we're done", "merge this", or "push it", invoke `shipyard pr` (the existing `ci` skill routes through this). Never run `gh pr create` + `shipyard ship` separately; never run the version-bump or skill-sync scripts by hand. `shipyard pr` orchestrates:

1. `skill_sync_check.py --mode=report` — hard-fails on missing SKILL.md updates.
2. `version_bump_check.py --mode=apply` — rewrites version files and CHANGELOG stub.
3. `git commit` + `gh pr create` + `shipyard ship` — one command, merges on green.
4. `.github/workflows/auto-release.yml` — on merge to main, tags the new version(s) and the existing tag-triggered release workflows build + publish binaries.

Shipyard v0.19.1+ (pinned as v0.21.0 in `tools/shipyard.toml`) auto-discovers Pulp's `tools/scripts/` layout; `.shipyard/config.toml [validation]` additionally pins the paths explicitly for reproducibility.

**Bypass trailers** (tip commit, never PR body — audit trail lives in git):

| Gate           | Trailer                                                          |
|----------------|------------------------------------------------------------------|
| Version bump   | `Version-Bump: <surface>=<patch\|minor\|major\|skip> reason="..."` |
| Skill update   | `Skill-Update: skip skill=<name> reason="..."`                  |
| Auto-release   | `Release: skip reason="..."`                                     |

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

For risky build-system or packaging work, "tests pass" also includes a clean detached validation pass with `./validate-build.sh`, not just an incremental rebuild in the existing `build/` directory.

When in doubt, validate in the branch/worktree first and merge to `main` second. The burden of proof should be on landing safely, not on fixing `main` after the fact.

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
| `auv2` | AU v2 adapter: aufx/aumf/aumi/aumu component types, MIDI input wiring, DAW cache gotchas |
| `auv3` | AU v3 adapter: AUAudioUnit render block, parameter tree, UMP / sysex, sidechain, iOS extension |
| `ci` | Local + cloud CI: validate branches, `shipyard pr` ship flow, merge on green, PR triage |
| `clap` | CLAP adapter: param / mod / sidechain routing, MIDI 1.0 + UMP + sysex + note-expression, ARA hook |
| `cli-maintenance` | CLI command add/modify/remove checklist — keeps source, slash commands, docs, skills in sync |
| `cmajor-external` | MIT-safe Cmajor lane: source-owned patches, external `cmaj` toolchain, generated-artifact flow |
| `engine` | JS engine backend selection (QuickJS / JavaScriptCore / V8) with recommendations per workload |
| `faust` | FAUST DSP plugins: offline codegen, pre-generated C++ headers, FaustProcessor wrapper |
| `hosting` | Load + run + test VST3 / AU / CLAP / LV2 plugins from Pulp (scanner, plugin_slot, signal_graph) |
| `import-design` | Import designs from Figma / Stitch / v0 / Pencil into Pulp web-compat JS with visual validation |
| `ios` | iOS platform: AUv3 app extensions, Simulator builds, UIKit host, CoreAudio, touch + Pencil input |
| `jsfx-subset` | Bounded JSFX subset — source-only examples, explicit exclusions (no `@gfx`), subset validation |
| `mpe` | Build MPE-aware synths: descriptor opt-in, `MpeBuffer` consumption, `MpeVoiceAllocator` routing |
| `packages` | Third-party audio package search, suggest, add, browse |
| `sdf-text` | SDF / MSDF / PSDF glyph atlases: building, sampling via SkSL, shared text-layout helpers |
| `ship` | Sign / notarize / package / distribute Pulp plugins and apps across macOS / Windows / Android |
| `streams` | `pulp::runtime::AsyncStream` selection, async-callback wiring without deadlock, backpressure |
| `threejs-bridge` | Native Dawn-backed Three.js: three.webgpu.js renderer, bridge tests, native demo capture |
| `upgrade` | `pulp upgrade` guidance: release discovery, migration notes, breaking-change fixes |
| `view-bridge` | Editor lifecycle and multi-view attach — `Processor::create_view()`, open/notify/resize/close protocol |
| `vst3` | VST3 adapter: SingleComponentEffect, bus arrangement, param/MIDI routing, state, Steinberg SDK traps |
| `webview-ui` | WebView UI: native bridge, embedded assets, directory-backed dev resources, WebView validation |

24 skills as of 2026-04-22. When adding a new skill, append its row here and register the subsystem in `tools/scripts/skill_path_map.json`.

### Claude Code Plugin

Pulp ships as a Claude Code plugin with slash commands (`/build`, `/test`, `/create`, `/status`, `/validate`, `/design`, `/ship`, `/import-design`), skills, and hooks. The plugin manifest is at `.claude-plugin/plugin.json`. See `docs/guides/claude-code-plugin.md` for installation and full details.

### CI Workflow

**Never merge a PR without green CI on all three platforms (macOS, Ubuntu, Windows).** Use the `ci` skill for all merges — it handles the full workflow.

#### The `ship` workflow (primary path for all agents)

```bash
# Primary path for "ship this" / "push a PR" / any natural-language ship trigger:
shipyard pr

# Useful options (v0.20.0):
shipyard pr --base origin/main
shipyard pr --no-apply-bumps                    # hard-fail on missing version bumps
shipyard pr --skip-bump plugin --bump-reason="test-only change"
shipyard pr --skip-skill-update ci --skill-reason="docs-only"
shipyard pr --skip-target ubuntu                # deliberate lane skip

# `pulp pr` is a Pulp-side wrapper; it still works and delegates to shipyard pr.
# Use either — agents should prefer `shipyard pr` for directness.
pulp pr

# Lower-level Shipyard commands — use only for diagnostics or recovery, NOT as the
# default ship path:
shipyard run                              # validate current branch
shipyard ship                             # PR + validate + merge on green
shipyard cloud run build <branch>         # dispatch to Namespace

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
2. The orchestrator runs skill-sync + version-bump gates, commits any bumps, pushes, opens a PR, and invokes `shipyard ship`
3. `shipyard ship` validates on macOS (locally), Ubuntu (SSH), and Windows (SSH)
4. Merges only when ALL targets pass
5. Posts a closeout comment

`local_ci.py` remains in the repo as a legacy fallback but is scheduled for removal (see issue #120).

#### GitHub Actions (backup gate)

PRs also trigger `.github/workflows/build.yml` which builds and tests on all three platforms via Namespace runners. This is a redundant safety net — Shipyard's local validation is the primary path.

#### Runner priority (hard rule)

**Always use Namespace for cloud CI. Never rely on GitHub-hosted runners as the primary path.**

1. **Namespace** (default): `shipyard cloud run build <branch>`
2. **Local VMs** (fallback): `ssh ubuntu`, `ssh win` via `shipyard run`
3. **GitHub-hosted** (last resort): Only if Namespace is unavailable.

macOS runs locally in parallel with Namespace/SSH Ubuntu/Windows builds.

See `docs/guides/local-ci.md` for setup.
