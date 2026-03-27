# Docs Industry Parity Plan

Date: 2026-03-25
Purpose: Identify documentation gaps between Pulp and industry-standard audio plugin frameworks, and plan phased work to close them.

## Methodology

Compared Pulp's current documentation against a mature industry-standard framework's documentation structure (24 modules, 85+ examples, Doxygen-generated API reference, 7 markdown guides, online tutorial portal, community forum).

## Current Pulp Documentation Strengths

These areas are at or near industry parity:

- **Architecture documentation** — thorough, with dependency diagrams and thread model
- **Getting started** — practical walkthrough with code examples for all formats
- **Examples** — 7 examples with per-example doc pages, gallery index, classification
- **Policies/contribution** — well-defined code style, agent rules, clean-room discipline
- **Build system basics** — CMake guide, pulp_add_plugin reference
- **Machine-readable manifests** — unique strength (industry frameworks don't have this)
- **Local-first docs CLI** — unique strength
- **Docs maintenance automation** — unique strength (CI + hooks + AGENTS.md)

## Gaps Ranked by Impact

### Tier 1: Critical gaps (blocks adoption)

| Gap | Industry benchmark | Pulp today | Impact |
|-----|-------------------|------------|--------|
| **API reference** | Doxygen-generated class/function docs from source comments | No generated API docs; modules.md has one-paragraph summaries | Developers can't look up function signatures, parameters, return types |
| **Format-specific guides** | Detailed per-format docs (VST3 params, AU sandbox, CLAP modulation) | Entry point macros documented; no format internals | Developers can't understand format-specific behavior |
| **Widget/UI system guide** | Comprehensive GUI component reference with examples | Stub coverage; VISION describes aspirations, no practical guide | Can't use the experimental view system without reading source |
| **DSP algorithm usage** | Per-algorithm documentation with parameter ranges and examples | Inventory list only; no usage examples | Developers must read headers to use signal processors |

### Tier 2: Important gaps (reduces credibility)

| Gap | Industry benchmark | Pulp today | Impact |
|-----|-------------------|------------|--------|
| **Module deep-dive guides** | Per-module usage guides explaining patterns and concepts | One paragraph per module in modules.md | Must read source to understand module usage patterns |
| **State/parameter system guide** | Detailed parameter API, serialization format, groups, display formatting | Moderate coverage but missing ParamValue API details | Parameter system is stable but under-documented |
| **Platform-specific guides** | Per-platform build and deployment guides (macOS, Windows, Linux, iOS, Android) | macOS only; Windows/Linux noted as "stubs exist" | Can't onboard non-macOS developers |
| **Testing deep-dive** | HeadlessHost API, golden-file workflows, sanitizer setup, benchmarking | Moderate; missing HeadlessHost API docs and sanitizer guide | Testing infrastructure exists but isn't teachable |
| **Deployment/shipping guide** | Signing, notarization, auto-updates, distribution workflows | Basic CLI reference for sign/package/check | Shipping pipeline exists but isn't documented end-to-end |

### Tier 3: Nice-to-have gaps (polish)

| Gap | Industry benchmark | Pulp today | Impact |
|-----|-------------------|------------|--------|
| **Lock-free primitive guide** | Usage patterns for thread-safe communication | Mentioned in architecture doc; no usage guide | Advanced topic but important for correctness |
| **MIDI deep-dive** | Event patterns, polyphonic expression, Note Expression, MIDI 2.0 | Basic coverage of message types and device I/O | Most users can work without this initially |
| **Migration/upgrade guide** | Version migration docs | None (only one version exists) | Not needed yet; needed at v2 |
| **Screenshots/visuals** | Screenshots in example docs, architecture diagrams | No screenshots; ASCII diagrams only | Reduces visual appeal of docs site |
| **Video tutorials** | Online tutorial portal | None | High effort, low priority for now |
| **Versioning/release process** | Explicit semver, release checklist | Not explicitly documented | Needed before public release |

## Phased Implementation Plan

### Phase A: API Reference Foundation
**Priority: Critical**
**Effort: Medium**
**Integrates with: existing docs system**

1. Add Doxygen configuration (`docs/doxygen/Doxyfile`) targeting public headers only
2. Add documentation comments to key public headers:
   - `core/format/include/pulp/format/processor.hpp` (Processor interface)
   - `core/state/include/pulp/state/*.hpp` (ParamValue, StateStore, Binding)
   - `core/audio/include/pulp/audio/*.hpp` (BufferView)
   - `core/midi/include/pulp/midi/*.hpp` (MidiEvent, MidiBuffer)
3. Add CI step to generate and deploy API docs alongside the site
4. Add `pulp docs api` CLI command to open generated API reference
5. Link from docs site navigation to API reference

**Does NOT require**: documenting every internal class. Focus on consumer-facing headers only.

### Phase B: Format and DSP Guides
**Priority: Critical**
**Effort: Medium**

1. Create `docs/guides/formats.md` — format-specific behavior guide:
   - VST3: parameter events, output changes, groups, transport
   - AU v2: component registration, auval requirements, process context
   - CLAP: parameter modulation, note expressions, MIDI output events
   - How format adapters translate Processor calls
2. Create `docs/guides/signal-processing.md` — DSP algorithm usage guide:
   - How to use each processor (constructor, process, parameter ranges)
   - Common patterns (filter chains, parallel effects, sidechain)
   - Real-time safety rules for DSP code
3. Create `docs/guides/parameters.md` — state system deep-dive:
   - ParamValue API (get, set, ranges, normalization)
   - StateStore registration patterns
   - Binding and gesture tracking
   - Serialization format and versioning
   - CLAP modulation offsets

### Phase C: Module Deep-Dives
**Priority: Important**
**Effort: Medium-High**

1. Create per-module guide pages in `docs/guides/modules/`:
   - `runtime.md` — lock-free primitives (SeqLock, TripleBuffer, SPSCQueue usage patterns)
   - `canvas.md` — 2D drawing API, backends (CoreGraphics, Skia), SVG, effects
   - `view.md` — widget system, flex layout, themes, JS scripting, hot-reload
   - `render.md` — Dawn/Skia Graphite setup, GPU surface management
   - `osc.md` — OSC sender/receiver patterns
2. Update `docs/status/docs-index.yaml` with new entries
3. Link from modules.md to individual guide pages

### Phase D: Platform and Deployment Guides
**Priority: Important**
**Effort: Medium**

1. Create `docs/guides/platforms/`:
   - `macos.md` — entitlements, signing, notarization, hardened runtime, auval
   - `windows.md` — build setup, SDK requirements, signing (when implemented)
   - `linux.md` — package dependencies, ALSA/JACK setup (when implemented)
2. Create `docs/guides/shipping.md` — end-to-end deployment:
   - Code signing workflow
   - Notarization with notarytool
   - PKG/DMG creation
   - Appcast generation for auto-updates
   - CI release pipeline
3. Create `docs/guides/testing-advanced.md`:
   - HeadlessHost API
   - Golden-file creation and maintenance
   - Sanitizer builds (ASan, TSan, UBSan)
   - Format validator workflows

### Phase E: Visual and Discovery Polish
**Priority: Nice-to-have**
**Effort: Low-Medium**

1. Add screenshots for examples (UI Preview, any examples with GUI)
2. Add architecture diagrams (module dependency, plugin format pipeline, thread model)
3. Add versioning/release process doc
4. Add search to the docs site (client-side lunr.js or similar)

## How This Integrates With Existing System

All new docs follow the existing model:
- Markdown files in `docs/`
- Entries added to `docs/status/docs-index.yaml`
- `pulp docs check` validates consistency
- CI catches drift on every PR
- Site generator picks up new pages automatically
- AGENTS.md and Claude Code hook remind about updates

The Doxygen API reference (Phase A) is the only piece that requires new tooling. It would:
- Live in `docs/doxygen/` for configuration
- Generate to `build/api-docs/` (gitignored)
- Deploy alongside the site to `generouscorp.com/pulp/api/`
- Be triggered by `pulp docs build-api` or CI

## Recommended Priority

1. **Phase A** (API reference) — highest leverage, unlocks self-service for developers
2. **Phase B** (formats + DSP + params) — fills the most painful knowledge gaps
3. **Phase C** (module deep-dives) — needed for experimental subsystems (view, render, canvas)
4. **Phase D** (platform + deployment) — needed before cross-platform shipping
5. **Phase E** (polish) — before public launch

## What NOT To Do

- Do not document unimplemented features as if they exist (VISION.md lesson)
- Do not build a separate docs authoring system outside the repo
- Do not add a heavyweight docs framework (Hugo, Docusaurus) — the Python builder is sufficient
- Do not write API docs by hand — use Doxygen from source comments
- Do not document internal/private APIs — public headers only
