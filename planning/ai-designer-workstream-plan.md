## Pulp AI Designer Workstream Plan

Date: 2026-03-26
Status: Proposed committed workstream
Owner: Planning / architecture

### Decision

Pulp should explicitly build the AI Designer.

This should not remain an implied aspiration spread across `VISION.md`, old loop prompts, and deferred `STATUS.md` items. It should become a named workstream with its own architecture docs, implementation sequence, and validation criteria.

### What This Means

The AI Designer is not just:

- screenshot capture
- asset embedding
- a showcase app
- a prompt that rewrites colors

It is a product surface built on top of a few deeper capabilities:

1. a stable retained UI model
2. a stable design-token system
3. deterministic token propagation and hot reload
4. inspector/introspection hooks
5. a render/view lifecycle that can preview changes reliably
6. a CLI and/or MCP surface that can drive those edits locally

If those foundations are missing or unstable, the AI Designer becomes a demo instead of a trustworthy tool.

### Non-Negotiable Product Requirements

The AI Designer must operate on real Pulp UI code and real shippable artifacts.

That means:

- it cannot be a mockup-only surface
- it cannot only restyle a fake showcase disconnected from product code
- it must be able to target a real plugin or app UI built with Pulp
- it must make material style changes, not just minor color swaps
- it must support realtime preview while editing
- it must let the user lock in a design and ship it

The practical standard is:

- tweak a real plugin UI
- preview the changes live
- inspect what changed
- save the result as durable style data
- rebuild or reload the target
- ship that style with confidence

### Current Repo Reality

The repo already commits to this direction in several places:

- `VISION.md` promises an AI Style Designer, token-driven theming, `pulp inspect`, and `pulp design`
- older Ralph prompts describe a showcase app and AI-driven theming flow
- `planning/STATUS.md` still marks the showcase, AI Style Designer integration, and `pulp design` as deferred

The current audit loop in `planning/ralph-loop-prompt-7.md` does not explicitly build this work. It is an audit/status/gap-closure loop. That is correct. The AI Designer should be introduced as a separate committed workstream that starts from truthful status, not from drifting claims.

### Strategic Position

The AI Designer should be treated as a flagship Pulp differentiator:

- structured design tokens instead of inheritance-heavy theme code
- local-first AI workflows instead of mandatory remote web tooling
- inspectable and scriptable UI state
- deterministic preview/review surfaces for agents
- clean integration with `pulp inspect`, `pulp design`, screenshots, docs, and examples

This is a meaningful reason to use Pulp, not a side feature.

### Canonical Product Shape

The canonical AI Designer should be a native, local-first Pulp tool.

Recommended primary shape:

- `pulp design` CLI entry point
- native preview host built on the same Pulp view/render stack being designed
- native inspector integration
- local token/style-pack files
- optional MCP bridge for agent control

Why this should be canonical:

- it matches Pulp's core thesis: native GPU rendering, not browser-runtime tooling
- it validates the actual framework rather than a parallel shell
- it keeps one runtime, one rendering path, and one debugging model
- it is the easiest path for local agents and subscription-based AI tools to use without mandatory cloud infrastructure

Tauri or browser-first should not be the canonical design-authoring surface.

They may still be useful later for:

- read-only sharing
- hosted previews
- documentation demos
- lightweight remote review via WASM

But the primary authoring path should stay native and local-first.

### Workstream Split

This work should be split into three major streams, with clear dependency order.

#### Stream A: UI / Token / Inspection Architecture

Goal: define the Pulp-native architecture that the AI Designer depends on.

This stream should produce specs for:

- minimal retained node tree
- event propagation model
- animation scheduling model
- frame lifecycle and invalidation rules
- design token model and inheritance rules
- inspection/introspection surface
- token serialization format
- AI edit application model

Inputs:

- current Pulp `view`, `render`, `canvas`, `events`, and inspector-related code
- `planning/08-architecture-spec.md`
- `planning/12-rendering-strategy.md`
- `planning/phases/14-phased-roadmap.md`
- clean-room pattern extraction from `~/Code/visage`
- design-token and editing ideas from `~/Code/ai-style-designer`

Constraints:

- no copied APIs or structure
- no renderer-aware widgets
- no extra renderer abstraction layer
- no dependency on Visage
- compatible with JS/TS UI workflows, CLI, MCP, and headless operation

#### Stream B: Product Foundation Implementation

Goal: implement the minimum real substrate needed for the AI Designer to be credible.

This stream should land or harden:

- retained UI node primitives
- deterministic design token application
- hot reload for token changes
- inspector hooks for token and node introspection
- stable preview path for screenshots and/or live design sessions
- enough view/render integration to support repeatable visual updates

This stream is where real code gets judged. It should not start from speculative docs alone; it should build against accepted Stream A specs.

#### Stream C: AI Designer Surface, Showcase, and Assets

Goal: make the system usable and legible to humans and agents.

This stream should cover:

- widget showcase app
- design-session UX
- `pulp design` CLI
- optional MCP/session hooks for tool-driven editing
- example screenshots and visual docs
- asset and token example packs
- export flows (JSON, CSS, C++, shader uniforms, OKLCH)

This stream depends on Streams A and B. It can prototype earlier, but the real user-facing version should wait until the substrate is stable enough that the showcase is demonstrating reality rather than masking churn.

### Dependency Order

Recommended order:

1. truth-first audit and status reset
2. Stream A architecture/spec work
3. Stream B foundation implementation
4. Stream C showcase, assets, and AI design surface

Allowed overlap:

- clean-room pattern extraction can begin while the status audit is wrapping up
- token-schema exploration can overlap late Stream A
- early showcase exploration can happen in an `explore/` branch only

Not allowed:

- presenting `pulp design` as usable before a stable token + preview + inspection path exists
- letting showcase/demo needs dictate renderer or view architecture

### What To Build First

The first concrete deliverables should be docs and specs, not prompts or branding assets.

Create first:

- `planning/ui-pattern-extraction-from-visage.md`
- `planning/pulp-ui-architecture-spec.md`
- `planning/design-token-and-ai-designer-spec.md`
- `planning/showcase-and-visual-assets-spec.md`

Then create repo docs when the architecture is accepted:

- `docs/architecture/ui-system.md`
- `docs/architecture/render-loop.md`
- `docs/architecture/animation-system.md`
- `docs/architecture/event-system.md`
- `docs/architecture/design-tokens.md`
- `docs/architecture/inspection.md`

These docs should describe actual intended contracts, not optimistic prose detached from the code.

### Core User Workflow

The product has to be legible to a real developer, not just interesting in principle.

The intended first-class workflow should be:

1. open a shipped primitive pack or an example plugin preview
2. launch `pulp design`
3. connect to a local AI tool, including Claude via a local CLI or MCP bridge
4. describe a desired style in natural language
5. preview token changes in real time on the actual Pulp UI
6. inspect resolved tokens and component scope
7. accept, reject, or partially accept the proposed token diff
8. save a style pack
9. reapply that style pack to the same or another compatible plugin/example
10. continue tweaking manually or with AI

This is the experience that should make a Claude Max subscriber immediately understand why the tool is useful.

The user should be able to watch a real Pulp example plugin change in front of them, inspect exactly what changed, and keep the result as plain versioned data.

The workflow should work at three scopes:

- primitive-level styling
- example/showcase-level styling
- real plugin-level styling

The last scope is the credibility test. If a user cannot take an actual plugin UI, restyle it materially, lock it in, and ship it, the tool is not done.

### Baseline Primitives To Ship

The AI Designer needs a real baseline component set or it becomes a token editor with nothing persuasive to style.

Pulp should define a shipped primitive set for the first designer milestone:

- knob
- vertical and horizontal slider
- button
- toggle
- ComboBox / dropdown
- tab bar / panel
- text input
- meter with ballistics
- waveform or scope view
- modulation overlay / cable or routing primitive

For audio UX, these are the minimum useful surface area. They are enough to demonstrate:

- shape and spacing tokens
- typography and label systems
- color and contrast systems
- interactive states
- motion and feedback
- audio-native styling patterns

These primitives can be visually inspired by strong reference systems, including Visage-style design language, but only under clean-room and license-safe rules:

- no copied source
- no copied APIs
- no copied assets unless the license explicitly permits reuse
- if visual motifs or example compositions are referenced, record the provenance
- use AI-driven variation and Pulp-native implementation to materially diverge from the reference

The goal is not “clone Visage.” The goal is “ship a compelling first-party primitive library that proves Pulp’s UI system and can evolve into its own design language.”

The baseline primitive pack should be used in at least:

- the standalone showcase
- one effect example
- one instrument example
- one more complex visual example with meters, scopes, or routing overlays

At least one of those targets should be treated as the shipping proof path:

- apply AI-generated or AI-assisted style changes
- save them as a style pack or plugin-owned token file
- rebuild or reload the plugin
- confirm the plugin ships with the locked-in style and no runtime dependency on the AI session

That gives the AI Designer a believable proving ground instead of a toy canvas.

### Where Assets Fit

There are two different “asset” problems here:

1. binary/runtime assets
- icons
- fonts
- images
- preset bundles
- sample token packs

2. AI Designer outputs
- token themes
- style snapshots
- showcase presets
- screenshots
- documentation visuals

Pulp already has some support around binary asset embedding and screenshot capture. That is not yet an AI Designer asset workflow.

The AI Designer workstream should define:

- how theme/token packs are stored in-repo
- how style snapshots are versioned
- what generated assets are committed versus ephemeral
- how screenshots are regenerated
- which artifacts are documentation inputs versus runtime assets

### What A First Real MVP Should Be

A credible first AI Designer MVP should be narrower than the full vision.

Recommended MVP:

- a stable token schema
- one showcase app or preview target
- live token reload
- inspector view of resolved tokens
- `pulp design` command that:
  - loads a target showcase or example
  - accepts a natural-language request
  - generates token edits locally through a configured AI tool
  - previews the result
  - allows accept/reject
  - saves a token snapshot
- style-pack reapplication across compatible examples/plugins

Do not make the MVP depend on:

- every widget class being finished
- every platform path being mature
- broad asset generation beyond theme/token outputs
- cloud-hosted infrastructure

Recommended MVP deliverables:

- one shipped primitive pack using the baseline components above
- one or two reference visual languages applied to the same primitive pack
- token export and import that supports hot reload cleanly
- a preview target that can be restyled repeatedly without manual rebuild steps
- one or more example plugins proving the same style pack can be applied beyond the showcase
- one explicit ship path showing how a plugin locks in a style for release

Recommended first proof targets:

- `PulpGain` or another simple effect example
- `PulpTone` or another simple instrument example
- one richer GPU-oriented example when the render path is ready

This matters because the feature only becomes impressive when a user sees:

- the same design language applied across multiple plugin types
- a saved style pack reloaded later
- manual edits and AI edits coexisting cleanly
- a plugin-specific style locked in and ready to ship

### Verification And Test Strategy

The AI Designer should have a dedicated verification plan from day one.

Unit / schema tests:

- token schema parsing
- token validation and defaulting
- token inheritance resolution
- export/import round-trips
- snapshot/history serialization
- deterministic token diff generation

Component / rendering tests:

- primitive components render correctly under a fixed token set
- interaction states respond correctly to token changes
- layout does not break when theme geometry changes
- exported token packs rehydrate to the same visual result

Hot-reload / tooling tests:

- editing tokens triggers deterministic preview updates
- `pulp design` applies token patches and persists accepted snapshots
- inspector shows resolved token values for selected nodes
- token export formats stay in sync across JSON/CSS/C++/shader outputs
- saved style packs reapply correctly to the original target
- compatible style packs apply predictably across multiple example plugins
- locking a style into a plugin-owned artifact survives reload/rebuild without drift

Visual regression tests:

- baseline screenshots for shipped primitive packs
- before/after token application screenshots
- tolerance-based comparison for intentional visual changes
- fixed camera, size, and seed inputs so results are reproducible locally
- locked-in plugin styles match the accepted preview output

AI workflow tests:

- the natural-language layer should be tested as a token-patch pipeline, not as an unbounded “did the AI vibe feel good” system
- verify that a request produces a structured token diff
- verify that the diff can be previewed, accepted, rejected, and replayed
- keep a deterministic test mode using canned model outputs or fixtures
- verify scoped edits only affect the selected target surface
- verify rollback and undo preserve a valid token graph
- verify accepted style changes can be committed as plugin-owned data with no hidden dependency on the generation session

This work should include a clear separation between:

- deterministic system tests
- subjective design review

The former decides whether the system works. The latter decides whether the resulting themes are good.

### Claude Max / Local AI Integration

The AI Designer should make immediate sense to a user with an existing Claude Max subscription.

That means:

- use local Claude CLI or MCP-style integration where possible
- do not require a separate hosted Pulp AI service for the core workflow
- treat the model as a generator of structured token diffs, not as the owner of the design state
- keep accepted output as plain local files

The trust model should be:

- AI proposes
- Pulp previews
- user inspects
- user accepts or rejects
- Pulp saves durable style data

The end state is not “the AI session remembers the design.”
The end state is “the plugin or app owns the design as data.”

If the AI service is unavailable, the style packs and manual tooling should still work.

### What To Explicitly Reject

The following should be rejected up front:

- copying Visage class structures or API names
- making `render` aware of widget semantics
- inheritance-heavy theme systems
- hidden nondeterministic AI edits with no artifact trail
- a web-first workflow that makes local agents call external services by default
- a “magic designer” with no clear token diff or snapshot history

### Validation Criteria

The AI Designer should not be called real until it has:

- deterministic token schema and serialization
- repeatable preview path
- saved snapshot/history artifacts
- clear CLI contract
- docs that explain what it edits and what it does not edit
- tests for token parsing, propagation, hot reload, and snapshot round-trips
- tests for shipped primitives under multiple token packs
- screenshot/preview regression coverage for the baseline primitive pack
- honest status/support labeling

If screenshots or visual previews are central to acceptance, they should be reproducible locally.

### Relationship To The Audit Loop

`planning/ralph-loop-prompt-7.md` should remain audit-first.

The correct relationship is:

- prompt 7 resets the repo to truth
- prompt 7 records the AI Designer as planned/deferred work honestly
- a later dedicated loop or spec-driven branch executes the AI Designer streams

This avoids mixing “what is true now” with “what we are about to build.”

### Native vs Web-Based Tooling

The designer can plausibly be exposed as a web-based tool, but the architecture should remain local-first.

Recommended model:

- core token/edit/preview logic lives in Pulp-native code and local artifacts
- `pulp design` is the canonical local entry point
- a web-facing shell is optional and can sit on top later, likely via WASM or a local preview server
- a browser or Tauri shell may be appropriate for sharing/review, but not as the primary design-authoring environment

This gives Pulp the best of both worlds:

- easy online/docs/demo presentation
- easy local agent consumption
- no requirement that design work depend on remote web calls

So the right framing is:

- not “browser-only design tool”
- not “native-only closed tool”
- instead: local-first design system with an optional web presentation layer

### Suggested Follow-Up Execution Plan

1. finish the status-truth pass and keep AI Designer marked as planned/deferred
2. create the four planning docs listed above
3. decide the token schema and inspector contract
4. implement the minimum substrate for token-driven previews
5. build the showcase and `pulp design` MVP
6. only then promote the status/docs language

### Bottom Line

Pulp should absolutely build the AI Designer.

But it should be built as a first-class, spec-backed workstream:

- architecture first
- product substrate second
- showcase/assets/design-session surface third

That sequencing preserves credibility and gives the feature a real path to becoming one of the strongest reasons to build on Pulp.
